# Current Status (v0.0.3)

Last updated: 2026-02-24 (US/Pacific, late, follow-up)

## Project State

- Backend/cloud side is materially ahead and live-validated in AWS mode.
- Rust app (`obs-telemetry-bridge`) has live-validated Aegis control-plane client plumbing:
  - typed `relay/active`, `relay/start`, `relay/stop`
  - startup `relay/active` probe + cached in-memory relay snapshot
  - vault-backed control-plane JWT + `[aegis]` config
  - manual CLI relay control and temporary local dashboard relay controls
- Rust core IPC v1 foundations are implemented and locally validated:
  - named-pipe server for `hello` / `ping` / `request_status` / `status_snapshot`
  - protocol-error handling, heartbeat timeout, and test coverage (duplex-based)
  - core-triggered `switch_scene` + `scene_switch_result` request tracking
- C++ plugin shim (`obs-plugin-shim/`) is scaffolded and validated against the Rust core over real Windows named pipes and real OBS plugin lifecycle:
  - handshake + heartbeat + status snapshots
  - `switch_scene` receipt + callback-mode processing (auto-ack disabled in plugin mode)
  - callback hooks for pipe/message/switch-scene notifications
  - explicit queued `scene_switch_result` (success/failure) reporting path
  - explicit queued `obs_shutdown_notice` reporting path
  - OBS plugin entry callback skeleton with OBS-thread timer pump for `switch_scene` handling
  - real OBS scene switch attempt in timer path (`obs_frontend_set_current_scene`) with active-scene verification and explicit result reporting
  - real OBS 32.0.4 callback-mode validation completed for:
    - `success` (`ok=true`)
    - `scene_not_found`
    - `missing_scene_name` (via debug endpoint `allow_empty=true`)
- Browser dashboard remains transitional (debug/control surface), not final v0.0.3 UX.
- Dock UI aesthetic target remains the workspace reference mocks:
  - `E:\Code\telemyapp\aegis-dock.jsx`
  - `E:\Code\telemyapp\aegis-dock-wide.jsx`
- Terminology alignment pass has been documented (`docs/DOCK_TERMINOLOGY_ALIGNMENT_v0.0.3.md`); UI label implementation still needs to apply it.
- Minimal dock bridge replacements are now present in repo root:
  - `aegis-dock-bridge.js` (DockState reducer/projection + IPC/local setters)
  - `aegis-dock-bridge-host.js` (host adapter entry points for IPC + OBS callbacks)
- Browser-host bootstrap is now present for future OBS dock/CEF invocation:
  - `aegis-dock-browser-host-bootstrap.js` (exposes `window.aegisDockNative.*` JSON entry points)
- OBS plugin dock-host scaffolding was expanded and validated in real OBS runtime:
  - compile-gated browser-dock scaffold module (`AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST`)
  - stable C ABI for dock JS executor registration + page lifecycle notifications
  - plugin-side replay cache for IPC/status/scene snapshot/current scene + latest switch completion
  - page-ready hook queues a fresh `request_status` (coalesced) after replay
- First real OBS dock host implementation is now present behind an additional opt-in build flag:
  - `AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE`
  - Qt WebEngine-backed `QDockWidget` host registered via `obs_frontend_add_custom_qdock(...)`
  - real JS executor registered (`QWebEnginePage::runJavaScript`) through existing shim C ABI
  - page-ready probe checks `window.aegisDockNative.*` before calling shim page-ready lifecycle
  - generated diagnostic dock page can show delivered IPC/scene events and projected bridge state
  - dock page now attempts to inline/load real repo bridge stack:
    - `aegis-dock-bridge.js`
    - `aegis-dock-bridge-host.js`
    - `aegis-dock-browser-host-bootstrap.js`
  - fallback remains available (validation page + scaffold logs) if Qt6 WebEngine or JS assets are unavailable
- Real OBS scaffold-mode plugin validation was re-run successfully in this session (OBS 32.0.4 + Rust core IPC):
  - scaffold plugin loads cleanly in OBS with no plugin-load error
  - scaffold fallback log path active (`Qt/CEF embedding TODO`)
  - IPC session/connectivity healthy (`hello_ack`, recurring `status_snapshot`, `pong`)
  - scene snapshot payload logs observed on real OBS frontend events
  - callback-mode `switch_scene` -> explicit queued/sent `scene_switch_result(ok=true)` observed
  - unload path emitted `obs_shutdown_notice` and clean disconnect
