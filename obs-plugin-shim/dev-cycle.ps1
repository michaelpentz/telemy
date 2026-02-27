param(
    [string]$WorkspaceRoot = "E:\Code\telemyapp\telemy-v0.0.3",
    [string]$RepoRoot = "E:\Code\telemyapp",
    [string]$BuildDir = "E:\Code\telemyapp\telemy-v0.0.3\obs-plugin-shim\build-obs-cef",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",
    [string]$ObsRoot = "C:\Program Files (x86)\obs-studio",
    [switch]$SkipBuild,
    [switch]$SkipDeploy,
    [switch]$SkipRun,
    [switch]$SkipValidate
)

$ErrorActionPreference = "Stop"

$shimRoot = Join-Path $WorkspaceRoot "obs-plugin-shim"
$deployScript = Join-Path $shimRoot "deploy-to-obs.ps1"
$runScript = Join-Path $shimRoot "run-dev-session.ps1"
$validateScript = Join-Path $shimRoot "validate-obs-log.ps1"

if (-not (Test-Path -LiteralPath $shimRoot)) {
    throw "Shim root not found: $shimRoot"
}

Write-Host "Aegis OBS dev cycle"
Write-Host "  Workspace: $WorkspaceRoot"
Write-Host "  BuildDir:  $BuildDir"
Write-Host "  Config:    $Config"
Write-Host "  ObsRoot:   $ObsRoot"
Write-Host ""

if (-not $SkipBuild) {
    Write-Host "[1/4] Building plugin..."
    cmake --build $BuildDir --config $Config --target aegis-obs-shim
}

if (-not $SkipDeploy) {
    Write-Host "[2/4] Deploying plugin/assets..."
    & $deployScript -BuildDir $BuildDir -Config $Config -ObsRoot $ObsRoot -BridgeRoot $RepoRoot -StopObs
}

if (-not $SkipRun) {
    Write-Host "[3/4] Starting core + OBS..."
    & $runScript -WorkspaceRoot $WorkspaceRoot -RepoRoot $RepoRoot -ObsRoot $ObsRoot -StopExisting -DisableShutdownCheck
}

if (-not $SkipValidate) {
    Write-Host "[4/4] Validating latest OBS log..."
    Start-Sleep -Seconds 5
    & $validateScript -RequireBridgeAssets -RequirePageReady
}

Write-Host ""
Write-Host "Dev cycle complete."

