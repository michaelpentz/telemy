# Aegis IPC Protocol v1 (Plugin <-> Core)

## 1. Scope

This document defines the local IPC protocol between:
- OBS plugin shim (C++)
- Aegis core process (Rust)

Version target:
- `telemy-v0.0.3`

Transport:
- Windows Named Pipes

Serialization:
- MessagePack

---

## 2. Transport Contract

Pipe names:
- Command pipe (plugin -> core): `\\.\pipe\aegis_cmd_v1`
- Event pipe (core -> plugin): `\\.\pipe\aegis_evt_v1`

Connection model:
- Core hosts pipe server endpoints.
- Plugin connects as client after spawning core.
- Full duplex behavior is implemented with two unidirectional pipes.

I/O requirements:
- Plugin must use overlapped (async) I/O for all pipe operations.
- Plugin must not block OBS main thread on pipe I/O.
- Core uses async named pipe runtime (tokio Windows named pipe).

Timeouts:
- Connect timeout: 1500ms
- Per read/write timeout: 500ms
- Heartbeat interval: 1000ms
- Heartbeat miss threshold: 3 consecutive misses

---

## 3. Framing and Encoding

Frame format:
1. 4-byte little-endian unsigned length prefix
2. MessagePack payload bytes

Constraints:
- Max frame size: 64 KiB
- Frames above max: reject and log protocol error
- Unknown fields: ignored when safe
- Unknown message type: respond with protocol error event

All messages include envelope fields:
```json
{
  "v": 1,
  "id": "uuid-v4",
  "ts_unix_ms": 1700000000000,
  "type": "message_type",
  "priority": "critical|high|normal|low",
  "payload": {}
}
```

---

## 4. Message Types

## 4.1 Plugin -> Core (Commands)

1. `hello`
- Purpose: protocol/version handshake
- Payload:
```json
{
  "plugin_version": "0.0.3",
  "protocol_version": 1,
  "obs_pid": 1234,
  "capabilities": ["scene_switch", "dock", "restart_hint"]
}
```

2. `ping`
- Purpose: liveness probe
- Payload: `{ "nonce": "uuid-v4" }`

3. `request_status`
- Purpose: immediate status pull for dock refresh
- Payload: `{}`

4. `scene_switch_result`
- Purpose: ack outcome of scene switch attempt requested by core
- Payload:
```json
{
  "request_id": "uuid-v4",
  "ok": true,
  "error": null
}
```

5. `obs_shutdown_notice`
- Purpose: graceful shutdown signal when OBS unload starts
- Payload:
```json
{
  "reason": "obs_module_unload|obs_exit"
}
```

## 4.2 Core -> Plugin (Events/Commands)

1. `hello_ack`
- Purpose: handshake completion
- Payload:
```json
{
  "core_version": "0.0.3",
  "protocol_version": 1,
  "capabilities": ["state_machine", "chatbridge", "dashboard"]
}
```

2. `pong`
- Purpose: response to `ping`
- Payload: `{ "nonce": "uuid-v4" }`

3. `status_snapshot`
- Purpose: state/update for dock
- Payload:
```json
{
  "mode": "studio|irl",
  "health": "good|degraded|offline",
  "bitrate_kbps": 4500,
  "rtt_ms": 72,
  "override_enabled": false,
  "relay": {
    "status": "inactive|provisioning|active|grace",
    "region": "us-east-1",
    "grace_remaining_seconds": 0
  }
}
```

4. `switch_scene`
- Purpose: request plugin execute OBS scene switch
- Priority: `critical`
- Payload:
```json
{
  "request_id": "uuid-v4",
  "scene_name": "BRB",
  "reason": "auto_failover|chat_command|manual",
  "deadline_ms": 550
}
```

5. `user_notice`
- Purpose: display warning/info in dock
- Payload:
```json
{
  "level": "info|warn|error",
  "message": "Aegis engine restarted. Reconnecting..."
}
```

6. `shutdown_request`
- Purpose: ask plugin to terminate core path cleanly
- Payload:
```json
{
  "reason": "fatal_error|manual_exit|update_restart"
}
```

---

## 5. Priority and Backpressure Rules

Priority lanes:
1. `critical`: failover scene switch, shutdown coordination
2. `high`: lifecycle control and status sync
3. `normal`: regular status updates
4. `low`: diagnostics

Queue policy:
- Separate bounded queues per lane.
- On pressure, drop oldest `low` then `normal` messages first.
- Never drop `critical` messages; if blocked, trigger degraded-mode alert.

Coalescing:
- `status_snapshot` may be coalesced to latest value.
- `switch_scene` must never be coalesced or reordered.

---

## 6. Handshake and Session Lifecycle

