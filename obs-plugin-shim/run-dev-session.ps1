param(
    [string]$RepoRoot = "E:\Code\telemyapp",
    [string]$WorkspaceRoot = "E:\Code\telemyapp\telemy-v0.0.3",
    [string]$ObsRoot = "C:\Program Files (x86)\obs-studio",
    [switch]$StopExisting,
    [switch]$DisableShutdownCheck,
    [string]$SelfTestActionJson = "",
    [switch]$SelfTestDirectPluginIntake
)

$ErrorActionPreference = "Stop"

$coreExe = Join-Path $WorkspaceRoot "obs-telemetry-bridge\target\debug\obs-telemetry-bridge.exe"
$coreWd = Join-Path $WorkspaceRoot "obs-telemetry-bridge"
$obsExe = Join-Path $ObsRoot "bin\64bit\obs64.exe"
$obsWd = Split-Path $obsExe -Parent

if (-not (Test-Path -LiteralPath $coreExe)) {
    throw "Core executable not found: $coreExe"
}
if (-not (Test-Path -LiteralPath $obsExe)) {
    throw "OBS executable not found: $obsExe"
}
if (-not (Test-Path -LiteralPath $RepoRoot)) {
    throw "Repo root not found: $RepoRoot"
}

if ($StopExisting) {
    Write-Host "Stopping existing obs64 / obs-telemetry-bridge..."
    Get-Process obs64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Get-Process obs-telemetry-bridge -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

$env:AEGIS_DOCK_BRIDGE_ROOT = $RepoRoot
if ($SelfTestActionJson) {
    $env:AEGIS_DOCK_SELFTEST_ACTION_JSON = $SelfTestActionJson
    if ($SelfTestDirectPluginIntake) {
        $env:AEGIS_DOCK_SELFTEST_DIRECT_PLUGIN_INTAKE = "1"
    } else {
        $env:AEGIS_DOCK_SELFTEST_DIRECT_PLUGIN_INTAKE = "0"
    }
} else {
    Remove-Item Env:AEGIS_DOCK_SELFTEST_ACTION_JSON -ErrorAction SilentlyContinue
    Remove-Item Env:AEGIS_DOCK_SELFTEST_DIRECT_PLUGIN_INTAKE -ErrorAction SilentlyContinue
}

$core = Start-Process -FilePath $coreExe -WorkingDirectory $coreWd -PassThru
$obsArgs = @()
if ($DisableShutdownCheck) {
    $obsArgs += "--disable-shutdown-check"
}
$obs = Start-Process -FilePath $obsExe -WorkingDirectory $obsWd -ArgumentList $obsArgs -PassThru

Write-Host ""
Write-Host "Started dev session:"
Write-Host "  Core PID: $($core.Id)"
Write-Host "  OBS PID:  $($obs.Id)"
Write-Host "  AEGIS_DOCK_BRIDGE_ROOT=$RepoRoot"
if ($SelfTestActionJson) {
    Write-Host "  AEGIS_DOCK_SELFTEST_ACTION_JSON=<set>"
    Write-Host "  AEGIS_DOCK_SELFTEST_DIRECT_PLUGIN_INTAKE=$($SelfTestDirectPluginIntake.IsPresent)"
}
Write-Host ""
