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
    [switch]$SkipValidate,
    [string]$SelfTestActionJson = "",
    [switch]$SelfTestDirectPluginIntake,
    [string]$ValidateRequestId = "",
    [string]$ValidateActionType = "",
    [string]$ValidateTerminalStatus = "",
    [int]$ValidateRetrySeconds = 30,
    [switch]$AllowNoUsableLog,
    [switch]$BuildDockApp,
    [string]$DockPreviewRoot = "E:\Code\telemyapp\dock-preview"
)

$ErrorActionPreference = "Stop"

$shimRoot = Join-Path $WorkspaceRoot "obs-plugin-shim"
$deployScript = Join-Path $shimRoot "deploy-to-obs.ps1"
$runScript = Join-Path $shimRoot "run-dev-session.ps1"
$validateScript = Join-Path $shimRoot "validate-obs-log.ps1"

if (-not (Test-Path -LiteralPath $shimRoot)) {
    throw "Shim root not found: $shimRoot"
}

$validateAfterTimestamp = [datetime]::MinValue

Write-Host "Aegis OBS dev cycle"
Write-Host "  Workspace: $WorkspaceRoot"
Write-Host "  BuildDir:  $BuildDir"
Write-Host "  Config:    $Config"
Write-Host "  ObsRoot:   $ObsRoot"
if ($BuildDockApp) {
    Write-Host "  DockBuild: $DockPreviewRoot"
}
Write-Host ""

if ($BuildDockApp) {
    if (-not (Test-Path -LiteralPath $DockPreviewRoot)) {
        throw "Dock preview root not found: $DockPreviewRoot"
    }
    $dockPackageJson = Join-Path $DockPreviewRoot "package.json"
    if (-not (Test-Path -LiteralPath $dockPackageJson)) {
        throw "Dock preview package.json not found: $dockPackageJson"
    }
    Write-Host "[0/4] Building dock app bundle..."
    npm run build --prefix $DockPreviewRoot
}

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
    $validateAfterTimestamp = Get-Date
    $runArgs = @{
        WorkspaceRoot = $WorkspaceRoot
        RepoRoot = $RepoRoot
        ObsRoot = $ObsRoot
        StopExisting = $true
        DisableShutdownCheck = $true
    }
    if ($SelfTestActionJson) {
        $runArgs.SelfTestActionJson = $SelfTestActionJson
        if ($SelfTestDirectPluginIntake) {
            $runArgs.SelfTestDirectPluginIntake = $true
        }
    }
    & $runScript @runArgs
}

if (-not $SkipValidate) {
    $effectiveRequestId = $ValidateRequestId
    $effectiveActionType = $ValidateActionType
    if ((-not $effectiveRequestId) -and (-not $effectiveActionType) -and $SelfTestActionJson) {
        try {
            $parsedAction = $SelfTestActionJson | ConvertFrom-Json
            if ($parsedAction.requestId) {
                $effectiveRequestId = [string]$parsedAction.requestId
            } elseif ($parsedAction.request_id) {
                $effectiveRequestId = [string]$parsedAction.request_id
            }
            if ($parsedAction.type) {
                $effectiveActionType = [string]$parsedAction.type
            }
        } catch {
            Write-Warning "Could not parse -SelfTestActionJson for validate auto-derivation: $($_.Exception.Message)"
        }
    }

    Write-Host "[4/4] Validating latest OBS log..."
    Start-Sleep -Seconds 8
    $validateArgs = @{
        RequireBridgeAssets = $true
        RequirePageReady = $true
    }
    if ($validateAfterTimestamp -gt [datetime]::MinValue) {
        $validateArgs.AfterTimestamp = $validateAfterTimestamp
    }
    if ($effectiveRequestId) {
        $validateArgs.RequestId = $effectiveRequestId
    }
    if ($effectiveActionType) {
        $validateArgs.ActionType = $effectiveActionType
    }
    if ($ValidateTerminalStatus) {
        $validateArgs.TerminalStatus = $ValidateTerminalStatus
    }

    $retryUntil = (Get-Date).AddSeconds([Math]::Max(0, $ValidateRetrySeconds))
    $attempt = 0
    while ($true) {
        $attempt++
        try {
            & $validateScript @validateArgs
            break
        } catch {
            $msg = $_.Exception.Message
            $retryable = $msg -like "*No usable OBS log found at/after*"
            if ($retryable -and (Get-Date) -ge $retryUntil -and $AllowNoUsableLog) {
                Write-Warning "Validation skipped after retry timeout: no usable OBS log for current session."
                break
            }
            if (-not $retryable -or (Get-Date) -ge $retryUntil) {
                throw
            }
            Write-Warning "Validation attempt $attempt had no usable current-session log yet; retrying..."
            Start-Sleep -Seconds 4
        }
    }
}

Write-Host ""
Write-Host "Dev cycle complete."
