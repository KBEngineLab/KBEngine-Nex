param(
    [Parameter(Mandatory = $true)]
    [string]$Components,

    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,

    [Parameter(Mandatory = $true)]
    [string]$AssetsRoot,

    [Parameter(Mandatory = $true)]
    [string]$BinaryRoot,

    [Parameter(Mandatory = $true)]
    [string]$RunRoot,

    [ValidateRange(10, 150)]
    [int]$StartupTimeoutSeconds = 90
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-NormalizedPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return [System.IO.Path]::GetFullPath($Path).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
}

function Read-LogFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ''
    }

    return Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
}

$repositoryPath = Get-NormalizedPath -Path $RepositoryRoot
$assetsPath = Get-NormalizedPath -Path $AssetsRoot
$binaryPath = Get-NormalizedPath -Path $BinaryRoot
$runPath = Get-NormalizedPath -Path $RunRoot
$binaryPrefix = $binaryPath + [System.IO.Path]::DirectorySeparatorChar
if (-not $runPath.StartsWith($binaryPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "RunRoot must stay under the CMake binary tree: $runPath"
}

$componentSpecs = @()
foreach ($encodedComponent in $Components.Split('|', [System.StringSplitOptions]::RemoveEmptyEntries)) {
    $fields = $encodedComponent.Split(@('::'), [System.StringSplitOptions]::None)
    if ($fields.Count -ne 4) {
        throw "Invalid component specification: $encodedComponent"
    }

    $executablePath = Get-NormalizedPath -Path $fields[1]
    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw "Component executable is missing: $executablePath"
    }

    $componentSpecs += [pscustomobject]@{
        Name = $fields[0]
        Executable = $executablePath
        ComponentId = [uint64]$fields[2]
        GlobalOrder = [int]$fields[3]
    }
}

if ($componentSpecs.Count -ne 9) {
    throw "Server cluster smoke requires exactly 9 components, received $($componentSpecs.Count)."
}

# 清理范围在解析后固定到 CMake Testing 子目录，避免测试失败时触碰源码或用户资产。
# Cleanup is pinned to a CMake Testing subdirectory after normalization so failures cannot touch source files or user assets.
if (Test-Path -LiteralPath $runPath) {
    Remove-Item -LiteralPath $runPath -Recurse -Force
}
New-Item -ItemType Directory -Path $runPath -Force | Out-Null

$env:KBE_ROOT = $repositoryPath
$env:KBE_RES_PATH = @(
    Join-Path $repositoryPath 'kbe/res'
    $assetsPath
    Join-Path $assetsPath 'scripts'
    Join-Path $assetsPath 'res'
) -join ';'
$env:KBE_BIN_PATH = Split-Path -Parent $componentSpecs[0].Executable

$started = [System.Collections.Generic.List[object]]::new()
try {
    foreach ($spec in $componentSpecs) {
        $stdoutPath = Join-Path $runPath "$($spec.Name).stdout.log"
        $stderrPath = Join-Path $runPath "$($spec.Name).stderr.log"
        $startArguments = @{
            FilePath = $spec.Executable
            ArgumentList = @("--cid=$($spec.ComponentId)", "--gus=$($spec.GlobalOrder)", '--hide=1')
            WorkingDirectory = $runPath
            RedirectStandardOutput = $stdoutPath
            RedirectStandardError = $stderrPath
            WindowStyle = 'Hidden'
            PassThru = $true
        }
        $process = Start-Process @startArguments

        $started.Add([pscustomobject]@{
            Spec = $spec
            Process = $process
            Stdout = $stdoutPath
            Stderr = $stderrPath
        })

        # Machine 必须先建立发现端点，其他组件随后仍由真实广播流程互相发现。
        # Machine must establish discovery first; subsequent components still discover each other through the real broadcast path.
        if ($spec.Name -eq 'machine') {
            Start-Sleep -Seconds 1
        } else {
            Start-Sleep -Milliseconds 150
        }
    }

    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    do {
        $allRunning = $true
        foreach ($entry in $started) {
            if ($entry.Process.HasExited) {
                throw "$($entry.Spec.Name) exited during startup with code $($entry.Process.ExitCode)."
            }

            $stdout = Read-LogFile -Path $entry.Stdout
            if ($stdout -notmatch "----\s+$([regex]::Escape($entry.Spec.Name))\s+is running\s+----") {
                $allRunning = $false
            }
        }

        if ($allRunning) {
            break
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    if (-not $allRunning) {
        $pending = @($started | Where-Object {
            (Read-LogFile -Path $_.Stdout) -notmatch "----\s+$([regex]::Escape($_.Spec.Name))\s+is running\s+----"
        } | ForEach-Object { $_.Spec.Name })
        throw "Components did not reach running state before timeout: $($pending -join ', ')."
    }

    Start-Sleep -Seconds 5
    foreach ($entry in $started) {
        if ($entry.Process.HasExited) {
            throw "$($entry.Spec.Name) exited after reporting ready with code $($entry.Process.ExitCode)."
        }

        $combinedLog = (Read-LogFile -Path $entry.Stdout) + "`n" + (Read-LogFile -Path $entry.Stderr)
        if ($combinedLog -match '\[ERROR\]' -or
            $combinedLog -match '\[FATAL\]' -or
            $combinedLog -match 'Traceback \(most recent call last\)' -or
            $combinedLog -match 'Unhandled exception') {
            throw "$($entry.Spec.Name) emitted an error. See $($entry.Stdout) and $($entry.Stderr)."
        }
    }

    foreach ($managerName in @('baseappmgr', 'cellappmgr')) {
        $manager = $started | Where-Object { $_.Spec.Name -eq $managerName }
        if ((Read-LogFile -Path $manager.Stdout) -notmatch 'Found all the components!') {
            throw "$managerName did not complete component discovery."
        }
    }

    Write-Output "SERVER_CLUSTER_PASS components=$($started.Count)"
}
finally {
    # 只反向停止本次 Start-Process 返回的 PID，不按进程名清理其他开发或生产实例。
    # Stop only the exact PIDs returned by this run, in reverse order, without touching other development or production instances by name.
    for ($index = $started.Count - 1; $index -ge 0; --$index) {
        $entry = $started[$index]
        if (-not $entry.Process.HasExited) {
            Stop-Process -Id $entry.Process.Id -Force -ErrorAction SilentlyContinue
        }
    }

    foreach ($entry in $started) {
        try {
            [void]$entry.Process.WaitForExit(5000)
        } catch {
            Write-Warning "Failed to wait for $($entry.Spec.Name) PID $($entry.Process.Id): $($_.Exception.Message)"
        }
    }
}