- Qt/WebEngine dock host runtime validation is currently blocked in OBS despite successful local builds:
  - opt-in Qt/WebEngine plugin build compiles and links locally
  - plugin load fails in OBS with generic `Module ... aegis-obs-shim.dll not loaded`
  - required Qt WebEngine runtime files/resources were deployed (`windeployqt`) and `dxcompiler`/`dxil` were added, but load failure persisted
  - likely root cause is Qt build incompatibility (OBS-shipped Qt 6.8.3 binaries differ from `aqtinstall` Qt 6.8.3 binaries despite same version string), causing mixed-Qt runtime incompatibility in-process
- OBS-native CEF dock host runtime path is now compile- and real-OBS-validated behind a new opt-in build flag:
  - `AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF`
  - uses OBS `obs-browser` panel APIs (`obs_browser_init_panel`, `QCef::create_widget`, `QCefWidget::executeJavaScript`)
  - deferred init retry handles plugin-load ordering (`aegis-obs-shim` can load before `obs-browser`)
  - explicit post-page-ready JS sink delivery validation logs added (one-time per page-ready cycle):
    - `receiveIpcEnvelopeJson`
    - `receiveSceneSnapshotJson`
    - `receiveSceneSwitchCompletedJson`
  - fallback payload logging now includes phase labels and sampling/noise-gating after JS sink registration:
    - `no_js_sink`
    - `pre_page_ready`
    - `post_page_ready_sink_miss`
  - first-pass build-time JS asset staging added in `obs-plugin-shim/CMakeLists.txt`:
    - stages bridge assets to `.../Release/data/obs-plugins/aegis-obs-shim/`
    - `AEGIS_DOCK_BRIDGE_ROOT` remains the preferred dev override
  - CEF shutdown path stabilized for OBS close:
    - early host shutdown on `OBS_FRONTEND_EVENT_EXIT`
    - explicit `QCefWidget::closeBrowser()` (`panel_version >= 2`)
    - no active dock removal / manual delete during shutdown (OBS owns dock teardown)
  - real bridge asset stack loads successfully via `AEGIS_DOCK_BRIDGE_ROOT=E:\Code\telemyapp`:
    - `aegis-dock-bridge.global.js`
    - `aegis-dock-bridge-host.js`
    - `aegis-dock-browser-host-bootstrap.js`
- Validation artifacts / handoff anchors for the latest real-OBS scaffold run:
  - OBS log (confirmed scaffold-mode success + IPC/scene/shutdown evidence): `C:\Users\mpent\AppData\Roaming\obs-studio\logs\2026-02-24 10-30-10.txt`
  - working scaffold plugin build output: `E:\Code\telemyapp\telemy-v0.0.3\obs-plugin-shim\build-obs-scaffold\Release\aegis-obs-shim.dll`
  - installed OBS plugin (currently scaffold-mode fallback): `C:\Program Files (x86)\obs-studio\obs-plugins\64bit\aegis-obs-shim.dll`
- Validation artifacts / handoff anchors for OBS/CEF host bring-up + stabilized shutdown:
  - first successful CEF host activation (with fallback validation page): `C:\Users\mpent\AppData\Roaming\obs-studio\logs\2026-02-24 11-27-00.txt`
  - successful CEF host activation + real bridge asset load + clean shutdown (no new crash): `C:\Users\mpent\AppData\Roaming\obs-studio\logs\2026-02-24 11-39-26.txt`
  - latest crash before shutdown fix stabilization (historical reference): `C:\Users\mpent\AppData\Roaming\obs-studio\crashes\Crash 2026-02-24 11-21-45.txt`
  - CEF plugin build output: `E:\Code\telemyapp\telemy-v0.0.3\obs-plugin-shim\build-obs-cef\Release\aegis-obs-shim.dll`
