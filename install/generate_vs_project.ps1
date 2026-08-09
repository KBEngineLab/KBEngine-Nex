[CmdletBinding()]
param(
    [string]$VisualStudioVersion = '',
    [string]$VcpkgRepository = $(if ($env:KBE_VCPKG_REPOSITORY) { $env:KBE_VCPKG_REPOSITORY } else { 'https://github.com/microsoft/vcpkg.git' }),
    [string]$VcpkgRef = $(if ($env:KBE_VCPKG_REF) { $env:KBE_VCPKG_REF } else { '2825cdd8fe079a9538032fd78c3102d033195a2c' })
)

$ErrorActionPreference = 'Stop'
$InstallRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$SourceRoot = Join-Path $InstallRoot 'kbe/src'
$VcpkgRoot = Join-Path $InstallRoot 'kbe/vcpkg'

function Require-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "未找到 $Name。请先安装 Git 和 CMake。"
    }
}

function Resolve-VSVersion {
    if ($VisualStudioVersion) {
        switch ($VisualStudioVersion) {
            '2022' { return '2022' }
            '2026' { return '2026' }
            default { throw "无效的 Visual Studio 版本: $VisualStudioVersion。请传入 2022 或 2026。" }
        }
    }

    if ($Host.UI -and $Host.UI.RawUI) {
        Write-Host '请选择要导出的 Visual Studio 版本：'
        Write-Host '  1) VS 2022'
        Write-Host '  2) VS 2026'
        $choice = Read-Host '输入 1 或 2'
        switch ($choice) {
            '1' { return '2022' }
            '2' { return '2026' }
            default { throw '无效选择，请输入 1 或 2。' }
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
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg 克隆失败。' }
    if ($VcpkgRef) { git -C $VcpkgRoot checkout $VcpkgRef }
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg 检出失败。' }
    & (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat')
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg 引导失败。' }
}

$env:VCPKG_ROOT = $VcpkgRoot
$vsVersion = Resolve-VSVersion
$preset = "windows-vs$vsVersion"

if ($vsVersion -eq '2026' -and -not (Test-CMakeSupportsVs2026)) {
    throw '当前 CMake 不支持 Visual Studio 18 2026 生成器，请升级 CMake/Visual Studio 后再导出 VS2026，或选择 VS2022。'
}

Push-Location $SourceRoot
try {
    & cmake --fresh --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "VS$vsVersion CMake 工程生成失败。请确认当前 CMake 和 Visual Studio 支持对应生成器。" }
} finally {
    Pop-Location
}

Write-Host "[SUCCESS] VS$vsVersion 工程已生成：kbe/src/out/build/windows-vs$vsVersion"
