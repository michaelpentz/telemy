param(
    [string]$BuildDir = "E:\Code\telemyapp\telemy-v0.0.3\obs-plugin-shim\build-obs-cef",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",
    [string]$ObsRoot = "C:\Program Files (x86)\obs-studio",
    [string]$BridgeRoot = "",
    [switch]$StopObs
)

$ErrorActionPreference = "Stop"

function Copy-IfExists {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Missing required source file: $Source"
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Resolve-BridgeSourceFile {
    param(
        [Parameter(Mandatory = $true)][string]$BridgeRootDir,
        [Parameter(Mandatory = $true)][string]$BuildAssetDir,
        [Parameter(Mandatory = $true)][string]$FileName
    )
    if ($BridgeRootDir) {
        $candidate = Join-Path $BridgeRootDir $FileName
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
        throw "BridgeRoot override set, but file is missing: $candidate"
    }
    return (Join-Path $BuildAssetDir $FileName)
}

$dllSource = Join-Path $BuildDir "$Config\aegis-obs-shim.dll"
$assetSourceDir = Join-Path $BuildDir "$Config\data\obs-plugins\aegis-obs-shim"

$dllDestDir = Join-Path $ObsRoot "obs-plugins\64bit"
$assetDestDir = Join-Path $ObsRoot "data\obs-plugins\aegis-obs-shim"
$dllDest = Join-Path $dllDestDir "aegis-obs-shim.dll"

if (-not (Test-Path -LiteralPath $ObsRoot)) {
    throw "OBS root not found: $ObsRoot"
}
if (-not (Test-Path -LiteralPath $dllDestDir)) {
    throw "OBS plugin dll dir not found: $dllDestDir"
}
if (-not (Test-Path -LiteralPath $assetSourceDir)) {
    throw "Built asset staging dir not found: $assetSourceDir"
}

if ($BridgeRoot) {
    if (-not (Test-Path -LiteralPath $BridgeRoot)) {
        throw "BridgeRoot path not found: $BridgeRoot"
    }
}

if ($StopObs) {
    Write-Host "Stopping OBS processes before deploy..."
    Get-Process obs64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
}

New-Item -ItemType Directory -Path $assetDestDir -Force | Out-Null

Copy-IfExists -Source $dllSource -Destination $dllDest
Copy-IfExists -Source (Resolve-BridgeSourceFile -BridgeRootDir $BridgeRoot -BuildAssetDir $assetSourceDir -FileName "aegis-dock-bridge.js") -Destination (Join-Path $assetDestDir "aegis-dock-bridge.js")
Copy-IfExists -Source (Resolve-BridgeSourceFile -BridgeRootDir $BridgeRoot -BuildAssetDir $assetSourceDir -FileName "aegis-dock-bridge-host.js") -Destination (Join-Path $assetDestDir "aegis-dock-bridge-host.js")
Copy-IfExists -Source (Resolve-BridgeSourceFile -BridgeRootDir $BridgeRoot -BuildAssetDir $assetSourceDir -FileName "aegis-dock-browser-host-bootstrap.js") -Destination (Join-Path $assetDestDir "aegis-dock-browser-host-bootstrap.js")
Copy-IfExists -Source (Join-Path $assetSourceDir "aegis-dock-app.js") -Destination (Join-Path $assetDestDir "aegis-dock-app.js")
Copy-IfExists -Source (Join-Path $assetSourceDir "aegis-dock.html") -Destination (Join-Path $assetDestDir "aegis-dock.html")

# Prevent stale mixed deployments: runtime may choose .global.js if left behind in older builds.
$staleGlobal = Join-Path $assetDestDir "aegis-dock-bridge.global.js"
if (Test-Path -LiteralPath $staleGlobal) {
    cmd /c del /f /q "$staleGlobal" | Out-Null
}

Write-Host ""
Write-Host "Deployed Aegis OBS shim -> $ObsRoot"
Write-Host "  DLL:   $dllDest"
Write-Host "  Assets: $assetDestDir"
if ($BridgeRoot) {
    Write-Host "  Bridge source override: $BridgeRoot"
} else {
    Write-Host "  Bridge source: $assetSourceDir"
}
Write-Host ""
Get-ChildItem -LiteralPath $assetDestDir |
    Where-Object { $_.Name -like "aegis-dock*" } |
    Sort-Object Name |
    Select-Object Name, Length, LastWriteTime |
    Format-Table -AutoSize
