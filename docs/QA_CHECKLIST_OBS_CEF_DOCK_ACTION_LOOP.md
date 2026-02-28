# QA Checklist: OBS/CEF Dock Action Loop (v0.0.3)

This checklist provides manual verification steps for the Telemy OBS/CEF dock, focusing on state population and the action transport loop (JS -> Plugin -> Core).

## 1. Preconditions
- **OBS Version:** 32.0.4 (x64)
- **Plugin Build:** `aegis-obs-shim.dll` built with `AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF=ON`.
- **Environment:** `AEGIS_DOCK_BRIDGE_ROOT` set to the repository root (for dev/validation).
- **Rust Core:** `obs-telemetry-bridge` running (required for full pipe/telemetry verification).
- **OBS Scene Setup:** At least 2-3 scenes created in OBS for switching tests.

## 2. Dock Visibility & Startup
| Step | Action | Expected Result | Pass/Fail |
| :--- | :--- | :--- | :--- |
| 2.1 | Launch OBS 32.0.4 | Plugin loads; `Aegis Dock (Telemy)` panel appears in the UI. | |
| 2.2 | Close Dock Panel | Panel disappears from the OBS layout. | |
| 2.3 | Re-show via Menu | Navigate to `Tools -> Show Aegis Dock (Telemy)`. Dock panel reappears immediately. | |
| 2.4 | Restart OBS | Dock panel persistence check: Panel should appear where it was last placed. | |

## 3. State Population (CEF Path)
*Verify these elements are populated and updated in the visible dock UI.*
| Element | Expected Behavior | Pass/Fail |
| :--- | :--- | :--- |
| **Header** | Title is `Telemy Aegis`; Subtitle shows version or "Bridge Connected". | |
| **Mode** | Displays `STUDIO` or `IRL` (matches Core state). | |
| **Scenes List** | Lists all scenes currently defined in the OBS Scene Collection. | |
| **Active Scene** | The currently active scene in OBS is highlighted in the dock. | |
| **Event Log** | Shows bridge-level events (e.g., `ipc connected`, `handshake success`). | |
| **Pipe Health** | Shows `Pipe: OK` (Green) when Rust Core is running. | |

## 4. Manual Scene Switch Loop
*Test the end-to-end action transport (JS -> title transport -> plugin intake -> OBS apply).*
| Step | Action | Expected Result | Pass/Fail |
| :--- | :--- | :--- | :--- |
| 4.1 | Click `Switch` | Click a `Switch` button for a non-active scene in the dock. | UI shows `[SWITCHING]` badge/state. | |
| 4.2 | Log Check (Intake) | Check OBS log for `__AEGIS_DOCK_ACTION__` intercept. | Log shows `actionType: switch_scene` and `status: queued`. | |
| 4.3 | Log Check (Apply) | Check OBS log for scene apply events. | Log shows `switch_scene applying` and `obs_frontend_set_current_scene`. | |
| 4.4 | Result Receipt | Verify UI update after transition. | UI `[SWITCHING]` badge clears; target scene becomes active; `receiveDockActionResultJson` shows `completed`. | |
| 4.5 | Callback Path | Verify authoritative callback. | `receiveSceneSwitchCompletedJson` observed in logs with `ok: true`. | |

## 5. Status Request (`request_status`)
*Verify manual status polling with Rust Core running.*
| Step | Action | Expected Result | Pass/Fail |
| :--- | :--- | :--- | :--- |
| 5.1 | Trigger Request | (Trigger manual refresh if UI button exists, or via console `window.aegisDockNative.sendDockAction({type:'request_status'})`). | Native action result `status: queued` received. | |
| 5.2 | Update Flow | Observe UI/Logs. | Fresh `status_snapshot` received from Rust Core; UI elements flicker/update. | |

## 6. Mode/Setting Completion Semantics
*Verify `set_mode` / `set_setting` produce terminal action results (not just queued).*
| Step | Action | Expected Result | Pass/Fail |
| :--- | :--- | :--- | :--- |
| 6.1 | Toggle Auto Scene Switch | Click Auto Scene Switch (`ARMED <-> MANUAL`). | Action result shows `queued` then terminal `completed` (or `failed` with timeout on no snapshot convergence). | |
| 6.2 | Set Mode | Trigger mode change (`studio` / `irl`). | Action result shows `queued` then terminal `completed` once `status_snapshot.mode` matches target. | |
| 6.3 | Invalid Setting Payload | Send malformed `set_setting` via console (missing key/value). | Action result shows `rejected` with explicit parse/validation error. | |
| 6.4 | Unsupported Action | Send unknown action type via console. | Action result shows `rejected` with `unsupported_action_type`. | |

## 7. Negative & Failure Checks
| Step | Scenario | Expected Result | Pass/Fail |
| :--- | :--- | :--- | :--- |
| 7.1 | Core Down | Stop the Rust Core process. | UI shows `Pipe: DOWN`; Relay status becomes `inactive`. | |
| 7.2 | Completion Timeout | Trigger action while suppressing follow-on snapshots (debug scenario). | Terminal `failed` with `completion_timeout` observed. | |
| 7.3 | Layout Hidden | Start OBS with dock hidden by layout persistence. | Use `Tools -> Show Aegis Dock (Telemy)` to recover; verify state replays correctly. | |

## 10. Fix Verification (v0.0.3+ Patch)
| Step | Target Fix | Verification Method | Pass/Fail |
| :--- | :--- | :--- | :--- |
| 10.1 | **Idempotency Key (#4)** | Start Relay; check Go logs for `400 Bad Request`. Fix is OK if relay starts and Go receives UUID-v4. | |
| 10.2 | **IPC Pipe DACL (#1)** | Attempt to connect to named pipe from a different user account. Fix is OK if connection is denied. | |
| 10.3 | **Token Logging (#2)** | Check OBS/Core logs for dashboard URL. Fix is OK if token is truncated or absent. | |

## 8. Automated Smoke Path (Local)
- Build/deploy only:
  - `obs-plugin-shim\dev-cycle.ps1 -SkipRun -SkipValidate`
- Full local cycle:
  - `obs-plugin-shim\dev-cycle.ps1`
- Startup log validation:
  - `obs-plugin-shim\validate-obs-log.ps1 -RequireBridgeAssets -RequirePageReady`
- RequestId-specific action lifecycle validation:
  - `obs-plugin-shim\validate-obs-log.ps1 -RequestId <request_id>`

## 9. Known Temporary Behaviors (v0.0.3)
- **Transport:** The `document.title` command channel is temporary and will be replaced in future versions.
- **Completion Semantics:** `set_mode` / `set_setting` completion is snapshot-driven (terminal `completed` on state convergence, terminal `failed` on timeout).
- **Layout:** High vertical/narrow layouts may show clipping or overlapping sections (UX polish pending).
- **Action Results:** `request_status`, `set_mode`, and `set_setting` now produce terminal action results in addition to immediate `queued`.

## Evidence to Capture
*In case of failure, capture the following:*
- **Screenshot:** Dock UI showing overlapping sections or missing state.
- **OBS Log Snippet:** Full sequence from `__AEGIS_DOCK_ACTION__` to `receiveDockActionResultJson`.
- **Action JSON:** The raw payload being sent (if accessible via CEF DevTools).
