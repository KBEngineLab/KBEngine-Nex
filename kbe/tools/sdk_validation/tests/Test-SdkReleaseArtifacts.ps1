[CmdletBinding()]
param(
    [string] $KbcmdPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../../../..")).Path
$releaseTool = (Resolve-Path (Join-Path $PSScriptRoot "../New-SdkReleaseArtifacts.ps1")).Path
$pythonAssets = (Resolve-Path (Join-Path $repoRoot "kbe/res/sdk_templates/server/python_assets")).Path
$clientTemplateRoot = (Resolve-Path (Join-Path $repoRoot "kbe/res/sdk_templates/client")).Path
$pwsh = (Get-Command pwsh -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("kbe-sdk-release-artifacts-" + [Guid]::NewGuid().ToString("N"))
$releasePath = Join-Path $temporaryRoot "release"
$repeatReleasePath = Join-Path $temporaryRoot "release-repeat"
$expectedSdks = @("csharp", "cxx", "gdscript", "typescript")

if ([string]::IsNullOrWhiteSpace($KbcmdPath)) {
    $KbcmdPath = Join-Path $repoRoot "kbe/bin/server/kbcmd.exe"
}

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

$oldKbeRoot = $env:KBE_ROOT
$oldKbeResPath = $env:KBE_RES_PATH
$oldKbeBinPath = $env:KBE_BIN_PATH

try {
    Assert-Condition (Test-Path -LiteralPath $KbcmdPath -PathType Leaf) "kbcmd.exe was not found at $KbcmdPath."
    [System.IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null

    $templateNames = @(Get-ChildItem -LiteralPath $clientTemplateRoot -Directory | ForEach-Object { $_.Name.ToLowerInvariant() } | Sort-Object)
    Assert-Condition (($templateNames -join ",") -eq ($expectedSdks -join ",")) "Client template directories are not limited to the four release SDKs: $($templateNames -join ', ')."
    foreach ($legacyGenerator in @("client_sdk_unity.cpp", "client_sdk_unity.h", "client_sdk_ue4.cpp", "client_sdk_ue4.h")) {
        Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $repoRoot "kbe/src/server/tools/kbcmd/$legacyGenerator"))) "Legacy SDK generator source remains: $legacyGenerator"
    }

    $releaseOutput = @(& $pwsh -NoProfile -File $releaseTool -AssetsPath $pythonAssets -OutputPath $releasePath -KbcmdPath $KbcmdPath 2>&1)
    Assert-Condition ($LASTEXITCODE -eq 0) "Release artifact generation failed: $($releaseOutput -join [Environment]::NewLine)"
    Assert-Condition (($releaseOutput -join [Environment]::NewLine) -match "SDK_RELEASE_ARTIFACTS_PASS") "The release artifact pass marker is missing."

    $manifestPath = Join-Path $releasePath "sdk-release-manifest.json"
    $manifestBody = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8
    $manifest = $manifestBody | ConvertFrom-Json -Depth 32
    $manifestSdkNames = @($manifest.sdks | ForEach-Object { $_.name } | Sort-Object)
    Assert-Condition (($manifestSdkNames -join ",") -eq ($expectedSdks -join ",")) "The release manifest does not contain exactly four supported SDKs."
    Assert-Condition ($manifestBody -notmatch "[A-Za-z]:[\\/]") "The release manifest leaked a local absolute path."

    $releaseTreeLines = [System.Collections.Generic.List[string]]::new()
    $releaseTreeLines.Add("engineVersion`t$($manifest.metadata.engineVersion)")
    $releaseTreeLines.Add("scriptVersion`t$($manifest.metadata.scriptVersion)")
    $releaseTreeLines.Add("protocolDigest`t$($manifest.metadata.protocolDigest)")
    $releaseTreeLines.Add("entityDefDigest`t$($manifest.metadata.entityDefDigest)")

    foreach ($sdk in $manifest.sdks) {
        Assert-Condition ($sdk.fileCount -eq $sdk.files.Count -and $sdk.fileCount -gt 0) "SDK '$($sdk.name)' has an invalid file count."
        $treeLines = [System.Collections.Generic.List[string]]::new()
        foreach ($fileEntry in $sdk.files) {
            Assert-Condition (-not [System.IO.Path]::IsPathRooted($fileEntry.path)) "SDK '$($sdk.name)' manifest contains an absolute file path."
            Assert-Condition ($fileEntry.path -notmatch '(^|/)\.\.(/|$)') "SDK '$($sdk.name)' manifest contains a parent path segment."
            $filePath = Join-Path (Join-Path $releasePath $sdk.directory) $fileEntry.path
            Assert-Condition (Test-Path -LiteralPath $filePath -PathType Leaf) "Manifest file is missing: $($sdk.name)/$($fileEntry.path)"
            $actualHash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToUpperInvariant()
            Assert-Condition ($actualHash -eq $fileEntry.sha256) "Manifest hash differs for $($sdk.name)/$($fileEntry.path)."
            $treeLines.Add("$($fileEntry.path)`t$($fileEntry.bytes)`t$($fileEntry.sha256)")
        }

        $treeInput = $treeLines -join "`n"
        $treeBytes = [System.Security.Cryptography.SHA256]::HashData([System.Text.Encoding]::UTF8.GetBytes($treeInput))
        Assert-Condition ([System.Convert]::ToHexString($treeBytes) -eq $sdk.treeSha256) "SDK '$($sdk.name)' tree digest differs from its file manifest."
        $releaseTreeLines.Add("$($sdk.name)`t$($sdk.treeSha256)")
    }

    $releaseTreeBytes = [System.Security.Cryptography.SHA256]::HashData([System.Text.Encoding]::UTF8.GetBytes($releaseTreeLines -join "`n"))
    Assert-Condition ([System.Convert]::ToHexString($releaseTreeBytes) -eq $manifest.releaseTreeSha256) "The root release digest differs from the four SDK trees."

    # 在独立空目录重复生成并比较根摘要，确保时间戳之外的发布内容可重复。
    # Generate again in an independent empty directory and compare root digests so release content remains reproducible apart from timestamps.
    $repeatOutput = @(& $pwsh -NoProfile -File $releaseTool -AssetsPath $pythonAssets -OutputPath $repeatReleasePath -KbcmdPath $KbcmdPath 2>&1)
    Assert-Condition ($LASTEXITCODE -eq 0) "Repeated release artifact generation failed: $($repeatOutput -join [Environment]::NewLine)"
    $repeatManifest = Get-Content -LiteralPath (Join-Path $repeatReleasePath "sdk-release-manifest.json") -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 32
    Assert-Condition ($repeatManifest.releaseTreeSha256 -eq $manifest.releaseTreeSha256) "Repeated generation produced a different root release digest."

    $env:KBE_ROOT = $repoRoot
    $env:KBE_RES_PATH = "$repoRoot/kbe/res;$pythonAssets;$pythonAssets/scripts;$pythonAssets/res"
    $env:KBE_BIN_PATH = Join-Path $repoRoot "kbe/bin/server"
    foreach ($legacySdk in @("unity", "ue4", "js", "javascript")) {
        $legacyOutputPath = Join-Path $temporaryRoot "legacy-$legacySdk"
        $legacyOutput = @(& $KbcmdPath "--clientsdk=$legacySdk" "--outpath=$legacyOutputPath" 2>&1)
        Assert-Condition ($LASTEXITCODE -ne 0) "Legacy SDK '$legacySdk' was unexpectedly accepted: $($legacyOutput -join [Environment]::NewLine)"
    }

    $nonEmptyPath = Join-Path $temporaryRoot "non-empty"
    [System.IO.Directory]::CreateDirectory($nonEmptyPath) | Out-Null
    $sentinelPath = Join-Path $nonEmptyPath "sentinel.txt"
    [System.IO.File]::WriteAllText($sentinelPath, "preserve", [System.Text.UTF8Encoding]::new($false))
    $nonEmptyOutput = @(& $pwsh -NoProfile -File $releaseTool -AssetsPath $pythonAssets -OutputPath $nonEmptyPath -KbcmdPath $KbcmdPath 2>&1)
    Assert-Condition ($LASTEXITCODE -ne 0) "The release tool accepted a non-empty output directory."
    Assert-Condition ((Get-Content -LiteralPath $sentinelPath -Raw) -eq "preserve") "The release tool modified a pre-existing output directory."

    Write-Output "SDK_RELEASE_ARTIFACT_TEST_PASS sdks=4 hashes=true tree-hashes=true root-hash=true reproducible=true legacy-rejected=true clean-output=true"
}
finally {
    $env:KBE_ROOT = $oldKbeRoot
    $env:KBE_RES_PATH = $oldKbeResPath
    $env:KBE_BIN_PATH = $oldKbeBinPath

    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
