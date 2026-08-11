[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$VcpkgRepository = $(if ($env:KBE_VCPKG_REPOSITORY) { $env:KBE_VCPKG_REPOSITORY } else { 'https://github.com/microsoft/vcpkg.git' }),
    [string]$VcpkgRef = $env:KBE_VCPKG_REF,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$InstallRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$SourceRoot = Join-Path $InstallRoot 'kbe/src'
$VcpkgRoot = Join-Path $InstallRoot 'kbe/vcpkg'

function Require-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Command not found: $Name. Please install Git, CMake and Ninja, then run this script from a Developer PowerShell."
    }
}

function Assert-NativeCommandSucceeded {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Description,

        [int]$ExitCode
    )

    # PowerShell 5 does not promote a native process failure to a terminating error, so
    # callers capture the exit code immediately or CI may continue with missing artifacts.
    if ($ExitCode -ne 0) {
        throw "$Description failed with exit code $ExitCode."
    }
}

function Wait-ForKeyPress {
    if ($env:CI) {
        return
    }

    if ([Console]::IsInputRedirected -or [Console]::IsOutputRedirected) {
        return
    }

    Write-Host 'Press any key to exit...'
    [void][Console]::ReadKey($true)
}

Require-Command git
Require-Command cmake

if (-not (Test-Path (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
    Write-Host "[INFO] Cloning vcpkg to $VcpkgRoot"
    git clone $VcpkgRepository $VcpkgRoot
    Assert-NativeCommandSucceeded -Description 'git clone vcpkg' -ExitCode $LASTEXITCODE
    if ($VcpkgRef) {
        git -C $VcpkgRoot checkout $VcpkgRef
        Assert-NativeCommandSucceeded -Description 'git checkout vcpkg revision' -ExitCode $LASTEXITCODE
    }
    & (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat')
    Assert-NativeCommandSucceeded -Description 'vcpkg bootstrap' -ExitCode $LASTEXITCODE
}

$env:VCPKG_ROOT = $VcpkgRoot
Write-Host "[INFO] VCPKG_ROOT=$env:VCPKG_ROOT"

Push-Location $SourceRoot
try {
    & cmake --fresh --preset windows-ninja
    Assert-NativeCommandSucceeded -Description 'CMake configure' -ExitCode $LASTEXITCODE
    if (-not $SkipBuild) {
        $buildPreset = if ($Configuration -eq 'Debug') { 'windows-ninja-tests-debug' } else { 'windows-ninja-tests-release' }
        & cmake --build --preset $buildPreset --target kbe_servers kbe_tests --parallel
        Assert-NativeCommandSucceeded -Description 'CMake build' -ExitCode $LASTEXITCODE
    }
} finally {
    Pop-Location
}

Write-Host '[SUCCESS] Task completed.'
Write-Host '[OUTPUT] CMake build directory: kbe/src/out/build/windows-ninja-multi'
Write-Host '[OUTPUT] Runtime directory: kbe/bin/server'
Wait-ForKeyPress
