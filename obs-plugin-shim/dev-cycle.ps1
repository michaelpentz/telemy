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
    [string]$DockPreviewRoot = "E:\Code\telemyapp\dock-preview",
    [switch]$ConfigureObsCef
)

$ErrorActionPreference = "Stop"

$shimRoot = Join-Path $WorkspaceRoot "obs-plugin-shim"
$deployScript = Join-Path $shimRoot "deploy-to-obs.ps1"
$runScript = Join-Path $shimRoot "run-dev-session.ps1"
$validateScript = Join-Path $shimRoot "validate-obs-log.ps1"
$configureScript = Join-Path $shimRoot "configure-obs-cef.ps1"

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
    $dockSourceJsx = Join-Path $RepoRoot "aegis-dock.jsx"
    if (-not (Test-Path -LiteralPath $dockSourceJsx)) {
        throw "Dock source JSX not found (required by dock-preview alias): $dockSourceJsx"
    }
    $dockPackageJson = Join-Path $DockPreviewRoot "package.json"
    if (-not (Test-Path -LiteralPath $dockPackageJson)) {
        throw "Dock preview package.json not found: $dockPackageJson"
    }
    Write-Host "[0/4] Building dock app bundle..."
    npm run build --prefix $DockPreviewRoot

    $stageDockDir = Join-Path $BuildDir "$Config\data\obs-plugins\aegis-obs-shim"
    $stageApp = Join-Path $stageDockDir "aegis-dock-app.js"
    $stageHtml = Join-Path $stageDockDir "aegis-dock.html"
    $repoApp = Join-Path $RepoRoot "aegis-dock-app.js"
    $repoHtml = Join-Path $RepoRoot "aegis-dock.html"
    if ((Test-Path -LiteralPath $RepoRoot) -and (Test-Path -LiteralPath $stageApp)) {
        Copy-Item -LiteralPath $stageApp -Destination $repoApp -Force
        if (Test-Path -LiteralPath $stageHtml) {
            Copy-Item -LiteralPath $stageHtml -Destination $repoHtml -Force
        }
        Write-Host "      Synced dock runtime assets to repo root for AEGIS_DOCK_BRIDGE_ROOT."
    }
}

if ($ConfigureObsCef) {
    if (-not (Test-Path -LiteralPath $configureScript)) {
        throw "Configure helper not found: $configureScript"
    }
    Write-Host "[0/4] Configuring OBS CEF plugin build..."
    & $configureScript -WorkspaceRoot $WorkspaceRoot -BuildDir $BuildDir
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
            $retryable = ($msg -like "*No usable OBS log found at/after*") -or
                         ($msg -like "*Missing log evidence:*")
            if ($retryable -and (Get-Date) -ge $retryUntil -and $AllowNoUsableLog) {
                Write-Warning "Validation skipped after retry timeout: startup evidence not fully available in current-session logs."
                break
            }
            if (-not $retryable -or (Get-Date) -ge $retryUntil) {
                throw
            }
            Write-Warning "Validation attempt $attempt missing startup evidence; retrying..."
            Start-Sleep -Seconds 4
        }
    }
}

Write-Host ""
Write-Host "Dev cycle complete."
