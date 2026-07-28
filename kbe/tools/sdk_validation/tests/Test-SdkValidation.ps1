[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$runner = (Resolve-Path (Join-Path $PSScriptRoot "../Invoke-SdkValidation.ps1")).Path
$emitter = (Resolve-Path (Join-Path $PSScriptRoot "Emit-ValidationResult.ps1")).Path
$pwsh = (Get-Command pwsh -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("kbe-sdk-validation-" + [Guid]::NewGuid().ToString("N"))
[System.IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null

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

function New-CommandDefinition {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Mode,

        [string] $Scenario = "baseline"
    )

    return [ordered] @{
        file = $pwsh
        arguments = @("-NoProfile", "-File", $emitter, "-Mode", $Mode, "-Scenario", $Scenario)
        timeoutSeconds = 10
        requiredPatterns = if ($Mode -eq "pass") {
            @("E2E_PASS scenario=$Scenario", "RESOURCE_PASS client_connections=0 server_channels=0 kcp_timers=0")
        }
        else {
            @()
        }
        forbiddenPatterns = @("E2E_FAIL", "RESOURCE_FAIL")
    }
}

function Write-Manifest {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Name,

        [Parameter(Mandatory = $true)]
        [object] $Manifest
    )

    $path = Join-Path $temporaryRoot "$Name.json"
    [System.IO.File]::WriteAllText(
        $path,
        ($Manifest | ConvertTo-Json -Depth 16),
        [System.Text.UTF8Encoding]::new($false))
    return $path
}

function Invoke-Runner {
    param(
        [Parameter(Mandatory = $true)]
        [string] $ManifestPath,

        [Parameter(Mandatory = $true)]
        [string] $ResultsPath,

        [string[]] $AdditionalArguments = @()
    )

    $output = @(& $pwsh -NoProfile -File $runner -ManifestPath $ManifestPath `
        -ResultsPath $ResultsPath @AdditionalArguments 2>&1)
    return [pscustomobject] @{
        exitCode = $LASTEXITCODE
        output = $output -join [Environment]::NewLine
    }
}

try {
    $matrixManifest = [ordered] @{
        schemaVersion = 1
        sdks = @(
            [ordered] @{
                name = "csharp"
                build = New-CommandDefinition -Mode "pass" -Scenario "build"
                run = New-CommandDefinition -Mode "pass"
                scenarios = @(
                    ([ordered] @{ name = "alpha"; verify = New-CommandDefinition -Mode "pass" -Scenario "alpha-verify" } + (New-CommandDefinition -Mode "pass" -Scenario "alpha")),
                    ([ordered] @{ name = "beta"; verify = New-CommandDefinition -Mode "pass" -Scenario "beta-verify" } + (New-CommandDefinition -Mode "fail" -Scenario "beta")),
                    ([ordered] @{ name = "gamma" } + (New-CommandDefinition -Mode "pass" -Scenario "gamma"))
                )
            }
        )
    }
    $matrixPath = Write-Manifest -Name "matrix" -Manifest $matrixManifest
    $matrixResults = Join-Path $temporaryRoot "matrix-results"
    $matrixRun = Invoke-Runner -ManifestPath $matrixPath -ResultsPath $matrixResults
    Assert-Condition ($matrixRun.exitCode -eq 1) "The matrix must fail when one scenario fails."

    $matrixSummary = Get-Content -LiteralPath (Join-Path $matrixResults "summary.json") -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 16
    Assert-Condition (($matrixSummary.stages | Where-Object stage -eq "scenario-alpha").status -eq "passed") "Scenario alpha did not pass."
    Assert-Condition (($matrixSummary.stages | Where-Object stage -eq "scenario-beta").status -eq "failed") "Scenario beta did not fail."
    Assert-Condition (($matrixSummary.stages | Where-Object stage -eq "scenario-beta-verify").status -eq "passed") "Scenario beta verification was blocked by the scenario failure."
    Assert-Condition (($matrixSummary.stages | Where-Object stage -eq "scenario-gamma").status -eq "passed") "Scenario gamma was incorrectly blocked."
    Assert-Condition (Test-Path -LiteralPath (Join-Path $matrixResults "csharp-scenario-gamma.log")) "Scenario gamma log was not created."

    $filteredResults = Join-Path $temporaryRoot "filtered-results"
    $filteredRun = Invoke-Runner -ManifestPath $matrixPath -ResultsPath $filteredResults `
        -AdditionalArguments @("-Scenario", "gamma")
    Assert-Condition ($filteredRun.exitCode -eq 0) "A selected passing scenario must pass."

    $filteredSummary = Get-Content -LiteralPath (Join-Path $filteredResults "summary.json") -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 16
    Assert-Condition (($filteredSummary.stages | Where-Object stage -eq "run").status -eq "skipped") "Scenario filtering did not skip the baseline run."
    Assert-Condition (($filteredSummary.stages | Where-Object stage -eq "scenario-beta").status -eq "skipped") "Scenario filtering did not skip beta."
    Assert-Condition (($filteredSummary.stages | Where-Object stage -eq "scenario-gamma").status -eq "passed") "Scenario filtering did not execute gamma."

    $unmatchedResults = Join-Path $temporaryRoot "unmatched-results"
    $unmatchedRun = Invoke-Runner -ManifestPath $matrixPath -ResultsPath $unmatchedResults `
        -AdditionalArguments @("-Scenario", "missing-scenario")
    Assert-Condition ($unmatchedRun.exitCode -eq 1) "An unknown scenario filter must fail instead of reporting an empty success."
    Assert-Condition ($unmatchedRun.output -match "requested scenarios: missing-scenario") "The unknown scenario diagnostic is missing."

    $blockedManifest = [ordered] @{
        schemaVersion = 1
        sdks = @(
            [ordered] @{
                name = "cxx"
                build = New-CommandDefinition -Mode "build-fail" -Scenario "build"
                scenarios = @(
                    ([ordered] @{ name = "alpha"; verify = New-CommandDefinition -Mode "pass" -Scenario "alpha-verify" } + (New-CommandDefinition -Mode "pass" -Scenario "alpha")),
                    ([ordered] @{ name = "gamma" } + (New-CommandDefinition -Mode "pass" -Scenario "gamma"))
                )
            }
        )
    }
    $blockedPath = Write-Manifest -Name "blocked" -Manifest $blockedManifest
    $blockedResults = Join-Path $temporaryRoot "blocked-results"
    $blockedRun = Invoke-Runner -ManifestPath $blockedPath -ResultsPath $blockedResults
    Assert-Condition ($blockedRun.exitCode -eq 1) "A failed prerequisite must fail the SDK."

    $blockedSummary = Get-Content -LiteralPath (Join-Path $blockedResults "summary.json") -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 16
    Assert-Condition (($blockedSummary.stages | Where-Object stage -eq "scenario-alpha").status -eq "blocked") "Scenario alpha was not blocked after build failure."
    Assert-Condition (($blockedSummary.stages | Where-Object stage -eq "scenario-alpha-verify").status -eq "blocked") "Scenario alpha verification was not blocked after build failure."
    Assert-Condition (($blockedSummary.stages | Where-Object stage -eq "scenario-gamma").status -eq "blocked") "Scenario gamma was not blocked after build failure."

    $warningBuild = New-CommandDefinition -Mode "build-warning" -Scenario "build"
    $warningBuild.forbiddenPatterns = @("E2E_FAIL", "RESOURCE_FAIL", "(?:warning|警告)\s+[A-Z]+[0-9]+:")
    $warningManifest = [ordered] @{
        schemaVersion = 1
        sdks = @(
            [ordered] @{
                name = "cxx"
                build = $warningBuild
                run = New-CommandDefinition -Mode "pass"
                scenarios = @(
                    ([ordered] @{ name = "alpha" } + (New-CommandDefinition -Mode "pass" -Scenario "alpha"))
                )
            }
        )
    }
    $warningPath = Write-Manifest -Name "warning" -Manifest $warningManifest
    $warningResults = Join-Path $temporaryRoot "warning-results"
    $warningRun = Invoke-Runner -ManifestPath $warningPath -ResultsPath $warningResults
    Assert-Condition ($warningRun.exitCode -eq 1) "A zero-exit build containing an MSVC warning must fail."

    $warningSummary = Get-Content -LiteralPath (Join-Path $warningResults "summary.json") -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 16
    $warningBuildResult = $warningSummary.stages | Where-Object stage -eq "build"
    Assert-Condition ($warningBuildResult.status -eq "failed") "The warning output did not fail the build stage."
    Assert-Condition ($warningBuildResult.exitCode -eq 0) "The warning fixture must prove output validation independently of exit codes."
    Assert-Condition (($warningSummary.stages | Where-Object stage -eq "run").status -eq "blocked") "The baseline run was not blocked after the warning gate failed."
    Assert-Condition (($warningSummary.stages | Where-Object stage -eq "scenario-alpha").status -eq "blocked") "The scenario was not blocked after the warning gate failed."

    $csharpWarningBuild = New-CommandDefinition -Mode "build-warning-csharp" -Scenario "build"
    $csharpWarningBuild.forbiddenPatterns = @("E2E_FAIL", "RESOURCE_FAIL", "(?:warning|警告)\s+[A-Z]+[0-9]+:")
    $csharpWarningManifest = [ordered] @{
        schemaVersion = 1
        sdks = @(
            [ordered] @{
                name = "csharp"
                build = $csharpWarningBuild
                run = New-CommandDefinition -Mode "pass"
            }
        )
    }
    $csharpWarningPath = Write-Manifest -Name "csharp-warning" -Manifest $csharpWarningManifest
    $csharpWarningResults = Join-Path $temporaryRoot "csharp-warning-results"
    $csharpWarningRun = Invoke-Runner -ManifestPath $csharpWarningPath -ResultsPath $csharpWarningResults
    Assert-Condition ($csharpWarningRun.exitCode -eq 1) "A zero-exit C# build containing a warning must fail."

    $csharpWarningSummaryPath = Join-Path $csharpWarningResults "summary.json"
    Assert-Condition (Test-Path -LiteralPath $csharpWarningSummaryPath) "The C# warning runner did not write a summary: $($csharpWarningRun.output)"
    $csharpWarningSummary = Get-Content -LiteralPath $csharpWarningSummaryPath -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 16
    $csharpWarningBuildResult = $csharpWarningSummary.stages | Where-Object stage -eq "build"
    Assert-Condition ($csharpWarningBuildResult.status -eq "failed") "The C# warning output did not fail the build stage."
    Assert-Condition ($csharpWarningBuildResult.exitCode -eq 0) "The C# warning fixture must prove output validation independently of exit codes."
    Assert-Condition (($csharpWarningSummary.stages | Where-Object stage -eq "run").status -eq "blocked") "The C# baseline run was not blocked after the warning gate failed."

    Write-Output "SDK_VALIDATION_TEST_PASS matrix=true filtering=true unmatched=true verify-after-failure=true prerequisite-blocking=true zero-warning-gate=true csharp-zero-warning-gate=true"
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