- Validation artifacts / handoff anchors for CEF host logging + real-asset-path follow-up:
  - fallback-mode validation of new JS sink delivery logs + phase-tagged fallback logs: `C:\Users\mpent\AppData\Roaming\obs-studio\logs\2026-02-24 12-08-01.txt`
  - real-asset-path validation (`mode=real_bridge_assets`) using `AEGIS_DOCK_BRIDGE_ROOT=E:\Code\telemyapp`: `C:\Users\mpent\AppData\Roaming\obs-studio\logs\2026-02-24 12-20-35.txt`
- v0.0.3 target architecture remains hybrid OBS plugin + Rust core via IPC named pipes.

## Current Priority

1. Re-center on plugin/core hybrid path.
2. Continue plugin/core hybrid validation and bridge/state integration on the working OBS/CEF host path (real OBS + Rust IPC), with scaffold build retained as fallback baseline.
3. Decide near-term runtime asset packaging strategy for dock JS assets now that the full post-page-ready JS sink delivery validation set is complete on the `real_bridge_assets` path.
4. Promote/standardize the preferred runtime asset path for validation and operator use:
   - keep `AEGIS_DOCK_BRIDGE_ROOT` for dev
   - and/or deploy staged module-data assets into OBS install for runtime stability
5. Begin plugin dock UI implementation/styling aligned to `aegis-dock.jsx` / `aegis-dock-wide.jsx`, with terminology cleanup toward v0.0.3 state-machine/operator wording.
6. Keep browser dashboard changes minimal unless needed for validation/debugging.

## Operator / Validation Notes

- Aegis control-plane host after reboot/hardening validation: `http://52.13.2.122:8080` (EC2 public IP `52.13.2.122` observed 2026-02-22 US/Pacific).
- Local client validation succeeded for auth/path using a fresh `cp_access_jwt` for `uid=usr_ec2_validation`.
- `aegis-relay-active` returned `null` at validation time (no active session; auth and endpoint path were valid).
- Local IPC/plugin-path validation succeeded with:
  - Rust `ipc_dev_client` (stand-in plugin)
  - C++ shim harness (`obs-plugin-shim/build/Debug/aegis_plugin_shim_harness.exe`)
  - dashboard-triggered `IPC Switch Scene` -> C++ shim received `switch_scene` and auto-acked `scene_switch_result(ok)`
- Real OBS plugin callback-mode validation succeeded (OBS 32.0.4 + Rust `/obs` debug endpoint):
  - `switch_scene` `success` -> explicit queued `scene_switch_result(ok=true)` observed in OBS log and Rust core log
  - `switch_scene` invalid scene -> `scene_not_found` observed in OBS log and Rust core log
  - `switch_scene` empty scene name -> `missing_scene_name` observed in OBS log and Rust core log (via `/ipc/switch-scene` debug `allow_empty=true`)
  - plugin unload -> `obs_shutdown_notice` observed in OBS log and Rust core log
- Dock UX reference prototype captured in repo root `aegis-dock.jsx` (operator-facing target look/flow for future OBS plugin dock, not production implementation).
- Additional wide-layout aesthetic reference exists at repo root `aegis-dock-wide.jsx` and should inform plugin dock layout direction where appropriate.
- `aegis-dock.jsx` is a mock/simulated UI today, but it usefully defines the intended plugin-facing state surfaces:
  - scene list + active scene + switch result
  - IPC/pipe health
  - per-link connection telemetry (signal/bitrate/status) + bonded bitrate thresholds
  - relay/cloud status (region/latency/uptime/time bank)
  - failover engine state + transitions
  - quick settings flags + event log stream
- Draft `DockState` <-> IPC/status projection and action mapping is now documented in `docs/IPC_PROTOCOL_v1.md` section 10 (coverage vs gaps identified for plugin bring-up).
- Handoff history referenced a repo-root `aegis-dock-bridge.js` as present before it existed in this checkout; a minimal replacement has now been recreated along with `aegis-dock-bridge-host.js`.
- Plugin callback path now emits a structured scene snapshot payload matching the bridge host contract and supports a pluggable emitter hook (with log fallback) pending browser-dock embedding.
- Browser-side bootstrap now exposes a stable native-call target for future CEF/JS execution:
  - `window.aegisDockNative.receiveSceneSnapshotJson(...)`
  - `window.aegisDockNative.receiveIpcEnvelopeJson(...)`
