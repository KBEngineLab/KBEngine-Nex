[CmdletBinding()]
param(
    [string]$VcpkgRepository = $(if ($env:KBE_VCPKG_REPOSITORY) { $env:KBE_VCPKG_REPOSITORY } else { 'https://github.com/microsoft/vcpkg.git' }),
    [string]$VcpkgRef = $(if ($env:KBE_VCPKG_REF) { $env:KBE_VCPKG_REF } else { '2825cdd8fe079a9538032fd78c3102d033195a2c' })
)

$ErrorActionPreference = 'Stop'
$InstallRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$SourceRoot = Join-Path $InstallRoot 'kbe/src'
$VcpkgRoot = Join-Path $InstallRoot 'kbe/vcpkg'

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw '未找到 Git。' }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw '未找到 CMake。' }

if (-not (Test-Path (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
    git clone $VcpkgRepository $VcpkgRoot
    if ($VcpkgRef) { git -C $VcpkgRoot checkout $VcpkgRef }
    & (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat')
}

$env:VCPKG_ROOT = $VcpkgRoot
Push-Location $SourceRoot
try {
    & cmake --fresh --preset windows-vs2022
    if ( -ne 0) { throw 'VS2022 CMake 工程生成失败。' }
} finally {
    Pop-Location
}

Write-Host '[SUCCESS] VS2022 工程已生成：kbe/src/out/build/windows-vs2022'