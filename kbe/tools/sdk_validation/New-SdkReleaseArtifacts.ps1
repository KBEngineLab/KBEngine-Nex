[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $AssetsPath,

    [Parameter(Mandatory = $true)]
    [string] $OutputPath,

    [string] $KbcmdPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$assetsFullPath = (Resolve-Path -LiteralPath $AssetsPath).Path
$outputFullPath = [System.IO.Path]::GetFullPath($OutputPath)
$supportedSdks = [ordered] @{
    csharp = @(".cs", ".md", ".asmdef", ".meta", ".jslib")
    cxx = @(".cpp", ".h", ".c", ".md")
    typescript = @(".ts")
    gdscript = @(".gd")
}
$forbiddenSegments = @("bin", "obj", "node_modules", ".git", ".vs", ".godot", "__pycache__", "debug", "release", "x64")
$forbiddenExtensions = @(".dll", ".exe", ".lib", ".pdb", ".ilk", ".obj", ".o", ".a", ".so", ".dylib", ".pyc", ".cache", ".csproj", ".props", ".targets")

if ([string]::IsNullOrWhiteSpace($KbcmdPath)) {
    $KbcmdPath = Join-Path $repoRoot "kbe/bin/server/kbcmd.exe"
}
$kbcmdFullPath = (Resolve-Path -LiteralPath $KbcmdPath).Path

function Assert-Condition {
    param(
        [Parameter(Mandatory = $true)]
        [bool] $Condition,

        [Parameter(Mandatory = $true)]
        [string] $Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Get-UniqueGeneratedValue {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.FileInfo[]] $Files,

        [Parameter(Mandatory = $true)]
        [string] $Pattern,

        [Parameter(Mandatory = $true)]
        [string] $Label
    )

    $values = [System.Collections.Generic.List[string]]::new()
    foreach ($file in $Files) {
        $body = [System.IO.File]::ReadAllText($file.FullName)
        foreach ($match in [regex]::Matches($body, $Pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
            $values.Add($match.Groups["value"].Value)
        }
    }

    $uniqueValues = @($values | Sort-Object -Unique)
    Assert-Condition ($uniqueValues.Count -eq 1) "Generated SDK metadata '$Label' must have exactly one value, found: $($uniqueValues -join ', ')."
    return $uniqueValues[0]
}

function Get-Sha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

# 发布工具只接受空目录，不替调用方清理路径，从边界上避免误删已有版本或用户文件。
# The release tool accepts only an empty directory and never cleans caller paths, preventing deletion of existing releases or user files.
if (Test-Path -LiteralPath $outputFullPath) {
    $existingItems = @(Get-ChildItem -LiteralPath $outputFullPath -Force)
    Assert-Condition ($existingItems.Count -eq 0) "Release output must be a new or empty directory: $outputFullPath"
}
else {
    [System.IO.Directory]::CreateDirectory($outputFullPath) | Out-Null
}

$oldKbeRoot = $env:KBE_ROOT
$oldKbeResPath = $env:KBE_RES_PATH
$oldKbeBinPath = $env:KBE_BIN_PATH
$sdkManifests = [System.Collections.Generic.List[object]]::new()
$releaseMetadata = $null

try {
    $env:KBE_ROOT = $repoRoot
    $env:KBE_RES_PATH = "$repoRoot/kbe/res;$assetsFullPath;$assetsFullPath/scripts;$assetsFullPath/res"
    $env:KBE_BIN_PATH = Join-Path $repoRoot "kbe/bin/server"

    foreach ($sdkName in $supportedSdks.Keys) {
        $sdkPath = Join-Path $outputFullPath $sdkName
        $generationOutput = @(& $kbcmdFullPath "--clientsdk=$sdkName" "--outpath=$sdkPath" 2>&1)
        $generationText = $generationOutput -join [Environment]::NewLine
        Assert-Condition ($LASTEXITCODE -eq 0) "SDK '$sdkName' generation failed: $generationText"

        $toolMatch = [regex]::Match(
            $generationText,
            "tool Version:\s*(?<version>[0-9A-Za-z._-]+)\.\s*ScriptVersion:\s*(?<scriptVersion>[0-9A-Za-z._-]+)\..*?Protocol:\s*(?<protocol>[A-Fa-f0-9]{32})\.",
            [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        Assert-Condition $toolMatch.Success "SDK '$sdkName' generation output did not contain version and protocol metadata."

        $files = @(Get-ChildItem -LiteralPath $sdkPath -Recurse -File | Sort-Object FullName)
        Assert-Condition ($files.Count -gt 0) "SDK '$sdkName' generated no files."
        $fileEntries = [System.Collections.Generic.List[object]]::new()

        foreach ($file in $files) {
            $relativePath = [System.IO.Path]::GetRelativePath($sdkPath, $file.FullName).Replace("\", "/")
            $segments = @($relativePath.Split("/", [System.StringSplitOptions]::RemoveEmptyEntries) | ForEach-Object { $_.ToLowerInvariant() })
            $extension = $file.Extension.ToLowerInvariant()

            Assert-Condition (-not ($segments | Where-Object { $_ -in $forbiddenSegments })) "SDK '$sdkName' contains a forbidden build/cache directory: $relativePath"
            Assert-Condition ($extension -notin $forbiddenExtensions) "SDK '$sdkName' contains a forbidden binary/build artifact: $relativePath"
            Assert-Condition ($extension -in $supportedSdks[$sdkName]) "SDK '$sdkName' contains an unexpected file type '$extension': $relativePath"

            $fileEntries.Add([ordered] @{
                path = $relativePath
                bytes = $file.Length
                sha256 = Get-Sha256 -Path $file.FullName
            })
        }

        # 元数据同时从生成器输出与四端源码解析，防止混用旧目录时只看一侧仍得到假一致。
        # Metadata is parsed from both generator output and each SDK source so stale mixed directories cannot appear consistent from only one side.
        $sourceFiles = @($files | Where-Object { $_.Extension.ToLowerInvariant() -in @(".cs", ".cpp", ".h", ".ts", ".gd") })
        $metadata = [ordered] @{
            engineVersion = Get-UniqueGeneratedValue -Files $sourceFiles -Pattern 'clientVersion[^\r\n]*?"(?<value>[0-9A-Za-z._-]+)"' -Label "engineVersion"
            scriptVersion = Get-UniqueGeneratedValue -Files $sourceFiles -Pattern 'clientScriptVersion[^\r\n]*?"(?<value>[0-9A-Za-z._-]+)"' -Label "scriptVersion"
            protocolDigest = (Get-UniqueGeneratedValue -Files $sourceFiles -Pattern 'serverProtocolMD5[^\r\n]*?"(?<value>[A-Fa-f0-9]{32})"' -Label "protocolDigest").ToUpperInvariant()
            entityDefDigest = (Get-UniqueGeneratedValue -Files $sourceFiles -Pattern 'serverEntitydefMD5[^\r\n]*?"(?<value>[A-Fa-f0-9]{32})"' -Label "entityDefDigest").ToUpperInvariant()
        }

        Assert-Condition ($metadata.engineVersion -eq $toolMatch.Groups["version"].Value) "SDK '$sdkName' engine version differs between generator output and generated source."
        Assert-Condition ($metadata.scriptVersion -eq $toolMatch.Groups["scriptVersion"].Value) "SDK '$sdkName' script version differs between generator output and generated source."
        Assert-Condition ($metadata.protocolDigest -eq $toolMatch.Groups["protocol"].Value.ToUpperInvariant()) "SDK '$sdkName' protocol digest differs between generator output and generated source."

        if ($null -eq $releaseMetadata) {
            $releaseMetadata = $metadata
        }
        else {
            foreach ($metadataName in @("engineVersion", "scriptVersion", "protocolDigest", "entityDefDigest")) {
                Assert-Condition ($metadata[$metadataName] -eq $releaseMetadata[$metadataName]) "SDK '$sdkName' metadata '$metadataName' differs from the other release SDKs."
            }
        }

        # 路径、长度和文件摘要按固定顺序组成 tree 输入，使相同源码得到稳定的发布摘要。
        # Paths, lengths, and file hashes form a canonical ordered tree input so identical sources produce a stable release digest.
        $treeInput = ($fileEntries | ForEach-Object { "$($_.path)`t$($_.bytes)`t$($_.sha256)" }) -join "`n"
        $treeHashBytes = [System.Security.Cryptography.SHA256]::HashData([System.Text.Encoding]::UTF8.GetBytes($treeInput))
        $sdkManifests.Add([ordered] @{
            name = $sdkName
            directory = $sdkName
            fileCount = $fileEntries.Count
            totalBytes = [long] (($files | Measure-Object Length -Sum).Sum)
            treeSha256 = [System.Convert]::ToHexString($treeHashBytes)
            files = @($fileEntries)
        })
    }

    $releaseTreeInput = @(
        "engineVersion`t$($releaseMetadata.engineVersion)"
        "scriptVersion`t$($releaseMetadata.scriptVersion)"
        "protocolDigest`t$($releaseMetadata.protocolDigest)"
        "entityDefDigest`t$($releaseMetadata.entityDefDigest)"
        $sdkManifests | ForEach-Object { "$($_.name)`t$($_.treeSha256)" }
    ) -join "`n"
    $releaseTreeBytes = [System.Security.Cryptography.SHA256]::HashData([System.Text.Encoding]::UTF8.GetBytes($releaseTreeInput))

    $manifest = [ordered] @{
        schemaVersion = 1
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
        generator = [ordered] @{
            sha256 = Get-Sha256 -Path $kbcmdFullPath
        }
        metadata = $releaseMetadata
        releaseTreeSha256 = [System.Convert]::ToHexString($releaseTreeBytes)
        sdks = @($sdkManifests)
    }
    $manifestPath = Join-Path $outputFullPath "sdk-release-manifest.json"
    [System.IO.File]::WriteAllText(
        $manifestPath,
        ($manifest | ConvertTo-Json -Depth 32),
        [System.Text.UTF8Encoding]::new($false))

    Write-Output "SDK_RELEASE_ARTIFACTS_PASS sdks=4 version=$($releaseMetadata.engineVersion) protocol=$($releaseMetadata.protocolDigest) entitydef=$($releaseMetadata.entityDefDigest) manifest=$manifestPath"
}
finally {
    $env:KBE_ROOT = $oldKbeRoot
    $env:KBE_RES_PATH = $oldKbeResPath
    $env:KBE_BIN_PATH = $oldKbeBinPath
}