Startup sequence:
1. Plugin spawns core.
2. Plugin attempts pipe connect (retry with backoff until 1500ms budget per attempt).
3. Plugin sends `hello`.
4. Core validates version and replies `hello_ack`.
5. Both sides enter active heartbeat loop.

Version mismatch behavior:
- If major protocol mismatch, core emits `user_notice` and closes session.
- Plugin surfaces "protocol mismatch" error in dock.

---

## 7. Liveness and Recovery

Heartbeat:
- Plugin sends `ping` every 1000ms.
- Core replies `pong` with same nonce.

Failure detection:
- 3 missed heartbeat responses => link considered stale.

Recovery behavior:
1. Plugin marks IPC degraded.
2. Plugin attempts reconnect.
3. If core process not running, perform one restart attempt.
4. If restart fails, show non-fatal dock error and stop auto-restart loop.

Reconnect semantics:
- After reconnect, plugin sends `request_status`.
- Core returns authoritative `status_snapshot`.

---

## 8. Error Handling

Protocol error event:
```json
{
  "type": "protocol_error",
  "payload": {
    "code": "frame_too_large|decode_failed|unknown_type|timeout",
    "message": "human readable detail",
    "related_message_id": "uuid-v4"
  }
}
```

Rules:
- Decode failures do not crash either side.
- Repeated protocol errors (>5 within 10s) cause controlled connection reset.

---

## 9. Security Constraints

- Named pipe ACL must restrict access to current user/session context.

---

## 10. DockState Projection (Aegis Dock UI Bridge)

This section defines how the plugin/core IPC messages project into the UI-oriented `DockState`
shape used by the prototype dock (`aegis-dock.jsx`) and future OBS dock implementation.

Purpose:
- make the dock renderer a passive view over a normalized state object
- decouple UI layout from raw IPC envelopes
- identify IPC v1 coverage vs gaps for plugin-path bring-up

### 10.1 UI Bridge Model (Conceptual)

The dock UI consumes a normalized object (`DockState`) and emits user actions:

- `dockState`: current UI projection (read model)
- `onAction(action)`: UI intent events (write model)

Bridge responsibilities (plugin-side or host-page adapter):
1. Decode IPC envelopes.
2. Cache latest relevant core/plugin state.
3. Project cached state into `DockState`.
4. Forward UI actions to core/plugin execution path.

### 10.2 `DockState` Field Mapping (v0.0.3 Current + Draft)

Status legend:
- `Available`: directly derivable from current IPC v1 messages
- `Derived`: derivable by bridge/plugin local bookkeeping
- `Gap`: not currently present in IPC v1; requires new fields/messages or local source integration

#### Header

- `header.title`
  - Source: static UI/plugin constant
  - Status: `Derived`
- `header.subtitle`
  - Source: static UI/plugin constant
  - Status: `Derived`
- `header.mode`
  - Source: `status_snapshot.payload.mode`
  - Status: `Available`
  - Values: IPC uses `studio|irl`; UI can render these directly
- `header.modes`
  - Source: static list `["studio","irl"]`
  - Status: `Derived`
- `header.version`
  - Source: `hello_ack.payload.core_version` and/or plugin version from local build metadata
  - Status: `Available` (core version), `Derived` (combined display string)

#### Live Banner

- `live.isLive`
  - Source:
    - preferred: top-level mode + health + local OBS state
    - minimum fallback: `status_snapshot.payload.health != "offline"`
  - Status: `Derived`
  - Notes: current IPC v1 does not explicitly expose "stream live" boolean; bridge must infer
- `live.elapsedSec`
  - Source:
    - preferred: OBS/plugin local stream timer or core session timer
    - fallback: relay uptime in IRL mode (if added)
  - Status: `Gap` (no explicit elapsed field in current `status_snapshot`)

#### Scenes

- `scenes.items[]`
  - Source: OBS plugin local scene enumeration (OBS API)
  - Status: `Gap` (out of IPC v1 scope today; plugin-local source)
- `scenes.activeSceneId`
  - Source: OBS plugin local current scene
  - Status: `Gap` (plugin-local source; not in current `status_snapshot`)
- `scenes.pendingSceneId`
  - Source: bridge bookkeeping for in-flight `switch_scene.request_id`
  - Status: `Derived`
  - Flow:
    - set on core -> plugin `switch_scene`
    - clear on plugin -> core `scene_switch_result`
- `scenes.autoSwitchArmed`
  - Source:
    - preferred: core config/effective automation state in `status_snapshot`
    - temporary fallback: UI/local setting mirror
  - Status: `Gap` (not in current `status_snapshot`)

#### Connections (Per-link cards)

- `connections.items[]`
  - Source:
    - local network telemetry collectors (plugin/core)
    - relay/network monitor inputs
  - Status: `Gap`
