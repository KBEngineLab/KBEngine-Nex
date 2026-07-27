[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ManifestPath,

    [string[]] $Sdk,

    [string] $ResultsPath,

    [switch] $SkipGenerate,

    [switch] $SkipBuild,

    [switch] $SkipRun,

    [switch] $ShowOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$supportedSdks = @("csharp", "cxx", "typescript", "gdscript")
$stageNames = @("generate", "build", "run")

function Resolve-NormalizedPath {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [Parameter(Mandatory = $true)]
        [string] $BasePath,

        [switch] $RequireExisting
    )

    $candidate = if ([System.IO.Path]::IsPathRooted($Path)) {
        $Path
    }
    else {
        Join-Path $BasePath $Path
    }

    $fullPath = [System.IO.Path]::GetFullPath($candidate)
    if ($RequireExisting -and -not (Test-Path -LiteralPath $fullPath)) {
        throw "Path does not exist: $fullPath"
    }

    return $fullPath
}

function Expand-TemplateValue {
    param(
        [AllowEmptyString()]
        [string] $Value,

        [Parameter(Mandatory = $true)]
        [hashtable] $Variables,

        [string[]] $ExpansionStack = @()
    )

    if ($null -eq $Value) {
        return ""
    }

    $pattern = '\$\{([A-Za-z_][A-Za-z0-9_]*)\}'
    return [regex]::Replace($Value, $pattern, {
        param($match)

        $name = $match.Groups[1].Value
        if (-not $Variables.ContainsKey($name)) {
            throw "Unknown manifest variable '$name' in '$Value'."
        }

        if ($ExpansionStack -contains $name) {
            $cycle = ($ExpansionStack + $name) -join " -> "
            throw "Circular manifest variable reference: $cycle"
        }

        return Expand-TemplateValue -Value ([string] $Variables[$name]) `
            -Variables $Variables -ExpansionStack ($ExpansionStack + $name)
    })
}

function Convert-ObjectToStringMap {
    param(
        [AllowNull()]
        [object] $InputObject,

        [Parameter(Mandatory = $true)]
        [hashtable] $Variables
    )

    $result = @{}
    if ($null -eq $InputObject) {
        return $result
    }

    foreach ($property in $InputObject.PSObject.Properties) {
        $result[$property.Name] = Expand-TemplateValue -Value ([string] $property.Value) -Variables $Variables
    }

    return $result
}

function Get-OptionalPropertyValue {
    param(
        [AllowNull()]
        [object] $InputObject,

        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    if ($null -eq $InputObject) {
        return $null
    }

    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $null
    }

    return $property.Value
}

function Convert-ToCommandLineArgument {
    param(
        [AllowEmptyString()]
        [string] $Argument
    )

    if ($Argument -notmatch '[\s"]') {
        return $Argument
    }

    # Windows 命令行引用必须同时处理结尾反斜杠和引号前反斜杠，否则路径可能被 cmd.exe 截断。
    # Windows command-line quoting must handle trailing backslashes and backslashes before quotes, or cmd.exe may truncate paths.
    $escaped = [regex]::Replace($Argument, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

function New-ProcessStartInfo {
    param(
        [Parameter(Mandatory = $true)]
        [string] $FilePath,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]] $Arguments,

        [Parameter(Mandatory = $true)]
        [string] $WorkingDirectory,

        [Parameter(Mandatory = $true)]
        [hashtable] $Environment
    )

    $resolvedCommand = $FilePath
    if (-not [System.IO.Path]::IsPathRooted($resolvedCommand) -and
        $resolvedCommand.IndexOfAny([char[]] @('\', '/')) -ge 0) {
        $resolvedCommand = Resolve-NormalizedPath -Path $resolvedCommand -BasePath $WorkingDirectory -RequireExisting
    }
    elseif (-not [System.IO.Path]::IsPathRooted($resolvedCommand)) {
        $command = Get-Command $resolvedCommand -CommandType Application -ErrorAction Stop | Select-Object -First 1
        $resolvedCommand = $command.Source
    }
    elseif (-not (Test-Path -LiteralPath $resolvedCommand)) {
        throw "Command does not exist: $resolvedCommand"
    }

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $extension = [System.IO.Path]::GetExtension($resolvedCommand)
    if ($extension -in @(".cmd", ".bat")) {
        # UseShellExecute=false 不能直接可靠启动批处理文件，因此仅在该边界使用 cmd.exe，并逐参数完成引用。
        # UseShellExecute=false cannot reliably launch batch files directly, so cmd.exe is used only at this boundary with per-argument quoting.
        $commandParts = @((Convert-ToCommandLineArgument $resolvedCommand))
        $commandParts += $Arguments | ForEach-Object { Convert-ToCommandLineArgument $_ }
        $startInfo.FileName = $env:ComSpec
        $startInfo.ArgumentList.Add("/d")
        $startInfo.ArgumentList.Add("/s")
        $startInfo.ArgumentList.Add("/c")
        $startInfo.ArgumentList.Add(($commandParts -join " "))
    }
    else {
        $startInfo.FileName = $resolvedCommand
        foreach ($argument in $Arguments) {
            $startInfo.ArgumentList.Add($argument)
        }
    }

    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $startInfo.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false)

    foreach ($entry in $Environment.GetEnumerator()) {
        $startInfo.Environment[$entry.Key] = $entry.Value
    }

    return $startInfo
}

function Write-StageLog {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [Parameter(Mandatory = $true)]
        [string] $SdkName,

        [Parameter(Mandatory = $true)]
        [string] $StageName,

        [Parameter(Mandatory = $true)]
        [string] $Command,

        [Parameter(Mandatory = $true)]
        [string] $WorkingDirectory,

        [Parameter(Mandatory = $true)]
        [int] $ExitCode,

        [Parameter(Mandatory = $true)]
        [bool] $TimedOut,

        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string] $StandardOutput,

        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string] $StandardError
    )

    $content = @(
        "sdk=$SdkName",
        "stage=$StageName",
        "command=$Command",
        "workingDirectory=$WorkingDirectory",
        "exitCode=$ExitCode",
        "timedOut=$TimedOut",
        "",
        "----- stdout -----",
        $StandardOutput,
        "",
        "----- stderr -----",
        $StandardError
    ) -join [Environment]::NewLine

    [System.IO.File]::WriteAllText($Path, $content, [System.Text.UTF8Encoding]::new($false))
}

function Write-ProcessOutput {
    param(
        [AllowEmptyString()]
        [string] $StandardOutput,

        [AllowEmptyString()]
        [string] $StandardError,

        [Parameter(Mandatory = $true)]
        [bool] $Passed,

        [Parameter(Mandatory = $true)]
        [bool] $ShowAll
    )

    $combined = @(@($StandardOutput, $StandardError) |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($combined.Count -eq 0) {
        return
    }

    if ($ShowAll) {
        Write-Host (($combined | ForEach-Object { $_.TrimEnd() }) -join [Environment]::NewLine)
        return
    }

    if (-not $Passed) {
        # 失败时显示末尾诊断并保留完整日志，兼顾终端可读性和问题追溯所需的全部上下文。
        # On failure, show the diagnostic tail while retaining the full log, balancing terminal readability with complete evidence.
        $lines = (($combined -join [Environment]::NewLine) -split '\r?\n')
        $tail = @($lines | Select-Object -Last 80)
        Write-Host "----- output tail (full output is in the stage log) -----"
        Write-Host ($tail -join [Environment]::NewLine)
    }
}

function Invoke-ValidationStage {
    param(
        [Parameter(Mandatory = $true)]
        [string] $SdkName,

        [Parameter(Mandatory = $true)]
        [string] $StageName,

        [Parameter(Mandatory = $true)]
        [object] $Stage,

        [Parameter(Mandatory = $true)]
        [string] $DefaultWorkingDirectory,

        [Parameter(Mandatory = $true)]
        [hashtable] $CommonEnvironment,

        [Parameter(Mandatory = $true)]
        [hashtable] $Variables,

        [Parameter(Mandatory = $true)]
        [string] $OutputDirectory,

        [Parameter(Mandatory = $true)]
        [bool] $ShowOutput
    )

    $stageFile = Get-OptionalPropertyValue -InputObject $Stage -Name "file"
    if ($null -eq $stageFile -or [string]::IsNullOrWhiteSpace([string] $stageFile)) {
        throw "SDK '$SdkName' stage '$StageName' does not define a command file."
    }

    $filePath = Expand-TemplateValue -Value ([string] $stageFile) -Variables $Variables
    $arguments = @()
    $stageArguments = Get-OptionalPropertyValue -InputObject $Stage -Name "arguments"
    if ($null -ne $stageArguments) {
        $arguments = @($stageArguments | ForEach-Object {
            Expand-TemplateValue -Value ([string] $_) -Variables $Variables
        })
    }

    $workingDirectory = $DefaultWorkingDirectory
    $stageWorkingDirectory = Get-OptionalPropertyValue -InputObject $Stage -Name "workingDirectory"
    if ($null -ne $stageWorkingDirectory -and -not [string]::IsNullOrWhiteSpace([string] $stageWorkingDirectory)) {
        $workingDirectory = Resolve-NormalizedPath `
            -Path (Expand-TemplateValue -Value ([string] $stageWorkingDirectory) -Variables $Variables) `
            -BasePath $Variables.ManifestDir -RequireExisting
    }

    $environment = @{}
    foreach ($entry in $CommonEnvironment.GetEnumerator()) {
        $environment[$entry.Key] = $entry.Value
    }

    $stageEnvironment = Convert-ObjectToStringMap `
        -InputObject (Get-OptionalPropertyValue -InputObject $Stage -Name "environment") `
        -Variables $Variables
    foreach ($entry in $stageEnvironment.GetEnumerator()) {
        $environment[$entry.Key] = $entry.Value
    }

    $configuredTimeout = Get-OptionalPropertyValue -InputObject $Stage -Name "timeoutSeconds"
    $timeoutSeconds = if ($null -ne $configuredTimeout) { [int] $configuredTimeout } else { 120 }
    if ($timeoutSeconds -lt 1) {
        throw "SDK '$SdkName' stage '$StageName' must use a positive timeoutSeconds value."
    }

    $startInfo = New-ProcessStartInfo -FilePath $filePath -Arguments $arguments `
        -WorkingDirectory $workingDirectory -Environment $environment
    $displayCommand = (@($startInfo.FileName) + @($startInfo.ArgumentList)) -join " "
    Write-Host "[$SdkName/$StageName] $displayCommand"

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Failed to start SDK '$SdkName' stage '$StageName'."
    }

    # 两个流必须在等待进程前并发读取，避免任一重定向管道填满后让子进程与父进程互相等待。
    # Both streams must be read concurrently before waiting, preventing a full redirected pipe from deadlocking child and parent.
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $timedOut = -not $process.WaitForExit($timeoutSeconds * 1000)
    if ($timedOut) {
        try {
            $process.Kill($true)
        }
        catch {
            $process.Kill()
        }

        $process.WaitForExit()
    }

    $standardOutput = $stdoutTask.GetAwaiter().GetResult()
    $standardError = $stderrTask.GetAwaiter().GetResult()
    $stopwatch.Stop()

    $exitCode = if ($timedOut) { 124 } else { $process.ExitCode }
    $combinedOutput = $standardOutput + [Environment]::NewLine + $standardError
    $failures = [System.Collections.Generic.List[string]]::new()
    if ($timedOut) {
        $failures.Add("Timed out after $timeoutSeconds seconds.")
    }
    elseif ($exitCode -ne 0) {
        $failures.Add("Exited with code $exitCode.")
    }

    $configuredRequiredPatterns = Get-OptionalPropertyValue -InputObject $Stage -Name "requiredPatterns"
    $requiredPatterns = if ($null -eq $configuredRequiredPatterns) { @() } else { @($configuredRequiredPatterns) }
    foreach ($pattern in $requiredPatterns) {
        if ($combinedOutput -notmatch [string] $pattern) {
            $failures.Add("Required output pattern was not found: $pattern")
        }
    }

    $configuredForbiddenPatterns = Get-OptionalPropertyValue -InputObject $Stage -Name "forbiddenPatterns"
    $forbiddenPatterns = if ($null -eq $configuredForbiddenPatterns) { @() } else { @($configuredForbiddenPatterns) }
    foreach ($pattern in $forbiddenPatterns) {
        if ($combinedOutput -match [string] $pattern) {
            $failures.Add("Forbidden output pattern was found: $pattern")
        }
    }

    $logPath = Join-Path $OutputDirectory "$SdkName-$StageName.log"
    Write-StageLog -Path $logPath -SdkName $SdkName -StageName $StageName `
        -Command $displayCommand -WorkingDirectory $workingDirectory -ExitCode $exitCode `
        -TimedOut $timedOut -StandardOutput $standardOutput -StandardError $standardError

    $passed = $failures.Count -eq 0
    Write-ProcessOutput -StandardOutput $standardOutput -StandardError $standardError `
        -Passed $passed -ShowAll $ShowOutput
    if ($passed) {
        Write-Host "[$SdkName/$StageName] PASS ($([math]::Round($stopwatch.Elapsed.TotalSeconds, 2))s)" -ForegroundColor Green
    }
    else {
        Write-Host "[$SdkName/$StageName] FAIL: $($failures -join ' ')" -ForegroundColor Red
    }

    return [pscustomobject] @{
        sdk = $SdkName
        stage = $StageName
        status = if ($passed) { "passed" } else { "failed" }
        exitCode = $exitCode
        timedOut = $timedOut
        elapsedMilliseconds = $stopwatch.ElapsedMilliseconds
        log = $logPath
        failures = @($failures)
    }
}

$manifestFullPath = Resolve-NormalizedPath -Path $ManifestPath -BasePath (Get-Location).Path -RequireExisting
$manifestDirectory = Split-Path -Parent $manifestFullPath
$repoRoot = Resolve-NormalizedPath -Path "../../.." -BasePath $PSScriptRoot -RequireExisting
$manifest = Get-Content -LiteralPath $manifestFullPath -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 32

if ((Get-OptionalPropertyValue -InputObject $manifest -Name "schemaVersion") -ne 1) {
    throw "Unsupported SDK validation manifest schemaVersion '$($manifest.schemaVersion)'. Expected 1."
}

$variables = @{
    RepoRoot = $repoRoot
    ManifestDir = $manifestDirectory
}
$manifestVariables = Get-OptionalPropertyValue -InputObject $manifest -Name "variables"
if ($null -ne $manifestVariables) {
    foreach ($property in $manifestVariables.PSObject.Properties) {
        if ($variables.ContainsKey($property.Name)) {
            throw "Manifest variable '$($property.Name)' is reserved."
        }

        $variables[$property.Name] = [string] $property.Value
    }
}

$defaultWorkingDirectory = $manifestDirectory
$manifestWorkingDirectory = Get-OptionalPropertyValue -InputObject $manifest -Name "workingDirectory"
if ($null -ne $manifestWorkingDirectory -and -not [string]::IsNullOrWhiteSpace([string] $manifestWorkingDirectory)) {
    $defaultWorkingDirectory = Resolve-NormalizedPath `
        -Path (Expand-TemplateValue -Value ([string] $manifestWorkingDirectory) -Variables $variables) `
        -BasePath $manifestDirectory -RequireExisting
}

$manifestResultsDirectory = Get-OptionalPropertyValue -InputObject $manifest -Name "resultsDirectory"
$outputDirectory = if (-not [string]::IsNullOrWhiteSpace($ResultsPath)) {
    Resolve-NormalizedPath -Path $ResultsPath -BasePath $manifestDirectory
}
elseif ($null -ne $manifestResultsDirectory -and -not [string]::IsNullOrWhiteSpace([string] $manifestResultsDirectory)) {
    Resolve-NormalizedPath `
        -Path (Expand-TemplateValue -Value ([string] $manifestResultsDirectory) -Variables $variables) `
        -BasePath $manifestDirectory
}
else {
    Join-Path $manifestDirectory "sdk-validation-results"
}
[System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null

$commonEnvironment = Convert-ObjectToStringMap `
    -InputObject (Get-OptionalPropertyValue -InputObject $manifest -Name "environment") `
    -Variables $variables
$requestedSdks = @(if ($null -ne $Sdk -and $Sdk.Count -gt 0) {
    @($Sdk | ForEach-Object { $_.ToLowerInvariant() })
}
else {
    @()
})

foreach ($requestedSdk in $requestedSdks) {
    if ($requestedSdk -notin $supportedSdks) {
        throw "Unsupported SDK '$requestedSdk'. Supported values: $($supportedSdks -join ', ')."
    }
}

$manifestSdks = @(Get-OptionalPropertyValue -InputObject $manifest -Name "sdks")
if ($manifestSdks.Count -eq 0) {
    throw "The SDK validation manifest does not define any SDK entries."
}

$duplicateNames = @($manifestSdks | Group-Object { ([string] $_.name).ToLowerInvariant() } | Where-Object Count -gt 1)
if ($duplicateNames.Count -gt 0) {
    throw "Duplicate SDK entries: $(($duplicateNames.Name | Sort-Object) -join ', ')."
}

$results = [System.Collections.Generic.List[object]]::new()
$sdkSummaries = [System.Collections.Generic.List[object]]::new()

foreach ($sdkDefinition in $manifestSdks) {
    $sdkName = ([string] $sdkDefinition.name).ToLowerInvariant()
    if ($sdkName -notin $supportedSdks) {
        throw "Manifest contains unsupported SDK '$sdkName'. Supported values: $($supportedSdks -join ', ')."
    }

    if ($requestedSdks.Count -gt 0 -and $sdkName -notin $requestedSdks) {
        continue
    }

    Write-Host "=== SDK $sdkName ===" -ForegroundColor Cyan
    $sdkPassed = $true
    foreach ($stageName in $stageNames) {
        $skipStage = ($stageName -eq "generate" -and $SkipGenerate) -or
            ($stageName -eq "build" -and $SkipBuild) -or
            ($stageName -eq "run" -and $SkipRun)
        $stage = Get-OptionalPropertyValue -InputObject $sdkDefinition -Name $stageName

        if ($skipStage -or $null -eq $stage) {
            $results.Add([pscustomobject] @{
                sdk = $sdkName
                stage = $stageName
                status = "skipped"
                exitCode = $null
                timedOut = $false
                elapsedMilliseconds = 0
                log = $null
                failures = @()
            })
            continue
        }

        if (-not $sdkPassed) {
            # 前一阶段失败后继续运行会污染诊断，例如拿旧生成物掩盖生成失败，因此同 SDK 后续阶段直接跳过。
            # Running after an earlier failure can pollute diagnostics, such as stale output hiding generation failure, so later stages are skipped.
            $results.Add([pscustomobject] @{
                sdk = $sdkName
                stage = $stageName
                status = "blocked"
                exitCode = $null
                timedOut = $false
                elapsedMilliseconds = 0
                log = $null
                failures = @("A previous stage failed.")
            })
            continue
        }

        $stageResult = Invoke-ValidationStage -SdkName $sdkName -StageName $stageName -Stage $stage `
            -DefaultWorkingDirectory $defaultWorkingDirectory -CommonEnvironment $commonEnvironment `
            -Variables $variables -OutputDirectory $outputDirectory -ShowOutput $ShowOutput.IsPresent
        $results.Add($stageResult)
        $sdkPassed = $stageResult.status -eq "passed"
    }

    $sdkSummaries.Add([pscustomobject] @{
        name = $sdkName
        status = if ($sdkPassed) { "passed" } else { "failed" }
    })
}

if ($sdkSummaries.Count -eq 0) {
    throw "No manifest SDK entries matched the requested -Sdk filter."
}

$summary = [pscustomobject] @{
    schemaVersion = 1
    generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    manifest = $manifestFullPath
    resultsDirectory = $outputDirectory
    sdks = @($sdkSummaries)
    stages = @($results)
}
$summaryPath = Join-Path $outputDirectory "summary.json"
[System.IO.File]::WriteAllText(
    $summaryPath,
    ($summary | ConvertTo-Json -Depth 16),
    [System.Text.UTF8Encoding]::new($false))

$failedSdks = @($sdkSummaries | Where-Object status -eq "failed")
Write-Host "SDK validation summary: $summaryPath"
foreach ($sdkSummary in $sdkSummaries) {
    Write-Host "  $($sdkSummary.name): $($sdkSummary.status)"
}

if ($failedSdks.Count -gt 0) {
    exit 1
}

exit 0
