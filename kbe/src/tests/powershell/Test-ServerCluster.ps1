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

    [ValidateSet('mysql', 'mongodb', 'postgresql')]
    [string]$DatabaseType,

    [string]$DatabaseHost = '127.0.0.1',

    [ValidateRange(1, 65535)]
    [int]$DatabasePort,

    [string]$DatabaseName,

    [string]$DatabaseUsername,

    [string]$DatabasePassword,

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

function Read-ComponentOutput {
    param(
        [Parameter(Mandatory = $true)]$Entry,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $output = (Read-LogFile -Path $Entry.Stdout) + "`n" + (Read-LogFile -Path $Entry.Stderr)
    $engineLogRoot = Join-Path $Root 'logs'
    if (Test-Path -LiteralPath $engineLogRoot -PathType Container) {
        foreach ($logPattern in @("$($Entry.Spec.Name)*.log", "logger_$($Entry.Spec.Name).log")) {
            foreach ($engineLog in Get-ChildItem -LiteralPath $engineLogRoot -File -Filter $logPattern) {
                $output += "`n" + (Read-LogFile -Path $engineLog.FullName)
            }
        }
    }
    return $output
}

function Read-AllRunLogs {
    param(
        [Parameter(Mandatory = $true)]$Entries,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $output = ''
    foreach ($entry in $Entries) {
        $output += "`n" + (Read-LogFile -Path $entry.Stdout)
        $output += "`n" + (Read-LogFile -Path $entry.Stderr)
    }

    $engineLogRoot = Join-Path $Root 'logs'
    if (Test-Path -LiteralPath $engineLogRoot -PathType Container) {
        foreach ($engineLog in Get-ChildItem -LiteralPath $engineLogRoot -File -Filter '*.log') {
            $output += "`n" + (Read-LogFile -Path $engineLog.FullName)
        }
    }
    return $output
}

function Set-XmlElementValue {
    param(
        [Parameter(Mandatory = $true)][System.Xml.XmlElement]$Parent,
        [Parameter(Mandatory = $true)][string]$Name,
        [AllowEmptyString()][string]$Value
    )

    $element = $Parent.SelectSingleNode($Name)
    if ($null -eq $element) {
        $element = $Parent.OwnerDocument.CreateElement($Name)
        [void]$Parent.AppendChild($element)
    }
    $element.InnerText = $Value
}

function New-DatabaseConfigOverlay {
    param(
        [Parameter(Mandatory = $true)][string]$SourceAssets,
        [Parameter(Mandatory = $true)][string]$DestinationRoot
    )

    $sourceConfig = Join-Path $SourceAssets 'res/server/kbengine.xml'
    if (-not (Test-Path -LiteralPath $sourceConfig -PathType Leaf)) {
        $sourceConfig = Join-Path $SourceAssets 'kbengine.xml'
    }
    if (-not (Test-Path -LiteralPath $sourceConfig -PathType Leaf)) {
        throw "Assets do not contain a kbengine.xml database configuration: $SourceAssets"
    }

    $document = [System.Xml.XmlDocument]::new()
    $document.PreserveWhitespace = $true
    $document.Load($sourceConfig)
    $database = $document.SelectSingleNode('/root/dbmgr/databaseInterfaces/default')
    if ($null -eq $database) {
        throw "Database interface 'default' is missing from $sourceConfig"
    }

    Set-XmlElementValue -Parent $database -Name 'type' -Value $DatabaseType
    Set-XmlElementValue -Parent $database -Name 'host' -Value $DatabaseHost
    Set-XmlElementValue -Parent $database -Name 'port' -Value ([string]$DatabasePort)
    Set-XmlElementValue -Parent $database -Name 'databaseName' -Value $DatabaseName

    $auth = $database.SelectSingleNode('auth')
    if ($null -eq $auth) {
        $auth = $document.CreateElement('auth')
        [void]$database.AppendChild($auth)
    }
    Set-XmlElementValue -Parent $auth -Name 'username' -Value $DatabaseUsername
    Set-XmlElementValue -Parent $auth -Name 'password' -Value $DatabasePassword
    Set-XmlElementValue -Parent $auth -Name 'encrypt' -Value 'false'
    Set-XmlElementValue -Parent $auth -Name 'authSource' -Value $(if ($DatabaseType -eq 'mongodb') { 'admin' } else { '' })

    $configDirectory = Join-Path $DestinationRoot 'res/server'
    New-Item -ItemType Directory -Path $configDirectory -Force | Out-Null
    $destinationConfig = Join-Path $configDirectory 'kbengine.xml'
    $settings = [System.Xml.XmlWriterSettings]::new()
    $settings.Encoding = [System.Text.UTF8Encoding]::new($false)
    $settings.Indent = $false
    $writer = [System.Xml.XmlWriter]::Create($destinationConfig, $settings)
    try {
        $document.Save($writer)
    } finally {
        $writer.Dispose()
    }

    return (Join-Path $DestinationRoot 'res')
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

$resourceRoots = [System.Collections.Generic.List[string]]::new()
$overlayPath = $null
if ($DatabaseType) {
    if ($DatabasePort -eq 0 -or [string]::IsNullOrWhiteSpace($DatabaseName) -or
        [string]::IsNullOrWhiteSpace($DatabaseUsername)) {
        throw 'DatabaseType requires DatabasePort, DatabaseName, and DatabaseUsername.'
    }

    # 覆盖文件只写入 Testing 目录，原始 assets 始终保持只读。
    # The override is written only under Testing, keeping the source assets strictly read-only.
    $overlayPath = New-DatabaseConfigOverlay -SourceAssets $assetsPath -DestinationRoot (Join-Path $runPath 'config-overlay')
}
if ($overlayPath) {
    # Resmgr 使用首个匹配资源，覆盖目录必须先于原 assets。
    # Resmgr uses the first matching resource, so the overlay must precede the source assets.
    $resourceRoots.Add($overlayPath)
}
$resourceRoots.Add((Join-Path $repositoryPath 'kbe/res'))
$resourceRoots.Add($assetsPath)
$resourceRoots.Add((Join-Path $assetsPath 'scripts'))
$resourceRoots.Add((Join-Path $assetsPath 'res'))

$env:KBE_ROOT = $repositoryPath
$env:KBE_RES_PATH = $resourceRoots -join ';'
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

            $componentOutput = Read-ComponentOutput -Entry $entry -Root $runPath
            if ($componentOutput -notmatch "----\s+$([regex]::Escape($entry.Spec.Name))\s+is running\s+----") {
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
            (Read-ComponentOutput -Entry $_ -Root $runPath) -notmatch "----\s+$([regex]::Escape($_.Spec.Name))\s+is running\s+----"
        } | ForEach-Object { $_.Spec.Name })
        throw "Components did not reach running state before timeout: $($pending -join ', ')."
    }

    $discoveryDeadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $managersReady = $true
        foreach ($managerName in @('baseappmgr', 'cellappmgr')) {
            $manager = $started | Where-Object { $_.Spec.Name -eq $managerName }
            if ((Read-ComponentOutput -Entry $manager -Root $runPath) -notmatch 'Found all the components!') {
                $managersReady = $false
            }
        }
        if ($managersReady) {
            break
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $discoveryDeadline)

    if (-not $managersReady) {
        throw 'BaseAppMgr and CellAppMgr did not complete component discovery before timeout.'
    }

    foreach ($entry in $started) {
        if ($entry.Process.HasExited) {
            throw "$($entry.Spec.Name) exited after reporting ready with code $($entry.Process.ExitCode)."
        }

    }

    # 组件注册 Logger 后会把正式日志写入 RunRoot/logs，验收必须覆盖该目录而非只看启动期 stdout。
    # Components route formal logs into RunRoot/logs after Logger registration, so validation must inspect them as well as startup stdout.
    $combinedLog = Read-AllRunLogs -Entries $started -Root $runPath
    if ($combinedLog -match '\[ERROR\]' -or
        $combinedLog -match '\[FATAL\]' -or
        $combinedLog -match '(?m)^\s*(ERROR|FATAL)\s' -or
        $combinedLog -match 'Traceback \(most recent call last\)' -or
        $combinedLog -match 'Unhandled exception') {
        throw "Server cluster emitted an error. See logs under $runPath."
    }

    if ($DatabaseType) {
        $dbmgr = $started | Where-Object { $_.Spec.Name -eq 'dbmgr' }
        $dbmgrOutput = Read-ComponentOutput -Entry $dbmgr -Root $runPath
        $databasePattern = "(type|dbtype)=$([regex]::Escape($DatabaseType)).*(db|currdatabase)=$([regex]::Escape($DatabaseName)).*connected=(yes|true)"
        if ($dbmgrOutput -notmatch $databasePattern) {
            throw "DBMgr did not confirm the requested $DatabaseType database '$DatabaseName' as connected."
        }
    }

    $databaseResult = if ($DatabaseType) { " database=$DatabaseType" } else { '' }
    Write-Output "SERVER_CLUSTER_PASS components=$($started.Count)$databaseResult"
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
