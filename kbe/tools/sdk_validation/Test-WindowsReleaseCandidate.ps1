[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $AssetsPath,

    [Parameter(Mandatory = $true)]
    [string] $OutputPath,

    [string] $KbcmdPath,

    [string] $MSBuildPath = "E:/ProgramFiles/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe",

    [string] $GodotPath = "D:/ProgramFiles/Godot/Godot_v4.7-stable_win64/Godot_v4.7-stable_win64_console.exe",

    [string] $TypeScriptPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$releaseTool = (Resolve-Path (Join-Path $PSScriptRoot "New-SdkReleaseArtifacts.ps1")).Path
$assetsFullPath = (Resolve-Path -LiteralPath $AssetsPath).Path
$outputFullPath = [System.IO.Path]::GetFullPath($OutputPath)
$msbuildFullPath = (Resolve-Path -LiteralPath $MSBuildPath).Path
$godotFullPath = (Resolve-Path -LiteralPath $GodotPath).Path
$runId = [Guid]::NewGuid().ToString("N")
$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("kbe-windows-release-candidate-" + $runId)
# MSBuild 会对位于系统 TEMP 下的 C++ 中间目录固定报告 MSB8029，因此 C++ 使用仓库已忽略且会精确清理的 logs 工作区。
# MSBuild always reports MSB8029 for C++ intermediates under system TEMP, so C++ uses an ignored repository logs workspace that is cleaned precisely.
$cxxTemporaryRoot = Join-Path $repoRoot ("logs/sdk-validation-cxx-" + $runId)

if ([string]::IsNullOrWhiteSpace($KbcmdPath)) {
    $KbcmdPath = Join-Path $repoRoot "kbe/bin/server/kbcmd.exe"
}
$kbcmdFullPath = (Resolve-Path -LiteralPath $KbcmdPath).Path

if ([string]::IsNullOrWhiteSpace($TypeScriptPath)) {
    $TypeScriptPath = Join-Path $assetsFullPath "kbeclient/typescript-component-e2e/node_modules/.bin/tsc.cmd"
}
$typescriptFullPath = (Resolve-Path -LiteralPath $TypeScriptPath).Path
$nodeModulesPath = Split-Path (Split-Path $typescriptFullPath -Parent) -Parent
$typescriptModulePath = (Resolve-Path (Join-Path $nodeModulesPath "typescript/lib/typescript.js")).Path

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

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Label,

        [Parameter(Mandatory = $true)]
        [string] $FilePath,

        [Parameter(Mandatory = $true)]
        [string[]] $Arguments,

        [string[]] $ForbiddenPatterns = @()
    )

    $output = @(& $FilePath @Arguments 2>&1)
    $text = $output -join [Environment]::NewLine
    Assert-Condition ($LASTEXITCODE -eq 0) "$Label failed: $text"
    foreach ($pattern in $ForbiddenPatterns) {
        Assert-Condition ($text -notmatch $pattern) "$Label matched forbidden output '$pattern': $text"
    }

    return $text
}