- Real OBS plugin build (with scaffold flag enabled) now validated locally:
  - `AEGIS_BUILD_OBS_PLUGIN=ON`
  - `AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST=ON`
  - linked successfully using import libs in `third_party/obs-sdk-importlibs/`
- Real OBS runtime validation with scaffold-enabled plugin succeeded:
  - scaffold logs observed (`browser dock scaffold initialize ...`, `page unloaded`, `shutdown`)
  - IPC path remained healthy (`connected`, `status_snapshot`, `pong`)
  - callback-mode scene switch still succeeded and emitted `receiveSceneSwitchCompletedJson=...`
  - unload path used new skip behavior after `EXIT` (`skipping frontend callback remove after EXIT event`)
- New dock host implementation (not yet runtime-validated in OBS at time of this update):
  - `obs-plugin-shim/src/obs_browser_dock_host_scaffold.cpp` now contains an opt-in Qt6 WebEngine dock host path
  - `obs-plugin-shim/CMakeLists.txt` adds `AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE` (Qt6 WebEngine package lookup is best-effort; fallback scaffold still builds)
  - dock host loads a generated HTML shell that inlines real bridge/host/bootstrap JS when assets resolve
  - asset resolution search includes:
    - `AEGIS_DOCK_BRIDGE_ROOT` environment variable (recommended for local dev)
    - module data paths (`obs_module_file(...)`)
    - plugin DLL directory ancestry / app dir fallbacks
  - if assets are not found/readable, host falls back to the built-in validation page and logs a warning
- Follow-up validation in this session clarified a runtime blocker for the opt-in Qt/WebEngine host path:
  - Qt/WebEngine builds succeeded against local Qt `6.8.2` and `6.8.3` installs (`aqtinstall`)
  - OBS plugin load still failed in real OBS after deploying Qt runtimes/plugins/resources and `dxcompiler`/`dxil`
  - OBS-reported Qt version matched (`6.8.3`), but OBS and local Qt DLL binaries differ (hash mismatch on `Qt6Core.dll`), indicating likely mixed-build Qt incompatibility
  - scaffold-mode build (`AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE=OFF`) remains the working path for current real-OBS validation
- OBS-native CEF host follow-up validation (this session) now succeeded in real OBS:
  - fetched/populated `third_party/obs-studio/plugins/obs-browser` submodule to access `browser-panel.hpp` / `QCefWidget`
  - added opt-in CEF host build flag `AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF`
  - CEF host uses OBS `obs-browser` panel APIs and existing shim JS executor/page lifecycle hooks
  - initial runtime issue was module-load ordering (`aegis-obs-shim` loads before `obs-browser`), resolved with deferred retry
  - shutdown crash (libcef/obs-browser `QCefWidgetInternal::closeBrowser` during OBS close) was resolved by:
    - early host shutdown on `EXIT`
    - explicit `closeBrowser()`
    - avoiding active dock removal/manual delete during shutdown
  - CEF host now cleanly loads and unloads in OBS 32.0.4, and real bridge assets load via `AEGIS_DOCK_BRIDGE_ROOT`
- Additional OBS/CEF follow-up validation (this session):
  - `obs-plugin-shim/src/obs_plugin_entry.cpp` now emits one-time post-page-ready JS sink delivery validation logs for:
    - `receiveIpcEnvelopeJson`
    - `receiveSceneSnapshotJson`
    - `receiveSceneSwitchCompletedJson`
  - fallback bridge payload logs now include phase labeling (`no_js_sink`, `pre_page_ready`, `post_page_ready_sink_miss`) and sampling/noise-gating after JS sink registration
  - `obs-plugin-shim/CMakeLists.txt` adds first-pass build-time staging for dock JS assets into a module-data-like build output path
  - real OBS validation confirmed:
    - fallback-mode path shows new validation/fallback-phase logs (`2026-02-24 12-08-01.txt`)
    - real bridge assets path shows `mode=real_bridge_assets` and post-page-ready JS sink delivery logs (`2026-02-24 12-20-35.txt`)
  - operator/runtime gotcha:
    - launching `obs64.exe` from PowerShell with the wrong working directory can fail OBS app startup with `Failed to load locale` (OBS app locale, not plugin locale)
    - use `Start-Process -WorkingDirectory (Split-Path <obs64.exe>) ...` when launching with `AEGIS_DOCK_BRIDGE_ROOT`
