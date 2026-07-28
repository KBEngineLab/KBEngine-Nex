[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SdkPath,

    [Parameter(Mandatory = $true)]
    [string]$GodotPath
)

$ErrorActionPreference = "Stop"
$sdkDirectory = (Resolve-Path -LiteralPath $SdkPath).Path
$godotExecutable = (Resolve-Path -LiteralPath $GodotPath).Path
$programPath = Join-Path $PSScriptRoot "Program.gd"

foreach ($requiredFile in @("Messages.gd", "MemoryStream.gd", "MessageReaderWS.gd")) {
    if (-not (Test-Path -LiteralPath (Join-Path $sdkDirectory $requiredFile) -PathType Leaf)) {
        throw "Generated GDScript SDK is missing $requiredFile in $sdkDirectory"
    }
}

$projectPath = Join-Path $sdkDirectory "project.godot"
if (-not (Test-Path -LiteralPath $projectPath -PathType Leaf)) {
    # 生成 SDK 目录不是完整 Godot 项目；最小项目文件只用于建立 class_name 缓存，不修改 SDK 源码。
    # A generated SDK directory is not a full Godot project; this minimal file only builds the class_name cache without modifying SDK sources.
    $projectContent = @"
[application]
config/name="KBEngine GDScript WebSocket Validation"

[display]
window/size/viewport_width=320
window/size/viewport_height=180

[rendering]
renderer/rendering_method="gl_compatibility"
"@
    [System.IO.File]::WriteAllText($projectPath, $projectContent, [System.Text.UTF8Encoding]::new($false))
}

& $godotExecutable --headless --path $sdkDirectory --editor --quit
if ($LASTEXITCODE -ne 0) {
    throw "Godot failed to import the generated GDScript SDK, exitCode=$LASTEXITCODE"
}

& $godotExecutable --headless --path $sdkDirectory --script $programPath
if ($LASTEXITCODE -ne 0) {
    throw "GDScript WebSocket frame validation failed, exitCode=$LASTEXITCODE"
}