- `connections.items[].name`
  - Source: local link metadata (SIM/WiFi labels)
  - Status: `Gap`
- `connections.items[].type`
  - Source: local modem/network type metadata (5G/LTE/WiFi)
  - Status: `Gap`
- `connections.items[].signal`
  - Source: local radio/network telemetry
  - Status: `Gap`
- `connections.items[].bitrate`
  - Source: local per-link throughput telemetry
  - Status: `Gap`
- `connections.items[].status`
  - Source: local link health classification
  - Status: `Gap`

#### Bitrate Section

- `bitrate.maxPerLink`
  - Source: UI/config constant
  - Status: `Derived`
- `bitrate.maxBonded`
  - Source: UI/config constant
  - Status: `Derived`
- `bitrate.lowThresholdMbps`
  - Source: state machine/config thresholds (core)
  - Status: `Gap` (not in current `status_snapshot`)
- `bitrate.brbThresholdMbps`
  - Source: state machine/config thresholds (core)
  - Status: `Gap` (not in current `status_snapshot`)
- Bonded bitrate display value (UI computed)
  - Source: `status_snapshot.payload.bitrate_kbps` OR sum(per-link bitrate)
  - Status: `Available` (aggregate only) / `Derived`

#### Relay / Cloud (IRL mode)

- `relay.enabled`
  - Source: local config or mode capability
  - Status: `Derived`
- `relay.status`
  - Source: `status_snapshot.payload.relay.status`
  - Status: `Available`
  - Mapping:
    - `inactive` -> UI `inactive`
    - `provisioning` -> UI `connecting`
    - `active` -> UI `active`
    - `grace` -> UI `grace`
- `relay.region`
  - Source: `status_snapshot.payload.relay.region`
  - Status: `Available`
- `relay.latencyMs`
  - Source:
    - current fallback: `status_snapshot.payload.rtt_ms`
    - preferred future: relay-specific latency field
  - Status: `Available` (fallback semantics)
  - Note: current `rtt_ms` may represent OBS/local path, not relay RTT specifically
- `relay.uptimeSec`
  - Source: core session timing / relay session timestamps
  - Status: `Gap`
- `relay.instance`
  - Source: control-plane relay metadata (instance arch/type), if surfaced through core
  - Status: `Gap`
- `relay.timeBankLabel`
  - Source: control-plane quota/time-bank data, if surfaced through core
  - Status: `Gap`
- `relay.timeBankPct`
  - Source: control-plane quota/time-bank data, if surfaced through core
  - Status: `Gap`
- `relay.grace_remaining_seconds`
  - Source: `status_snapshot.payload.relay.grace_remaining_seconds`
  - Status: `Available`
  - UI usage: can feed grace countdown label/indicator if desired

#### Failover Engine

- `failover.health`
  - Source:
    - current fallback: `status_snapshot.payload.health`
    - preferred future: explicit state-machine health classification
  - Status: `Available` (coarse)
  - Mapping suggestion:
    - `good` -> `healthy`
    - `degraded` -> `degraded`
    - `offline` -> `offline`
- `failover.state`
  - Source:
    - preferred: core state machine top-level mode (`STUDIO`, `IRL_ACTIVE`, etc.) and/or compact state token
  - Status: `Gap`
  - Note: current `status_snapshot.mode` is insufficient to represent full v0.0.3 state-machine states
- `failover.states`
  - Source: UI static list or core-advertised state list
  - Status: `Derived`
- `failover.responseBudgetMs`
  - Source: config threshold/defaults
  - Status: `Gap` (can be UI constant until surfaced)
- `failover.lastFailoverLabel`
  - Source: core event history / transition log summary
  - Status: `Gap`
- `failover.totalFailoversLabel`
  - Source: core counters/session stats
  - Status: `Gap`

#### Quick Settings

- `settings.items[]`
  - Source:
    - preferred: core effective config + override flags in `status_snapshot`
    - temporary: plugin/UI local mirror
  - Status: `Gap` (except `override_enabled`, see below)
- `settings.items[*].value` (`manual override`)
  - Source: `status_snapshot.payload.override_enabled`
  - Status: `Available` (for one setting concept)
  - Note: current dock prototype labels differ; bridge should map to actual supported settings

#### Event Log

- `events[]`
  - Source:
    - `user_notice` (core -> plugin)
    - plugin local lifecycle events (connect/reconnect/scene switch outcomes)
    - optional protocol-error summaries
  - Status: `Available` (partial) / `Derived`
- `events[].time`
  - Source: envelope `ts_unix_ms`
  - Status: `Available`
- `events[].msg`
  - Source: `user_notice.payload.message` or local event formatter
  - Status: `Available` / `Derived`