- Dock action transport/status follow-up (2026-02-27, local code update):
  - `request_status` dock actions now emit terminal native action-result `completed` when the next `status_snapshot` is observed (in addition to immediate `queued`)
  - `set_mode` and `set_setting` dock actions are implemented and forwarded to Rust core IPC (`set_mode_request`, `set_setting_request`) with payload validation and explicit `queued`/`rejected` action-result paths
  - completion semantics now extended for `set_mode` / `set_setting`: plugin tracks pending action requests and emits terminal `completed` when follow-on `status_snapshot` reflects the requested state, or terminal `failed` with `completion_timeout` if not observed in time
  - log-noise reduction follow-up: high-frequency shim logs for recurring `status_snapshot`/`pong` frames and unchanged theme-poll refreshes were demoted to debug-level to keep OBS logs focused on lifecycle/action outcomes
- Dock runtime stabilization follow-up (2026-02-26, real OBS validation):
  - scene list/current scene population is now stable in the visible dock without manual scene interaction
  - auto-scene-switch toggle now correctly transitions `ARMED <-> MANUAL` (fixed toggle target derivation + classic bridge projection path)
  - OBS theme colors now apply dynamically to dock panel content again via `status_snapshot.payload.theme` projection in the classic/global bridge path
  - title transport UX bug fixed: dock action forwarding still uses `__AEGIS_DOCK_ACTION__:<...>` for native delivery, but page title is restored immediately so encoded payload text no longer persists in the dock title bar
  - temporary bootstrap diagnostic event noise (`aegis:dock:action-forwarded`) was removed after transport validation
- Plugin dock/IPC integration scaffolding added in `obs-plugin-shim`:
  - `src/dock_js_bridge_api.h` (C ABI hooks for JS executor + page ready/unloaded notifications)
  - `src/obs_browser_dock_host_scaffold.{h,cpp}` (compile-gated browser-dock host scaffold module)
  - `src/obs_plugin_entry.cpp` dispatch/replay path to `window.aegisDockNative.*` (log fallback when no JS sink exists)
  - `src/ipc_client.cpp` best-effort MsgPack->JSON incoming envelope forwarding + coalesced queued `request_status`

## Known Small Follow-Up (Not Blocking Current Client/Plugin Work)

- API `relay.public_ip` may include `/32` suffix; client currently normalizes this.
- Investigate/decide whether to continue Qt/WebEngine dock-host runtime pursuit:
  - likely requires the exact Qt build/ABI used by OBS (not just matching `6.8.3` version string), or
  - pivot to an OBS-native browser/CEF host path for runtime compatibility
- Post-page-ready JS sink delivery validation set is now confirmed on the `real_bridge_assets` path (`receiveIpcEnvelopeJson`, `receiveSceneSnapshotJson`, `receiveSceneSwitchCompletedJson`) in `C:\Users\mpent\AppData\Roaming\obs-studio\logs\2026-02-24 12-20-35.txt`.
- Decide near-term runtime packaging strategy for dock JS assets:
  - deploy staged module-data assets into OBS install as the default runtime path, and/or
  - keep `AEGIS_DOCK_BRIDGE_ROOT` dev override for local OBS validation
- Replace generated diagnostic dock page with a proper `dock.html` asset (or equivalent) once runtime path is stable.
- After browser-dock embedding exists, route real OBS scene inventory/current-scene callbacks through the existing emitter/bridge host path.
- Align dock terms/labels with v0.0.3 state-machine + IPC contracts while matching `aegis-dock.jsx` / `aegis-dock-wide.jsx` aesthetic direction.

## Read Next

- `docs/README.md` (canonical docs map)
- `docs/STATE_MACHINE_v1.md`
- `docs/IPC_PROTOCOL_v1.md`
- `docs/archive/HANDOFF_STATUS.md` (handoff index / latest addenda links)
- `docs/archive/HANDOFF_HISTORY.md` (full historical handoff log)
