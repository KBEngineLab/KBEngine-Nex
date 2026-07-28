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
                    ([ordered] @{ name = "alpha" } + (New-CommandDefinition -Mode "pass" -Scenario "alpha")),
                    ([ordered] @{ name = "beta" } + (New-CommandDefinition -Mode "fail" -Scenario "beta")),
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
                    ([ordered] @{ name = "alpha" } + (New-CommandDefinition -Mode "pass" -Scenario "alpha")),
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
    Assert-Condition (($blockedSummary.stages | Where-Object stage -eq "scenario-gamma").status -eq "blocked") "Scenario gamma was not blocked after build failure."

    Write-Output "SDK_VALIDATION_TEST_PASS matrix=true filtering=true unmatched=true prerequisite-blocking=true"
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