- `events[].type`
  - Source:
    - `user_notice.payload.level` mapped to UI severity
    - local event classification
  - Status: `Available` / `Derived`

#### Pipe / IPC Health Footer

- `pipe.status`
  - Source: plugin heartbeat/session state machine
  - Status: `Derived`
  - Suggested mapping:
    - connected + recent `pong` -> `ok`
    - reconnecting / missed heartbeat threshold not yet exceeded -> `degraded`
    - disconnected / stale -> `down`
- `pipe.label`
  - Source: bridge formatter from `pipe.status`
  - Status: `Derived`

### 10.3 Action Mapping (`onAction` -> Protocol / Local Ops)

The dock UI emits normalized actions. The bridge maps them to protocol messages and/or plugin-local operations.

1. `set_mode`
- UI action:
```json
{ "type": "set_mode", "mode": "studio|irl" }
```
- Current mapping: `Available`
- Implemented path:
  - plugin/UI forwards to Rust core via `set_mode_request`
  - plugin validates supported values (`studio|irl`) before queueing
  - dock receives immediate native action-result (`queued`)

2. `switch_scene`
- UI action:
```json
{ "type": "switch_scene", "sceneId": "live-main", "sceneName": "Live - Main" }
```
- Current IPC mapping:
  - Manual operator switch initiated in plugin UI can be executed locally by plugin (OBS API), then optionally reported to core via new command/event
  - Existing IPC v1 `switch_scene` is core -> plugin (automated/manual core-requested)
- Current protocol gap:
  - No plugin -> core "manual switch requested" message type in v1
- Interim bridge recommendation:
  - For plugin-native manual switch button: execute OBS switch locally and emit informational event to core later when protocol adds reporting

3. `set_setting`
- UI action:
```json
{ "type": "set_setting", "key": "auto_scene_switch", "value": true }
```
- Current mapping: `Available` (supported keys only)
- Implemented path:
  - plugin/UI forwards to Rust core via `set_setting_request`
  - plugin validates key/value payload shape before queueing
  - currently recognized keys:
    - `auto_scene_switch`
    - `low_quality_fallback`
    - `manual_override`
    - `chat_bot`
    - `alerts`
  - dock receives immediate native action-result (`queued`) or explicit `rejected` reason for invalid payloads

### 10.4 Recommended Minimum IPC Additions for Dock v1.1

To support the current dock design without overexpanding scope, add the following to `status_snapshot` (or a second coalesced `dock_snapshot`) in a future protocol revision:

1. `engine_state`
- Example: `STUDIO|IRL_CONNECTING|IRL_ACTIVE|IRL_GRACE|DEGRADED|FATAL`

2. `elapsed_live_seconds`
- Stream/session elapsed timer for banner

3. `thresholds`
- `low_bitrate_mbps`
- `brb_bitrate_mbps`

4. `effective_flags`
- `auto_scene_switch`
- `low_quality_fallback`
- `manual_override`

5. `relay` enrichment
- `uptime_seconds`
- `instance_label`
- `time_bank_seconds_remaining` or display-ready summary

6. `scene_status`
- `active_scene_name`
- optional `pending_request_id` / `pending_scene_name`

7. `recent_events` (optional if `user_notice` stream buffering remains plugin-side)
- small ring buffer summary for reconnect sync

### 10.5 Current Implementation Guidance (v0.0.3)

For current plugin-path bring-up, prioritize this order:
1. `pipe.status`, `header.mode`, relay status/region, aggregate bitrate, `health`, and event log from existing IPC.
2. Scene list/active scene from plugin-local OBS APIs (not IPC).
3. Pending switch tracking from bridge bookkeeping around `switch_scene` / `scene_switch_result`.
4. Leave unsupported settings and richer relay/failover metrics read-only or placeholder until protocol expansion.
- Reject connections from unexpected SID when feasible.
- IPC payloads must never include raw long-lived secrets.
- Tokens in payloads must be masked in logs.

---

## 10. Observability

Minimum logs (both sides):
- Connection established/closed
- Handshake success/failure
- Heartbeat loss and recovery
- Scene switch request latency and result
- Queue pressure/drop counters

Recommended metrics:
- `ipc_roundtrip_ms`
- `ipc_timeouts_total`
- `ipc_reconnects_total`
- `scene_switch_ipc_path_ms`

---

## 11. Acceptance Criteria

IPC v1 is ready when:
1. Handshake, heartbeat, and reconnect flows pass integration tests.
2. Plugin main thread never blocks on pipe operations.
3. Scene switch critical messages are delivered with priority guarantees.
4. Protocol error handling is resilient under malformed frames.
5. Version mismatch behavior is user-visible and non-crashing.
