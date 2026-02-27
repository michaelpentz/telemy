# OBS Aegis Plugin Shim (Skeleton)

Minimal C++ scaffold for the v0.0.3 OBS plugin shim that will talk to the Rust core over Windows named pipes.

Current scope:
- OBS-safe background worker thread lifecycle (`start` / `stop`)
- Named-pipe connect loop (`aegis_cmd_v1`, `aegis_evt_v1`)
- MessagePack envelope send/receive for `hello`, `request_status`, `ping`, and `scene_switch_result`
- Reconnect behavior for both read-side and write-side pipe failures
- IPC callback hooks for pipe-state, message-type, and `switch_scene` request notifications
- Optional `switch_scene` auto-ack toggle (enabled by default for harness/back-compat)
- Optional standalone harness target (no OBS SDK required), including a mock core named-pipe server for local IPC validation
- OBS plugin callback-mode scene-switch execution path with explicit `scene_switch_result` reporting
- OBS frontend event diagnostics for scene inventory/current-scene snapshots (`SCENE_CHANGED`, `SCENE_LIST_CHANGED`, etc.)
- OBS/CEF dock host runtime path with repo-root bridge assets and React dock app integration
- Native dock action transport (`switch_scene`, `request_status`, `set_mode`, `set_setting`) with structured action-result callbacks
- Terminal dock action completion semantics for `request_status`, `set_mode`, and `set_setting` via follow-on `status_snapshot` matching

Not implemented yet:
- Overlapped I/O (current skeleton uses blocking calls on a background thread only)
- Final release/installer packaging path for plugin + dock assets (current flow is local dev deploy script)

## Build (Standalone Harness)

```powershell
cd E:\Code\telemyapp\telemy-v0.0.3\obs-plugin-shim
cmake -S . -B build
cmake --build build --config Debug
```

Run harness:

```powershell
.\build\Debug\aegis_plugin_shim_harness.exe
```

The harness does not require OBS and is useful to validate IPC, reconnect behavior, and `switch_scene` handling while the real OBS plugin entry is being built out.

## Build + Deploy (OBS Runtime)

Build the OBS plugin target:

```powershell
cd E:\Code\telemyapp\telemy-v0.0.3\obs-plugin-shim
cmake -S . -B build-obs-cef -DAEGIS_BUILD_OBS_PLUGIN=ON -DAEGIS_ENABLE_OBS_BROWSER_DOCK_HOST=ON -DAEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF=ON
cmake --build build-obs-cef --config Release --target aegis-obs-shim
```

Deploy plugin + assets into local OBS install:

```powershell
.\deploy-to-obs.ps1 -BuildDir E:\Code\telemyapp\telemy-v0.0.3\obs-plugin-shim\build-obs-cef -Config Release -ObsRoot "C:\Program Files (x86)\obs-studio" -BridgeRoot E:\Code\telemyapp -StopObs
```

Notes:
- `-BridgeRoot` forces bridge JS files to come from repo root (recommended during active dock-bridge iteration).
- Omit `-BridgeRoot` to use build-staged bridge assets from the CMake output directory.
- If `-BridgeRoot` is omitted and workspace-root bridge files exist, deploy auto-selects workspace root bridge files before falling back to build-staged assets.

Start a local OBS + core dev session (with correct OBS working directory and bridge root):

```powershell
.\run-dev-session.ps1 -StopExisting -DisableShutdownCheck
```

If OBS still opens the crash-recovery prompt after a forced kill, dismiss it once and rerun the command.

Optional selftest action injection on page-ready:

```powershell
.\run-dev-session.ps1 -StopExisting -DisableShutdownCheck -SelfTestActionJson '{"type":"request_status","requestId":"selftest_req_status"}' -SelfTestDirectPluginIntake
```

Validate latest OBS log for startup (and optional action lifecycle):

```powershell
.\validate-obs-log.ps1 -RequireBridgeAssets -RequirePageReady
.\validate-obs-log.ps1 -RequestId selftest_set_setting_manual_override_true
.\validate-obs-log.ps1 -ActionType request_status -TerminalStatus completed -RequireTerminal
.\validate-obs-log.ps1 -RequestId selftest_set_mode_studio -ActionType set_mode -TerminalStatus completed
```

Run an end-to-end local dev cycle (build + deploy + run + validate):

```powershell
.\dev-cycle.ps1
```

Useful variants:

```powershell
.\dev-cycle.ps1 -SkipRun -SkipValidate
.\dev-cycle.ps1 -SkipBuild
.\dev-cycle.ps1 -SelfTestActionJson '{"type":"request_status","requestId":"selftest_req_status"}' -SelfTestDirectPluginIntake -ValidateTerminalStatus completed
.\dev-cycle.ps1 -SelfTestActionJson '{"type":"set_mode","mode":"studio","requestId":"selftest_set_mode_studio"}' -SelfTestDirectPluginIntake -ValidateTerminalStatus completed
```

## Harness Commands (Windows)

- `start` / `stop`
- `core-start` / `core-stop`
- `core-drop` (force active session disconnect to test reconnect)
- `core-switch <scene>` (inject `switch_scene`)
- `core-switch-missing-scene` (negative-path validation)
- `core-switch-missing-request [scene]` (no-ack path validation)
- `core-send-malformed` (decode-failure/no-crash validation)

## Recommended Local Validation Flow (Two Processes)

Use two harness processes so one acts as mock core and the other as shim runtime.

Process A:

```text
core-start
```

Process B:

```text
start
```

Then from Process A:

```text
core-switch DemoScene
core-drop
core-switch-missing-scene
core-send-malformed
```

The mock core logs inbound shim command frames and decodes `scene_switch_result` payload details (`request_id`, `ok`, `error`) for quick verification.

## Integration Hooks (Current Skeleton)

`ShimRuntime` now exposes lightweight IPC integration hooks without changing the worker-thread bring-up path:

- `SetIpcCallbacks(...)`
  - `on_pipe_state(bool connected)` for dock/health state
  - `on_message_type(string)` for diagnostics/debug counters
  - `on_switch_scene_request(request_id, scene_name, reason)` for OBS scene execution wiring
- `SetAutoAckSwitchScene(bool)`
  - defaults to `true` (harness-friendly)
  - set `false` when real OBS scene execution + explicit `scene_switch_result` reporting is implemented
- `QueueSceneSwitchResult(request_id, ok, error)`
  - thread-safe queue for reporting scene-switch completion from OBS callback code back to the IPC worker loop
- `QueueObsShutdownNotice(reason)`
  - thread-safe queue for sending plugin -> core graceful unload notice before disconnect

Threading note:
- Callbacks run on the IPC worker thread, not the OBS main thread.
- Real OBS integration should bounce work to the correct OBS thread/context before mutating OBS state.
- The current plugin entry queues `obs_shutdown_notice` on unload and waits briefly before stopping the IPC worker (best-effort graceful teardown).
- The current plugin entry also queues incoming `switch_scene` requests and drains them on an OBS timer callback (`50ms`) before sending explicit `scene_switch_result`.
- On real OBS plugin builds, frontend event callbacks log scene inventory/current-scene snapshots to support callback-mode validation and future dock bridge wiring.
- In harness mode (default `auto-ack`), missing `scene_name` with a present `request_id` now returns `scene_switch_result(ok=false, error="missing_scene_name")`.
