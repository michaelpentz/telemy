param(
    [string]$ObsLogDir = "C:\Users\mpent\AppData\Roaming\obs-studio\logs",
    [string]$RequestId = "",
    [string]$ActionType = "",
    [string]$TerminalStatus = "",
    [switch]$RequireQueued,
    [switch]$RequireTerminal,
    [switch]$RequirePageReady,
    [switch]$RequireBridgeAssets
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $ObsLogDir)) {
    throw "OBS log dir not found: $ObsLogDir"
}
if ($TerminalStatus -and @("completed", "failed", "rejected") -notcontains $TerminalStatus) {
    throw "Invalid -TerminalStatus '$TerminalStatus'. Expected one of: completed, failed, rejected."
}

$logs = Get-ChildItem -LiteralPath $ObsLogDir -File |
    Sort-Object LastWriteTime -Descending
if (-not $logs) {
    throw "No OBS logs found in: $ObsLogDir"
}
$log = $null
$content = $null
foreach ($candidate in $logs) {
    $candidateContent = Get-Content -LiteralPath $candidate.FullName
    $hasOnlyCrashNotice = $candidateContent.Count -le 3 -and
        (($candidateContent | Select-String -Pattern "Crash or unclean shutdown detected").Count -gt 0)
    if ($hasOnlyCrashNotice) {
        continue
    }
    $log = $candidate
    $content = $candidateContent
    break
}
if (-not $log) {
    throw "No usable OBS log found (latest files are crash-recovery stubs)"
}

function Assert-LogContains {
    param(
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Label
    )
    $matched = $content | Select-String -Pattern $Pattern
    if (-not $matched) {
        throw "Missing log evidence: $Label ($Pattern)"
    }
    return $matched
}

Write-Host "Validating OBS log: $($log.FullName)"

$moduleLoad = Assert-LogContains -Pattern "\[aegis-obs-shim\] module load" -Label "plugin module load"
$ipcConnected = Assert-LogContains -Pattern "\[aegis-obs-shim\] ipc pipe state: connected" -Label "IPC connected"

if ($RequireBridgeAssets) {
    $null = Assert-LogContains -Pattern "bridge assets loaded" -Label "bridge assets load"
}
if ($RequirePageReady) {
    $null = Assert-LogContains -Pattern "CEF page ready" -Label "CEF page ready"
}

if ($RequestId -or $ActionType -or $TerminalStatus -or $RequireQueued -or $RequireTerminal) {
    $basePattern = "dock action result: action_type=.*"
    if ($ActionType) {
        $basePattern = "dock action result: action_type=" + [regex]::Escape($ActionType) + "\b.*"
    }
    if ($RequestId) {
        $basePattern += "request_id=" + [regex]::Escape($RequestId) + ".*"
    }

    $terminalStatePattern = if ($TerminalStatus) { [regex]::Escape($TerminalStatus) } else { "(completed|failed|rejected)" }
    $queuedPattern = $basePattern + "status=queued"
    $terminalPattern = $basePattern + "status=" + $terminalStatePattern

    $requireQueuedMatch = $RequireQueued.IsPresent -or $RequestId
    $requireTerminalMatch = $RequireTerminal.IsPresent -or $RequestId -or [bool]$TerminalStatus

    if ($requireQueuedMatch) {
        $queued = Assert-LogContains -Pattern $queuedPattern -Label "queued action result"
        Write-Host ("  Queued:    " + $queued[-1].Line)
    }
    if ($requireTerminalMatch) {
        $terminal = Assert-LogContains -Pattern $terminalPattern -Label "terminal action result"
        Write-Host ("  Terminal:  " + $terminal[-1].Line)
    }
    if ($RequestId) {
        Write-Host ("  RequestId: " + $RequestId)
    }
    if ($ActionType) {
        Write-Host ("  Action:    " + $ActionType)
    }
}

Write-Host "Validation OK."
