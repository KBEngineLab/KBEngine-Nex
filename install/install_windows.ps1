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

Require-Command git
Require-Command cmake

if (-not (Test-Path (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
    Write-Host "[INFO] Cloning vcpkg to $VcpkgRoot"
    git clone $VcpkgRepository $VcpkgRoot
    if ($VcpkgRef) { git -C $VcpkgRoot checkout $VcpkgRef }
    & (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat')
}

$env:VCPKG_ROOT = $VcpkgRoot
Write-Host "[INFO] VCPKG_ROOT=$env:VCPKG_ROOT"

Push-Location $SourceRoot
try {
    & cmake --fresh --preset windows-ninja
    if (-not $SkipBuild) {
        $target = if ($Configuration -eq 'Debug') { 'windows-ninja-debug' } else { 'windows-ninja-release' }
        & cmake --build --preset $target --target kbe_runtime kbe_servers kbe_tests --parallel
    }
} finally {
    Pop-Location
}

Write-Host '[SUCCESS] Windows CMake install/build completed'
