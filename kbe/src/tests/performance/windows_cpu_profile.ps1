[CmdletBinding()]
param(
    [string[]]$ProcessName = @("baseapp"),
    [int[]]$ProcessId = @(),
    [ValidateRange(1, 3600)]
    [int]$DurationSeconds = 60,
    [string]$OutputDirectory = "",
    [ValidateRange(0, 255)]
    [int]$SessionId = 75,
    [switch]$IncludeStacks,
    [switch]$DownloadMicrosoftSymbols,
    [string[]]$SymbolPath = @()
)

$ErrorActionPreference = "Stop"

function Resolve-PerformanceTool {
    param([Parameter(Mandatory = $true)][string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    if ($Name -eq "xperf.exe") {
        $candidate = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Windows Performance Toolkit\xperf.exe"
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    throw "Required performance tool was not found: $Name"
}

function Resolve-VsDiagnostics {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "Visual Studio Installer vswhere.exe was not found."
    }

    $installationPath = (& $vswhere -latest -products * -property installationPath | Select-Object -First 1)
    if (-not $installationPath) {
        throw "No Visual Studio installation was found."
    }

    $collectorRoot = Join-Path $installationPath "Team Tools\DiagnosticsHub\Collector"
    $collector = Join-Path $collectorRoot "VSDiagnostics.exe"
    $config = Join-Path $collectorRoot "AgentConfigs\CpuUsageLow.json"
    if (-not (Test-Path -LiteralPath $collector) -or -not (Test-Path -LiteralPath $config)) {
        throw "Visual Studio DiagnosticsHub CPU collector is not installed under $installationPath."
    }

    return [pscustomobject]@{
        Collector = $collector
        Config = $config
    }
}

function Assert-LastExitCode {
    param([Parameter(Mandatory = $true)][string]$Operation)
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation failed with exit code $LASTEXITCODE."
    }
}

function Export-CpuSummary {
    param(
        [Parameter(Mandatory = $true)][string]$ProfilePath,
        [Parameter(Mandatory = $true)][string]$SummaryPath,
        [Parameter(Mandatory = $true)][string]$TracePath,
        [Parameter(Mandatory = $true)][int[]]$TargetPids
    )

    $rows = foreach ($line in (Get-Content -LiteralPath $ProfilePath)) {
        if ($line -match '^\s*(.+?) \(\s*(\d+)\),\s*([\d]+),\s*([\d.]+),\s*(.+?)\s*$') {
            [pscustomobject]@{
                process = $matches[1].Trim()
                pid = [int]$matches[2]
                weight = [double]$matches[3]
                usage_percent = [double]$matches[4]
                module = $matches[5].Trim()
            }
        }
    }

    # Aggregate the machine view separately from per-PID modules so local load generators
    # cannot be mistaken for server CPU.
    $processes = $rows | Group-Object process | ForEach-Object {
        [ordered]@{
            process = $_.Name
            instances = (@($_.Group.pid | Sort-Object -Unique)).Count
            usage_percent = [math]::Round(($_.Group | Measure-Object usage_percent -Sum).Sum, 4)
            weight = [math]::Round(($_.Group | Measure-Object weight -Sum).Sum)
        }
    }
    $processes = @($processes | Sort-Object { [double]$_['usage_percent'] } -Descending)

    $targetRows = @($rows | Where-Object { $TargetPids -contains $_.pid })
    $targetUsage = ($targetRows | Measure-Object usage_percent -Sum).Sum
    $modules = $targetRows | Group-Object module | ForEach-Object {
        $usage = ($_.Group | Measure-Object usage_percent -Sum).Sum
        $targetShare = 0.0
        if ($targetUsage -gt 0) {
            $targetShare = [math]::Round(100.0 * $usage / $targetUsage, 4)
        }
        [ordered]@{
            module = $_.Name
            usage_percent = [math]::Round($usage, 4)
            target_share_percent = $targetShare
            weight = [math]::Round(($_.Group | Measure-Object weight -Sum).Sum)
        }
    }
    $modules = @($modules | Sort-Object { [double]$_['usage_percent'] } -Descending)

    $summary = [ordered]@{
        schema_version = 1
        generated_at = (Get-Date).ToUniversalTime().ToString("o")
        trace = $TracePath
        target_pids = @($TargetPids)
        duration_seconds = $DurationSeconds
        target_usage_percent = [math]::Round($targetUsage, 4)
        processes = $processes
        target_modules = $modules
    }
    $summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $SummaryPath -Encoding utf8
}

$diagnostics = Resolve-VsDiagnostics
$xperf = Resolve-PerformanceTool -Name "xperf.exe"

if (-not $OutputDirectory) {
    $runName = "windows-cpu-{0}" -f (Get-Date -Format "yyyyMMdd-HHmmss")
    $OutputDirectory = Join-Path $PSScriptRoot "..\..\out\performance-runs\$runName"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

$targets = if ($ProcessId.Count -gt 0) {
    @(Get-Process -Id $ProcessId -ErrorAction SilentlyContinue | Sort-Object Id)
} else {
    @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Sort-Object Id)
}
if ($targets.Count -eq 0) {
    $requested = if ($ProcessId.Count -gt 0) { $ProcessId -join ", " } else { $ProcessName -join ", " }
    throw "None of the requested processes are running: $requested"
}
$targetPids = @($targets.Id)
$attach = $targetPids -join ";"
$sessionOutput = Join-Path $OutputDirectory "cpu.diagsession"

$started = $false
try {
    & $diagnostics.Collector start $SessionId "/attach:$attach" "/loadConfig:$($diagnostics.Config)" `
        "/scratchLocation:$OutputDirectory" /package:dir
    Assert-LastExitCode -Operation "DiagnosticsHub start"
    $started = $true
    Start-Sleep -Seconds $DurationSeconds
}
finally {
    if ($started) {
        & $diagnostics.Collector stop $SessionId "/output:$sessionOutput"
        Assert-LastExitCode -Operation "DiagnosticsHub stop"
    }
}

$trace = Get-ChildItem -LiteralPath $sessionOutput -Recurse -Filter "*.etl" |
    Sort-Object Length -Descending | Select-Object -First 1
if (-not $trace) {
    throw "DiagnosticsHub completed without producing an ETL trace."
}

$moduleReport = Join-Path $OutputDirectory "cpu-modules.txt"
& $xperf -i $trace.FullName -o $moduleReport -a profile -detail
Assert-LastExitCode -Operation "xperf module export"

$summary = Join-Path $OutputDirectory "cpu-summary.json"
Export-CpuSummary -ProfilePath $moduleReport -SummaryPath $summary -TracePath $trace.FullName -TargetPids $targetPids

if ($IncludeStacks) {
    $symbolParts = @($SymbolPath | Where-Object { $_ })
    if ($DownloadMicrosoftSymbols) {
        $symbolCache = Join-Path $OutputDirectory "symbols"
        $symbolParts = @("srv*$symbolCache*https://msdl.microsoft.com/download/symbols") + $symbolParts
    }
    if ($symbolParts.Count -gt 0) {
        $env:_NT_SYMBOL_PATH = $symbolParts -join ";"
    }
    $env:_NT_SYMCACHE_PATH = Join-Path $OutputDirectory "symcache"
    $stackReport = Join-Path $OutputDirectory "cpu-stacks.html"
    & $xperf -i $trace.FullName -symbols -o $stackReport -a stack -butterfly 10 -pid $targetPids
    Assert-LastExitCode -Operation "xperf stack export"
}

Write-Output "CPU_PROFILE_COMPLETE=$OutputDirectory"
Write-Output "TARGET_PIDS=$attach"
Write-Output "SUMMARY=$summary"
