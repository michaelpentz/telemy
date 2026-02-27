param(
    [string]$WorkspaceRoot = "E:\Code\telemyapp\telemy-v0.0.3",
    [string]$RepoRoot = "E:\Code\telemyapp",
    [string]$ObsRoot = "C:\Program Files (x86)\obs-studio",
    [int]$ValidateRetrySeconds = 12,
    [switch]$StopWhenDone
)

$ErrorActionPreference = "Stop"

$devCycle = Join-Path $WorkspaceRoot "obs-plugin-shim\dev-cycle.ps1"
if (-not (Test-Path -LiteralPath $devCycle)) {
    throw "dev-cycle script not found: $devCycle"
}
$stopScript = Join-Path $WorkspaceRoot "obs-plugin-shim\stop-dev-session.ps1"
if (-not (Test-Path -LiteralPath $stopScript)) {
    throw "stop-dev-session script not found: $stopScript"
}

$scriptFailed = $false
try {
    & $devCycle `
        -WorkspaceRoot $WorkspaceRoot `
        -RepoRoot $RepoRoot `
        -ObsRoot $ObsRoot `
        -ConfigureObsCef `
        -BuildDockApp `
        -SkipBuild `
        -SkipDeploy `
        -ValidationProfile smoke `
        -AllowNoUsableLog `
        -ValidateRetrySeconds $ValidateRetrySeconds
} catch {
    $scriptFailed = $true
    throw
} finally {
    if ($StopWhenDone) {
        try {
            & $stopScript -ForceIfNeeded
        } catch {
            if (-not $scriptFailed) {
                throw
            }
            Write-Warning "Smoke run failed and stop-dev-session also failed: $($_.Exception.Message)"
        }
    }
}
