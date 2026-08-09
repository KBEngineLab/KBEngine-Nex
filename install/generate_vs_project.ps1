[CmdletBinding()]
param(
    [string]$VisualStudioVersion = '',
    [string]$VcpkgRepository = $(if ($env:KBE_VCPKG_REPOSITORY) { $env:KBE_VCPKG_REPOSITORY } else { 'https://github.com/microsoft/vcpkg.git' }),
    [string]$VcpkgRef = $env:KBE_VCPKG_REF
)

$ErrorActionPreference = 'Stop'
$InstallRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$SourceRoot = Join-Path $InstallRoot 'kbe/src'
$VcpkgRoot = Join-Path $InstallRoot 'kbe/vcpkg'

function Require-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Command not found: $Name. Please install Git and CMake first."
    }
}

function Resolve-VSVersion {
    if ($VisualStudioVersion) {
        switch ($VisualStudioVersion) {
            '2022' { return '2022' }
            '2026' { return '2026' }
            default { throw "Invalid Visual Studio version: $VisualStudioVersion. Expected 2022 or 2026." }
        }
    }

    if ($Host.UI -and $Host.UI.RawUI) {
        Write-Host 'Select Visual Studio version to generate:'
        Write-Host '  1) VS 2022'
        Write-Host '  2) VS 2026'
        $choice = Read-Host 'Enter 1 or 2'
        switch ($choice) {
            '1' { return '2022' }
            '2' { return '2026' }
            default { throw 'Invalid choice. Expected 1 or 2.' }
        }
    }

    return '2022'
}

function Test-CMakeSupportsVs2026 {
    $help = & cmake --help 2>$null
    return $LASTEXITCODE -eq 0 -and ($help -match 'Visual Studio 18 2026')
}

Require-Command git
Require-Command cmake

if (-not (Test-Path (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
    git clone $VcpkgRepository $VcpkgRoot
    if ($LASTEXITCODE -ne 0) { throw 'Failed to clone vcpkg.' }
    if ($VcpkgRef) { git -C $VcpkgRoot checkout $VcpkgRef }
    if ($LASTEXITCODE -ne 0) { throw 'Failed to checkout vcpkg ref.' }
    & (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat')
    if ($LASTEXITCODE -ne 0) { throw 'Failed to bootstrap vcpkg.' }
}

$env:VCPKG_ROOT = $VcpkgRoot
$vsVersion = Resolve-VSVersion
$preset = "windows-vs$vsVersion"

if ($vsVersion -eq '2026' -and -not (Test-CMakeSupportsVs2026)) {
    throw 'Current CMake does not support the Visual Studio 18 2026 generator. Upgrade CMake/Visual Studio or choose VS2022.'
}

Push-Location $SourceRoot
try {
    & cmake --fresh --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "Failed to generate VS$vsVersion CMake project. Please confirm CMake and Visual Studio support the selected generator." }
} finally {
    Pop-Location
}

Write-Host "[SUCCESS] VS$vsVersion project generated: kbe/src/out/build/windows-vs$vsVersion"
