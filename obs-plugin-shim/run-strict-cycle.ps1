param(
    [string]$WorkspaceRoot = "E:\Code\telemyapp\telemy-v0.0.3",
    [string]$RepoRoot = "E:\Code\telemyapp",
    [string]$ObsRoot = "C:\Program Files (x86)\obs-studio",
    [int]$ValidateRetrySeconds = 30
)

$ErrorActionPreference = "Stop"

$devCycle = Join-Path $WorkspaceRoot "obs-plugin-shim\dev-cycle.ps1"
if (-not (Test-Path -LiteralPath $devCycle)) {
    throw "dev-cycle script not found: $devCycle"
}

& $devCycle `
    -WorkspaceRoot $WorkspaceRoot `
    -RepoRoot $RepoRoot `
    -ObsRoot $ObsRoot `
    -ConfigureObsCef `
    -BuildDockApp `
    -SkipBuild `
    -SkipDeploy `
    -ValidationProfile strict `
    -ValidateRetrySeconds $ValidateRetrySeconds
