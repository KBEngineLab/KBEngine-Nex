[CmdletBinding()]
param(
    [string] $KbcmdPath,
    [string] $DotnetPath = "dotnet"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../../../..")).Path
$pythonAssets = (Resolve-Path (Join-Path $repoRoot "kbe/res/sdk_templates/server/python_assets")).Path
$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("kbe-csharp-sdk-warnings-" + [Guid]::NewGuid().ToString("N"))
$generatedPath = Join-Path $temporaryRoot "generated"

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

    $env:KBE_ROOT = $repoRoot
    $env:KBE_RES_PATH = "$repoRoot/kbe/res;$pythonAssets;$pythonAssets/scripts;$pythonAssets/res"
    $env:KBE_BIN_PATH = Join-Path $repoRoot "kbe/bin/server"
    $generationOutput = @(& $KbcmdPath --clientsdk=csharp "--outpath=$generatedPath" 2>&1)
    Assert-Condition ($LASTEXITCODE -eq 0) "C# SDK generation failed: $($generationOutput -join [Environment]::NewLine)"

    $projectPath = Join-Path $generatedPath "GeneratedSdkWarningTest.csproj"
    $project = @"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <Nullable>disable</Nullable>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <TreatWarningsAsErrors>true</TreatWarningsAsErrors>
  </PropertyGroup>
</Project>
"@
    [System.IO.File]::WriteAllText($projectPath, $project, [System.Text.UTF8Encoding]::new($false))

    $program = @"
using System;
using System.Threading;
using KBEngine;

internal static class Program
{
    private static int Main()
    {
        var args = new KBEngineArgs();
        args.isMultiThreads = true;
        var app = new KBEngineAppThread(args);
        app.destroy();

        if (!app.kbethread.over)
            throw new InvalidOperationException("The KBEngine worker did not complete cooperative shutdown.");

        var selfDestroyingApp = new SelfDestroyingApp(args);
        if (!SpinWait.SpinUntil(() => selfDestroyingApp.kbethread.over, TimeSpan.FromSeconds(5)))
            throw new InvalidOperationException("The KBEngine worker did not complete self-triggered shutdown.");

        Console.WriteLine("CSHARP_THREAD_LIFECYCLE_TEST_PASS cooperative-stop=true self-stop=true");
        return 0;
    }

    private sealed class SelfDestroyingApp : KBEngineAppThread
    {
        public SelfDestroyingApp(KBEngineArgs args) : base(args)
        {
        }

        public override void process()
        {
            destroy();
        }
    }
}
"@
    [System.IO.File]::WriteAllText(
        (Join-Path $generatedPath "Program.cs"),
        $program,
        [System.Text.UTF8Encoding]::new($false))

    $buildOutput = @(& $DotnetPath build $projectPath -c Release --nologo 2>&1)
    Assert-Condition ($LASTEXITCODE -eq 0) "The generated C# SDK is not warning-free: $($buildOutput -join [Environment]::NewLine)"
    $combinedBuildOutput = $buildOutput -join [Environment]::NewLine
    Assert-Condition ($combinedBuildOutput -notmatch "(?:warning|警告)\s+[A-Z]+[0-9]+:") "The generated C# SDK build emitted a warning marker."

    $runOutput = @(& $DotnetPath run --project $projectPath -c Release --no-build 2>&1)
    Assert-Condition ($LASTEXITCODE -eq 0) "The generated C# SDK thread lifecycle test failed: $($runOutput -join [Environment]::NewLine)"
    Assert-Condition (($runOutput -join [Environment]::NewLine) -match "CSHARP_THREAD_LIFECYCLE_TEST_PASS") "The C# thread lifecycle pass marker is missing."

    Write-Output "CSHARP_SDK_WARNING_TEST_PASS release=true warnings=0 errors=0 cooperative-stop=true self-stop=true"
}
finally {
    $env:KBE_ROOT = $oldKbeRoot
    $env:KBE_RES_PATH = $oldKbeResPath
    $env:KBE_BIN_PATH = $oldKbeBinPath

    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
