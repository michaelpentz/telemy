# Dock Terminology Alignment (v0.0.3)

## Scope

This document aligns terminology used in the prototype dock references:

- `E:\Code\telemyapp\aegis-dock.jsx`
- `E:\Code\telemyapp\aegis-dock-wide.jsx`

with the authoritative v0.0.3 specs:

- `docs/STATE_MACHINE_v1.md`
- `docs/IPC_PROTOCOL_v1.md`

Goal: produce a rename map before implementing the OBS dock bridge/UI state path.

## Authoritative Vocabulary (v0.0.3)

Use these terms as canonical in bridge code, UI state, logs, and labels:

- Top-level engine state: `STUDIO`, `IRL_CONNECTING`, `IRL_ACTIVE`, `IRL_GRACE`, `DEGRADED`, `FATAL`
- Scene intent: `LIVE`, `BRB`, `OFFLINE`, `HOLD`
- IPC status snapshot fields: `mode`, `health`, `bitrate_kbps`, `rtt_ms`, `override_enabled`, `relay.*`
- Dock bridge projection fields (conceptual): `header.mode`, `relay.status`, `failover.health`, `failover.state`, `pipe.status`

## Rename Map (Prototype -> Canonical)

## 1. Product / Header Labels

| Prototype label | Replace with | Notes |
| --- | --- | --- |
| `IRL STREAM CONTROLLER` | `OBS + Core IPC Dock` (UI subtitle) | Reflects v0.0.3 hybrid architecture, not IRL-only behavior |
| `IRL CONTROLLER` | `OBS + Core IPC Dock` | Same reason as above |
| `AEGIS v0.1.0-alpha` | `Telemy v0.0.3` or `Aegis Dock (Telemy v0.0.3)` | Avoid mismatched versioning in production UI |

## 2. Mode / State Terminology

| Prototype term | Replace with | Type | Notes |
| --- | --- | --- | --- |
| `mode = "irl"|"studio"` | keep `mode` (`studio|irl`) | IPC field | This matches `status_snapshot.payload.mode` already |
| `State: S0` / `S0..S3` | `engine_state` (`STUDIO`, `IRL_CONNECTING`, `IRL_ACTIVE`, `IRL_GRACE`, `DEGRADED`, `FATAL`) | state machine | Do not ship placeholder S-states in v0.0.3 UI |
| `simState` (`healthy/degraded/switching`) | `failover.health` + `failover.state` | UI projection | Split coarse health from actual engine state |
| `Failover Engine` (section title) | keep `Failover Engine` | UI label | Term is compatible with `failover.*` projection |

## 3. Scene / Switching Terminology

| Prototype term | Replace with | Notes |
| --- | --- | --- |
| `Auto-Switch` / `AUTO-SWITCH ARMED` | `Auto Scene Switch` / `AUTO SCENE SWITCH: ARMED` | Align with `set_setting` example key `auto_scene_switch` in IPC doc |
| `activeScene` (UI local var) | `scenes.activeSceneId` + `scenes.items[]` | Bridge projection shape in IPC doc |
| `LIVE` / `BRB` scene badges from string heuristics | Scene intent badge from `scene intent` or active scene metadata | Avoid deriving intent from scene name substring |
| `Low Quality` scene labels | `Failover Scene (Low Bitrate)` or actual OBS scene name | "Low quality" is a UI nickname, not protocol term |

## 4. Relay / Cloud Terminology

| Prototype term | Replace with | Notes |
| --- | --- | --- |
| `Cloud Relay` | `Relay` (or `Cloud Relay`) | Both acceptable; map values to canonical `relay.status` |
| Relay status badge `ACTIVE` | Render from `relay.status` mapped values: `inactive|connecting|active|grace` | IPC uses `inactive|provisioning|active|grace`; UI maps `provisioning -> connecting` |
| `Latency` (relay panel) | `Relay Latency` when sourced from relay-specific metric; otherwise `RTT (fallback)` | Current `rtt_ms` may not be relay-specific |
| `Time Bank` | keep as UI term only when backed by control-plane data | Currently a gap in IPC v1 |

## 5. Health / Connection / IPC Terminology

| Prototype term | Replace with | Notes |
| --- | --- | --- |
| `PIPE OK` | `IPC: OK/DEGRADED/DOWN` | Align with `pipe.status` projection |
| `RELAY OK` | `Relay: <status>` | Use `relay.status` instead of generic "OK" |
| `connected/degraded/disconnected` (link cards) | keep for local link card health | Valid local UI classification; not top-level engine state |
| `LIVE` banner (top) | `LIVE` only when `live.isLive == true`; otherwise mode/health banner | IPC v1 does not expose explicit live boolean |

## 6. Events / Logging Terminology

| Prototype event text | Replace with | Notes |
| --- | --- | --- |
| `Recovered -> S0` | `Engine state -> <STATE>` (example: `Engine state -> IRL_ACTIVE`) | Replace placeholder state IDs |
| `Low bitrate -> S1` | `Low bitrate detected (intent BRB)` or `Engine state -> IRL_GRACE` as applicable | Separate scene intent changes from engine state transitions |
| `Relay connected` | `Relay telemetry connected` | Matches state machine / IPC wording |
| `Aegis initialized` | `Core connected` / `IPC handshake complete` / `Dock initialized` | Prefer event source-specific wording |

## 7. UI State Naming (Recommended Bridge-Side)

Use these names in the replacement bridge (`aegis-dock-bridge.js` or equivalent):

- `dockState.header.mode`
- `dockState.live.isLive`
- `dockState.live.elapsedSec`
- `dockState.scenes.items`
- `dockState.scenes.activeSceneId`
- `dockState.scenes.pendingSceneId`
- `dockState.relay.status`
- `dockState.failover.health`
- `dockState.failover.state`
- `dockState.pipe.status`
- `dockState.events`

Avoid new bridge fields named:

- `simState`
- `S0`, `S1`, `S2`, `S3`
- `irlController`
- `autoSwitch` (prefer `autoSceneSwitch` or `scenes.autoSwitchArmed`)

## 8. Implementation Notes (v0.0.3 Constraints)

- Keep `mode` values as `studio|irl` because they already match IPC v1 `status_snapshot`.
- Add a separate `engine_state` field in a future IPC revision (recommended in `IPC_PROTOCOL_v1.md`) before rendering full state-machine chips.
- Scene inventory and current scene should come from plugin-local OBS callbacks/API, then project into `dockState.scenes.*`.
- Keep unsupported settings read-only/placeholders until protocol support exists (except `override_enabled`).

## 9. Minimum Rename Set Before UI Wiring

Apply these first to avoid confusion during bridge implementation:

1. Replace all visible `S0..S3` references with `engine_state` placeholders using canonical names.
2. Replace footer `PIPE OK` with `IPC` status terminology.
3. Replace generic `Auto-Switch` labels with `Auto Scene Switch`.
4. Replace `IRL CONTROLLER` subtitle with hybrid dock wording.
5. Treat `mode` (`studio|irl`) and `engine_state` as separate concepts everywhere.

