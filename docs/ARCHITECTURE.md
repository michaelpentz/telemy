# Telemy Architecture (v0.0.3)

This document describes the hybrid architecture of Telemy v0.0.3, focusing on the integration between the Rust core, the OBS plugin shim, and the browser-based dock UI.

## System Overview

The v0.0.3 architecture is a hybrid system composed of four primary layers:

1.  **Rust Core (obs-telemetry-bridge):** The central backend process that manages Aegis control-plane integration, telemetry aggregation, and IPC server logic.
2.  **OBS Plugin Shim (obs-plugin-shim):** A C++ plugin for OBS that acts as the bridge between OBS internals and the Rust core. It handles the named-pipe IPC and hosts the dock UI.
3.  **OBS/CEF Dock Host:** The active runtime path for the UI. It uses OBS's native Chromium Embedded Framework (CEF) support to host the React-based dock.
4.  **Browser Bridge:** A set of JavaScript files (aegis-dock-bridge.js, aegis-dock-bridge-host.js, aegis-dock-browser-host-bootstrap.js) that expose a stable native API (window.aegisDockNative) to the React UI.

## Data and Control Flow

### 1. Status and Telemetry (Downstream)
- **Flow:** Rust Core IPC -> Plugin Shim -> Dock JS Callbacks.
- The Rust core pushes status_snapshot and ipc_envelope messages over a Windows named pipe.
- The plugin shim receives these via a worker thread, caches the latest state, and dispatches them to the dock via JavaScript execution.
- **Key Callbacks:**
    - receiveIpcEnvelopeJson(json)
    - receiveSceneSnapshotJson(json)

### 2. UI Actions and Command Control (Upstream)
- **Flow:** Dock UI -> sendDockAction(...) -> document.title Transport -> CEF Host -> Plugin Intake -> Rust Core.
- Since direct JS-to-C++ invocation in CEF can be restrictive, the bridge uses a **temporary document.title command channel**.
- JSON actions are percent-encoded and prefixed with __AEGIS_DOCK_ACTION__:.
- The CEF host intercepts the titleChanged event, decodes the JSON, and forwards it to the plugin's intake C ABI.
- **Native Results:** The plugin emits receiveDockActionResultJson to acknowledge receipt (queued or rejected).

### 3. Scene Switch Lifecycle
- **Flow:** switch_scene Action -> Plugin -> OBS API -> Completion Callback.
- Scene switches are queued to the OBS-thread handler.
- Authoritative completion is reported back to the dock via receiveSceneSwitchCompletedJson only after OBS confirms the scene transition.

### 4. Per-Link Relay Telemetry (Planned)
- **Flow:** Aegis Relay (AWS) -> Control Plane (Go) -> Rust Core -> IPC -> OBS Plugin -> Dock UI.
- Surfaces individual bonded link health (bitrate, RTT, loss, jitter) from the relay ingest back into the OBS dock.
- Leverages the existing `connections.items[]` bridge contract, mapping relay links directly to dock rows.

### 5. Multi-Encode / Multi-Upload Telemetry (Planned)
- **Flow:** OBS Outputs -> MetricsHub -> IPC Expansion -> Bridge -> Dock UI.
- Expands the IPC `status_snapshot` to carry an array of per-output metrics instead of a single aggregate bitrate.
- The Dock JS Bridge projects this into grouped encoder views (e.g., Horizontal vs. Vertical) with hide/show toggles for inactive outputs (Recording, Virtual Camera).

## Bridge/Browser Contract (window.aegisDockNative)

The UI interacts with the system through a stable interface exposed by the bridge.

### Methods
- getState(): Returns the current projected DockState. Note that this is a **nested** structure (e.g., header.mode, connections.items).
- sendDockAction(action): Sends an action object (e.g., { type: "switch_scene", sceneName: "Main" }).
- sendDockActionJson(json): JSON string variant of the above.

### Inbound Callbacks (from Plugin)
- receiveDockActionResultJson(json): Reports if an action was queued or rejected.
- receiveSceneSwitchCompletedJson(json): Authoritative scene switch result.

## Runtime Path and Asset Loading

### Active Path: OBS/CEF
- **Status:** Primary and validated runtime.
- **Reason:** Provides the best compatibility with OBS's internal browser panels.

### Blocked Path: Qt/WebEngine
- **Status:** Compiled but runtime-blocked.
- **Reason:** Incompatibility between OBS-shipped Qt binaries and standard Qt SDK binaries (ABI mismatch).

### Asset Resolution
Assets are loaded using the following priority:
1.  AEGIS_DOCK_BRIDGE_ROOT (Environment Variable): Preferred for development.
2.  **Staged Module-Data Assets:** Default for distribution, located in the plugin's data directory.
3.  **Packaged aegis-dock.html:** The primary entry point for the dock UI.

## Known Limitations and Placeholders
- **Transport Character Limits:** The `document.title` transport path may be subject to character limits. While sufficient for current validation, extremely long scene names or complex action payloads may be truncated.
- **Dual-Callback Redundancy:** For dock-originated scene switches, both `receiveDockActionResultJson` (with `completed` status) and `receiveSceneSwitchCompletedJson` are emitted. UI logic must handle this redundantly or idempotently.
- **Per-Link Telemetry:** Currently limited to aggregate bitrate and signal; richer per-link data is pending IPC v2 schema finalization.
- **Engine State:** failover.state is currently derived in the bridge (inferEngineState) rather than being a first-class IPC field.

## Next Engineering Milestones
1.  **Backend/Plugin:** Validate full dock action loop end-to-end in real OBS sessions.
2.  **Integration:** Expand native action handling to include set_mode and set_setting.
3.  **UX:** Transition aegis-dock.jsx to use the real nested DockState instead of simulated fallbacks.
4.  **Packaging:** Finalize the runtime asset staging script for stable distribution.
