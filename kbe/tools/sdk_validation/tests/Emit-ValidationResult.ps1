[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("pass", "fail", "build-fail")]
    [string] $Mode,

    [string] $Scenario = "baseline"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

switch ($Mode) {
    "pass" {
        Write-Output "E2E_PASS scenario=$Scenario"
        Write-Output "RESOURCE_PASS client_connections=0 server_channels=0 kcp_timers=0"
        exit 0
    }
    "fail" {
        Write-Output "E2E_FAIL scenario=$Scenario"
        exit 0
    }
    "build-fail" {
        Write-Error "Synthetic build failure"
        exit 9
    }
}
