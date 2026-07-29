[CmdletBinding()]
param(
    [string] $KbcmdPath,
    [string] $MSBuildPath = "E:/ProgramFiles/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../../../..")).Path
$pythonAssets = (Resolve-Path (Join-Path $repoRoot "kbe/res/sdk_templates/server/python_assets")).Path
$numericProject = (Resolve-Path (Join-Path $PSScriptRoot "cxx_numeric_boundary/CxxNumericBoundaryTest.vcxproj")).Path
$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("kbe-cxx-sdk-boundaries-" + [Guid]::NewGuid().ToString("N"))
$numericOutput = Join-Path $temporaryRoot "numeric/"
$numericIntermediate = Join-Path $temporaryRoot "numeric-obj/"
$numericExecutable = Join-Path $numericOutput "CxxNumericBoundaryTest.exe"

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

function Invoke-ErrorIdCase {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Name,

        [Parameter(Mandatory = $true)]
        [int] $ErrorId,

        [Parameter(Mandatory = $true)]
        [bool] $ShouldPass
    )

    $caseRoot = Join-Path $temporaryRoot $Name
    $customResourceRoot = Join-Path $caseRoot "custom-res"
    $serverResourceRoot = Join-Path $customResourceRoot "server"
    $outputPath = Join-Path $caseRoot "generated"
    [System.IO.Directory]::CreateDirectory($serverResourceRoot) | Out-Null

    $xml = @"
<root>
    <BOUNDARY_$($Name.ToUpperInvariant())>
        <id>$ErrorId</id>
        <descr>SDK boundary test.</descr>
    </BOUNDARY_$($Name.ToUpperInvariant())>
</root>
"@
    [System.IO.File]::WriteAllText(
        (Join-Path $serverResourceRoot "server_errors.xml"),
        $xml,
        [System.Text.UTF8Encoding]::new($false))

    $env:KBE_RES_PATH = "$repoRoot/kbe/res;$customResourceRoot;$pythonAssets;$pythonAssets/scripts;$pythonAssets/res"
    $processOutput = @(& $KbcmdPath --clientsdk=cxx "--outpath=$outputPath" 2>&1)
    $exitCode = $LASTEXITCODE
    $errorHeader = Join-Path $outputPath "ServerErrorDescrs.h"

    if ($ShouldPass) {
        Assert-Condition ($exitCode -eq 0) "The valid error ID $ErrorId failed generation: $($processOutput -join [Environment]::NewLine)"
        Assert-Condition (Test-Path -LiteralPath $errorHeader) "The valid error ID $ErrorId did not generate ServerErrorDescrs.h."
        $headerBody = Get-Content -LiteralPath $errorHeader -Raw -Encoding UTF8
        Assert-Condition ($headerBody -match "e\.id = $ErrorId;") "The valid upper-bound error ID was not preserved."
        Assert-Condition ($headerBody -match "serverErrs_\.Add\(static_cast<uint16>\(e\.id\), e\)") "The generated error table lost its explicit uint16 conversion."
    }
    else {
        Assert-Condition ($exitCode -ne 0) "The invalid error ID $ErrorId was accepted."
        Assert-Condition (-not (Test-Path -LiteralPath $errorHeader)) "The invalid error ID $ErrorId produced a truncated error table."
    }
}

$oldKbeRoot = $env:KBE_ROOT
$oldKbeResPath = $env:KBE_RES_PATH
$oldKbeBinPath = $env:KBE_BIN_PATH

try {
    Assert-Condition (Test-Path -LiteralPath $KbcmdPath -PathType Leaf) "kbcmd.exe was not found at $KbcmdPath."
    Assert-Condition (Test-Path -LiteralPath $MSBuildPath -PathType Leaf) "MSBuild.exe was not found at $MSBuildPath."
    [System.IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null

    # fixture 输出放入本轮临时目录，CTest 和手动执行都不会重新污染源码树。
    # Fixture output stays in this run's temporary directory so neither CTest nor manual execution repollutes the source tree.
    & $MSBuildPath $numericProject /m:1 /nr:false /t:Rebuild /p:Configuration=Release /p:Platform=x64 `
        /p:PlatformToolset=v143 "/p:OutDir=$numericOutput" "/p:IntDir=$numericIntermediate" /v:minimal
    Assert-Condition ($LASTEXITCODE -eq 0) "The C++ numeric boundary fixture failed to build."
    $numericOutput = @(& $numericExecutable 2>&1)
    Assert-Condition ($LASTEXITCODE -eq 0) "The C++ numeric boundary fixture failed: $($numericOutput -join [Environment]::NewLine)"
    Assert-Condition (($numericOutput -join [Environment]::NewLine) -match "CXX_NUMERIC_BOUNDARY_TEST_PASS") "The C++ numeric boundary pass marker is missing."

    $env:KBE_ROOT = $repoRoot
    $env:KBE_BIN_PATH = Join-Path $repoRoot "kbe/bin/server"
    Invoke-ErrorIdCase -Name "valid-max" -ErrorId 65535 -ShouldPass $true
    Invoke-ErrorIdCase -Name "invalid-overflow" -ErrorId 65536 -ShouldPass $false
    Invoke-ErrorIdCase -Name "invalid-negative" -ErrorId -1 -ShouldPass $false

    Write-Output "CXX_SDK_BOUNDARY_TEST_PASS numeric=true error-id-max=true overflow-rejected=true negative-rejected=true"
}
finally {
    $env:KBE_ROOT = $oldKbeRoot
    $env:KBE_RES_PATH = $oldKbeResPath
    $env:KBE_BIN_PATH = $oldKbeBinPath

    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