try {
    [System.IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null

    # 候选包由一个生成器和一份 EntityDef 一次性生成；编译工程全部位于隔离工作目录，不能污染发布摘要。
    # One generator and one EntityDef produce the complete candidate; all build projects stay in isolated work directories and cannot contaminate release hashes.
    $releaseOutput = @(
        & pwsh -NoProfile -File $releaseTool -AssetsPath $assetsFullPath -OutputPath $outputFullPath -KbcmdPath $kbcmdFullPath 2>&1
    )
    Assert-Condition ($LASTEXITCODE -eq 0) "Release candidate generation failed: $($releaseOutput -join [Environment]::NewLine)"

    $manifestPath = Join-Path $outputFullPath "sdk-release-manifest.json"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 32
    Assert-Condition ($manifest.sdks.Count -eq 4) "The release candidate does not contain exactly four SDKs."

    # C# 使用显式源码集合，项目与 bin/obj 均不进入候选目录；warning-as-error 是最终发布硬门禁。
    # C# uses an explicit source set while the project and bin/obj stay outside the candidate; warning-as-error is a hard release gate.
    $csharpProjectPath = Join-Path $temporaryRoot "csharp/ReleaseCandidate.csproj"
    [System.IO.Directory]::CreateDirectory((Split-Path $csharpProjectPath -Parent)) | Out-Null
    $csharpSourceGlob = [System.Security.SecurityElement]::Escape((Join-Path $outputFullPath "csharp/**/*.cs"))
    $csharpProject = @"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <Nullable>disable</Nullable>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
    <TreatWarningsAsErrors>true</TreatWarningsAsErrors>
  </PropertyGroup>
  <ItemGroup>
    <Compile Include="$csharpSourceGlob" />
  </ItemGroup>
</Project>
"@
    [System.IO.File]::WriteAllText($csharpProjectPath, $csharpProject, [System.Text.UTF8Encoding]::new($false))
    Invoke-CheckedCommand -Label "C# Release build" -FilePath "dotnet" -Arguments @(
        "build", $csharpProjectPath, "-c", "Release", "--nologo"
    ) -ForbiddenPatterns @('(?:warning|警告)\s+[A-Z]+[0-9]+:') | Out-Null

    # C++ 编译为静态库以覆盖全部翻译单元而不引入测试 main；/W4 与 /WX 同时防止警告回归。
    # C++ builds as a static library to cover every translation unit without a test main; /W4 and /WX jointly prevent warning regressions.
    $cxxProjectPath = Join-Path $cxxTemporaryRoot "ReleaseCandidate.vcxproj"
    [System.IO.Directory]::CreateDirectory((Split-Path $cxxProjectPath -Parent)) | Out-Null
    $cxxPath = Join-Path $outputFullPath "cxx"
    $cxxIncludePath = [System.Security.SecurityElement]::Escape($cxxPath)
    $cxxItems = Get-ChildItem -LiteralPath $cxxPath -File |
        Where-Object { $_.Extension -in @(".cpp", ".c") -and $_.Name -ne "UKBEMain.cpp" } |
        Sort-Object Name |
        ForEach-Object { '    <ClCompile Include="' + [System.Security.SecurityElement]::Escape($_.FullName) + '" />' }
    $cxxProject = @"
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals"><ProjectGuid>{63CB731B-E6CF-4E45-A17E-6E679192659B}</ProjectGuid><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion></PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'`$(Configuration)|`$(Platform)'=='Release|x64'" Label="Configuration"><ConfigurationType>StaticLibrary</ConfigurationType><UseDebugLibraries>false</UseDebugLibraries><PlatformToolset>v143</PlatformToolset></PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.props" />
  <ItemDefinitionGroup Condition="'`$(Configuration)|`$(Platform)'=='Release|x64'">
    <ClCompile><WarningLevel>Level4</WarningLevel><TreatWarningAsError>true</TreatWarningAsError><LanguageStandard>stdcpp17</LanguageStandard><AdditionalIncludeDirectories>$cxxIncludePath;$cxxIncludePath\blowfish;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories><PreprocessorDefinitions>WIN32_LEAN_AND_MEAN;NOMINMAX;_CRT_SECURE_NO_WARNINGS;%(PreprocessorDefinitions)</PreprocessorDefinitions><AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions></ClCompile>
  </ItemDefinitionGroup>
  <ItemGroup>
$($cxxItems -join [Environment]::NewLine)
  </ItemGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
"@
    [System.IO.File]::WriteAllText($cxxProjectPath, $cxxProject, [System.Text.UTF8Encoding]::new($false))
    Invoke-CheckedCommand -Label "C++ Release build" -FilePath $msbuildFullPath -Arguments @(
        $cxxProjectPath, "/m:1", "/nr:false", "/t:Rebuild", "/p:Configuration=Release", "/p:Platform=x64", "/p:PlatformToolset=v143", "/v:minimal"
    ) -ForbiddenPatterns @('(?:warning|警告)\s+[A-Z]+[0-9]+:') | Out-Null

    # TypeScript 全量编译、模块回归和 AST 依赖图使用同一套本地 4.9.5 工具链，不访问网络或依赖全局 npm 状态。
    # TypeScript full compilation, module regression, and the AST dependency graph use the same local 4.9.5 toolchain without network or global npm state.
    $typescriptPath = Join-Path $outputFullPath "typescript"
    $typescriptConfigPath = Join-Path $temporaryRoot "typescript/tsconfig.json"
    $typescriptCompiledPath = Join-Path $temporaryRoot "typescript/compiled"
    [System.IO.Directory]::CreateDirectory((Split-Path $typescriptConfigPath -Parent)) | Out-Null
    $typescriptConfig = [ordered] @{
        compilerOptions = [ordered] @{
            target = "ES2020"
            module = "CommonJS"
            moduleResolution = "Node"
            lib = @("ES2020", "DOM")
            strict = $false
            noEmitOnError = $true
            outDir = $typescriptCompiledPath.Replace("\", "/")
            skipLibCheck = $true
            forceConsistentCasingInFileNames = $true
        }
        include = @(([System.IO.Path]::Combine($typescriptPath, "*.ts")).Replace("\", "/"))
    }
    [System.IO.File]::WriteAllText($typescriptConfigPath, ($typescriptConfig | ConvertTo-Json -Depth 8), [System.Text.UTF8Encoding]::new($false))
    Invoke-CheckedCommand -Label "TypeScript build" -FilePath $typescriptFullPath -Arguments @("-p", $typescriptConfigPath) | Out-Null

    $moduleTestPath = (Resolve-Path (Join-Path $repoRoot "kbe/tools/sdk_validation/tests/typescript_modules/Program.js")).Path
    $moduleOutput = Invoke-CheckedCommand -Label "TypeScript module regression" -FilePath "node" -Arguments @(
        $moduleTestPath, $typescriptPath, $typescriptCompiledPath
    )
    Assert-Condition ($moduleOutput -match "TYPESCRIPT_MODULE_TEST_PASS") "The TypeScript module regression pass marker is missing."

    foreach ($typescriptFile in Get-ChildItem -LiteralPath $typescriptPath -Filter "*.ts" -File) {
        $typescriptBody = [System.IO.File]::ReadAllText($typescriptFile.FullName)
        Assert-Condition ($typescriptBody -notmatch 'EntityDef\.(?:moduledefs|idmoduledefs|id2datatypes|datatype2id)\s*\[') "Generated TypeScript still indexes a Map as an object: $($typescriptFile.Name)"
    }

    $cycleScriptPath = Join-Path $temporaryRoot "typescript/check-cycles.js"
    $cycleScript = @'
"use strict";
const fs = require("fs");
const path = require("path");
const ts = require(process.argv[2]);
const root = path.resolve(process.argv[3]);
const files = fs.readdirSync(root).filter(name => name.endsWith(".ts")).sort();
const graph = new Map(files.map(name => [name, []]));
for (const name of files) {
    const source = ts.createSourceFile(name, fs.readFileSync(path.join(root, name), "utf8"), ts.ScriptTarget.Latest, true);
    for (const statement of source.statements) {
        if ((!ts.isImportDeclaration(statement) && !ts.isExportDeclaration(statement)) || !statement.moduleSpecifier || !ts.isStringLiteral(statement.moduleSpecifier)) continue;
        const specifier = statement.moduleSpecifier.text;
        if (!specifier.startsWith("./")) continue;
        const target = path.basename(specifier) + ".ts";
        if (graph.has(target)) graph.get(name).push(target);
    }
}
const visiting = new Set();
const visited = new Set();
function visit(name, stack) {
    if (visiting.has(name)) throw new Error("TypeScript circular dependency: " + [...stack, name].join(" -> "));
    if (visited.has(name)) return;
    visiting.add(name);
    for (const dependency of graph.get(name)) visit(dependency, [...stack, name]);
    visiting.delete(name);
    visited.add(name);
}
for (const name of files) visit(name, []);
console.log(`TYPESCRIPT_DEPENDENCY_PASS files=${files.length} circular=0`);
'@
    [System.IO.File]::WriteAllText($cycleScriptPath, $cycleScript, [System.Text.UTF8Encoding]::new($false))
    $cycleOutput = Invoke-CheckedCommand -Label "TypeScript dependency graph" -FilePath "node" -Arguments @(
        $cycleScriptPath, $typescriptModulePath, $typescriptPath
    )
    Assert-Condition ($cycleOutput -match "TYPESCRIPT_DEPENDENCY_PASS") "The TypeScript dependency pass marker is missing."

    # Godot 在隔离副本中建立 .godot 缓存；候选 SDK 保持只读，解析错误与脚本错误均阻断发布。
    # Godot builds its .godot cache in an isolated copy; the candidate SDK remains read-only and parse or script errors block release.
    $gdscriptWorkPath = Join-Path $temporaryRoot "gdscript"
    Copy-Item -LiteralPath (Join-Path $outputFullPath "gdscript") -Destination $gdscriptWorkPath -Recurse
    $godotProject = @"
[application]
config/name="KBEngine SDK Release Candidate"
[rendering]
renderer/rendering_method="gl_compatibility"
"@
    [System.IO.File]::WriteAllText((Join-Path $gdscriptWorkPath "project.godot"), $godotProject, [System.Text.UTF8Encoding]::new($false))
    Invoke-CheckedCommand -Label "Godot headless parse" -FilePath $godotFullPath -Arguments @(
        "--headless", "--path", $gdscriptWorkPath, "--editor", "--quit"
    ) -ForbiddenPatterns @("SCRIPT ERROR", "Parse Error") | Out-Null

    Write-Output "WINDOWS_RELEASE_CANDIDATE_PASS version=$($manifest.metadata.engineVersion) protocol=$($manifest.metadata.protocolDigest) entitydef=$($manifest.metadata.entityDefDigest) root=$($manifest.releaseTreeSha256) csharp-warnings=0 cxx-warnings=0 typescript-build=true typescript-modules=true map-indexing=0 circular=0 godot-parse=true"
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $cxxTemporaryRoot) {
        Remove-Item -LiteralPath $cxxTemporaryRoot -Recurse -Force
    }
}
