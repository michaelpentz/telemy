param(
    [string]$WorkspaceRoot = "E:\Code\telemyapp\telemy-v0.0.3",
    [string]$BuildDir = "E:\Code\telemyapp\telemy-v0.0.3\obs-plugin-shim\build-obs-cef",
    [string]$ObsSourceRoot = "E:\Code\telemyapp\telemy-v0.0.3\third_party\obs-studio",
    [string]$ObsImportLibDir = "E:\Code\telemyapp\telemy-v0.0.3\third_party\obs-sdk-importlibs",
    [string]$Generator = "Visual Studio 17 2022"
)

$ErrorActionPreference = "Stop"

$shimRoot = Join-Path $WorkspaceRoot "obs-plugin-shim"
$obsLibobsInclude = Join-Path $ObsSourceRoot "libobs"
$obsFrontendApiInclude = Join-Path $ObsSourceRoot "frontend\api"

if (-not (Test-Path -LiteralPath $shimRoot)) {
    throw "Shim root not found: $shimRoot"
}
if (-not (Test-Path -LiteralPath (Join-Path $obsLibobsInclude "obs-module.h"))) {
    throw "OBS include missing obs-module.h: $obsLibobsInclude"
}
if (-not (Test-Path -LiteralPath (Join-Path $obsFrontendApiInclude "obs-frontend-api.h"))) {
    throw "OBS include missing obs-frontend-api.h: $obsFrontendApiInclude"
}
if (-not (Test-Path -LiteralPath (Join-Path $ObsImportLibDir "obs.lib"))) {
    throw "OBS import lib missing: $(Join-Path $ObsImportLibDir 'obs.lib')"
}
if (-not (Test-Path -LiteralPath (Join-Path $ObsImportLibDir "obs-frontend-api.lib"))) {
    throw "OBS import lib missing: $(Join-Path $ObsImportLibDir 'obs-frontend-api.lib')"
}

Write-Host "Configuring OBS CEF build..."
Write-Host "  ShimRoot:      $shimRoot"
Write-Host "  BuildDir:      $BuildDir"
Write-Host "  ObsSourceRoot: $ObsSourceRoot"
Write-Host "  ObsImportLib:  $ObsImportLibDir"
Write-Host "  Generator:     $Generator"
Write-Host ""

$obsIncludeDirs = "$obsLibobsInclude;$obsFrontendApiInclude"
$cmakeArgs = @(
    "-S", $shimRoot,
    "-B", $BuildDir,
    "-G", $Generator,
    "-DAEGIS_BUILD_OBS_PLUGIN=ON",
    "-DAEGIS_ENABLE_OBS_BROWSER_DOCK_HOST=ON",
    "-DAEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF=ON",
    "-DOBS_INCLUDE_DIRS=$obsIncludeDirs",
    "-DOBS_LIBRARY_DIRS=$ObsImportLibDir",
    "-DOBS_LIBRARIES=obs;obs-frontend-api"
)
& cmake @cmakeArgs

Write-Host ""
Write-Host "Configure complete."
