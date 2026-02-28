# Aegis Control Plane API Spec v1

## 1. Scope

This spec defines the Phase 2+ cloud API for `telemy-v0.0.3`.

Implementation status note (workspace snapshot audited 2026-02-22, US/Pacific):
- This document is the target v1 contract.
- The current local `aegis-control-plane` implementation does not yet enforce all items below.
- Known gaps include client-version/platform headers, rate limiting/response headers, and some error-envelope fields (`request_id`, `details`).

Focus:
- Authentication and authorization
- Relay lifecycle (`start`, `stop`, `active`)
- Idempotency guarantees
- Session state model
- Usage and reconciliation data contracts

Base path:
- `/api/v1`

Transport:
- HTTPS only (TLS 1.2+)
- JSON request/response

Current implementation note:
- The Go API server listens on plain HTTP and assumes TLS termination happens upstream (reverse proxy / load balancer).

---

## 2. Authentication Model

Credentials:
1. `cp_access_jwt`:
- Used by Rust core for control-plane API calls.
- Sent as `Authorization: Bearer <jwt>`.

2. `pair_token`:
- Not valid for control-plane endpoints.
- Ingest-only credential for relay path.

3. `relay_ws_token`:
- Not valid for control-plane endpoints.
- Relay telemetry auth only.

Hard rule:
- Any use of `pair_token` or `relay_ws_token` on control-plane endpoints returns `401`.

---

## 3. Common Headers

Required on authenticated endpoints:
- `Authorization: Bearer <cp_access_jwt>`
- `X-Aegis-Client-Version: 0.0.3`
- `X-Aegis-Client-Platform: windows`

Required for relay start:
- `Idempotency-Key: <uuid-v4>`

Optional tracing:
- `X-Request-ID: <uuid-v4>`

Current implementation note:
- Only `Authorization` and `Idempotency-Key` (for `POST /relay/start`) are enforced in code today.
- `X-Aegis-Client-Version`, `X-Aegis-Client-Platform`, and `X-Request-ID` are not currently validated or echoed.

---

## 4. Resource Model

## 4.1 Relay Session

```json
{
  "session_id": "ses_01JABCDEF...",
  "user_id": "usr_01JABCDEF...",
  "status": "provisioning|active|grace|stopped",
  "region": "us-east-1",
  "relay": {
    "instance_id": "i-0abc123...",
    "public_ip": "203.0.113.10",
    "srt_port": 9000,
    "ws_url": "wss://203.0.113.10:7443/telemetry"
  },
  "credentials": {
    "pair_token": "A1B2C3D4",
    "relay_ws_token": "eyJhbGciOi..."
  },
  "timers": {
    "grace_window_seconds": 600,
    "max_session_seconds": 57600
  },
  "usage": {
    "started_at": "2026-02-21T20:00:00Z",
    "ended_at": null,
    "duration_seconds": 0
  }
}
```

---

## 5. Endpoints

## 5.1 POST `/api/v1/relay/start`

Start or return an active relay session for the authenticated user.

Idempotency:
- `Idempotency-Key` is required.
- Same user + same key returns same session response for TTL window.
- If user already has active/provisioning session and key differs, return existing active/provisioning session (no duplicate provisioning).

Request body:
```json
{
  "region_preference": "auto|us-east-1|eu-west-1",
  "client_context": {
    "obs_connected": true,
    "mode": "studio|irl",
    "requested_by": "dashboard|chatbridge"
  }
}
```

Success responses:
- `200 OK` (existing session returned)
- `201 Created` (new session created)

Response body:
```json
{
  "session": {
    "session_id": "ses_01JABCDEF...",
    "status": "provisioning|active",
    "region": "us-east-1",
    "relay": {
      "public_ip": "203.0.113.10",
      "srt_port": 9000,
      "ws_url": "wss://203.0.113.10:7443/telemetry"
    },
    "credentials": {
      "pair_token": "A1B2C3D4",
      "relay_ws_token": "eyJhbGciOi..."
    },
    "timers": {
      "grace_window_seconds": 600,
      "max_session_seconds": 57600
    }
  }
}
```

Error responses:
- `400` invalid payload
- `401` invalid/missing JWT
- `403` tier/entitlement denied
- `409` illegal state transition
- `429` rate limited
- `500` internal error

## 5.2 GET `/api/v1/relay/active`

Return active or provisioning session for authenticated user.

Response:
- `200 OK` with session
- `204 No Content` if none exists

Example `200`:
```json
{
  "session": {
    "session_id": "ses_01JABCDEF...",
    "status": "active",
    "region": "us-east-1",
    "relay": {
      "public_ip": "203.0.113.10",
      "srt_port": 9000,
      "ws_url": "wss://203.0.113.10:7443/telemetry"
    },
    "credentials": {
      "pair_token": "A1B2C3D4",
      "relay_ws_token": "eyJhbGciOi..."
    },
    "timers": {
      "grace_remaining_seconds": 0,
      "max_session_remaining_seconds": 54000
    },
    "usage": {
      "started_at": "2026-02-21T20:00:00Z",
      "duration_seconds": 3600
    }
  }
}
```

## 5.3 POST `/api/v1/relay/stop`

Idempotently stop a relay session.

Request body:
```json
{
  "session_id": "ses_01JABCDEF...",
  "reason": "user_requested|shutdown|admin_forced|error_recovery"
}
```

Rules:
- Repeated calls with same `session_id` return success.
- If session already `stopped`, return terminal state.

Response `200`:
```json
{
  "session_id": "ses_01JABCDEF...",
  "status": "stopped",
  "stopped_at": "2026-02-21T21:15:00Z"
}
```

## 5.4 GET `/api/v1/relay/manifest`

Return launchable region and AMI metadata for relay provisioning.

Response `200`:
```json
{
  "regions": [
    {
      "region": "us-east-1",
      "ami_id": "ami-0123abcd",
      "default_instance_type": "t4g.small",
      "updated_at": "2026-02-21T18:00:00Z"
    },
    {
      "region": "eu-west-1",
      "ami_id": "ami-0456efgh",
      "default_instance_type": "t4g.small",
      "updated_at": "2026-02-21T18:00:00Z"
    }
  ]
}
```

---

## 6. Session State Machine (Backend)

States:
- `provisioning`
- `active`
- `grace`
- `stopped`

Valid transitions:
- `provisioning -> active`
- `active -> grace`
- `grace -> active`
- `active -> stopped`
- `grace -> stopped`

Invalid transitions return `409 conflict` with `invalid_transition`.

---

## 7. Idempotency Semantics

Header:
- `Idempotency-Key` must be UUIDv4 format.

Retention:
- Backend stores key mapping for 1 hour.

Behavior:
- Same user + same key + same endpoint returns original success payload.
- Same key with materially different body returns `409 idempotency_mismatch`.

---

## 8. Error Contract

Standard error body:
```json
{
  "error": {
    "code": "invalid_request",
    "message": "Human-readable summary",
    "request_id": "9f27b2ea-4bf2-4b87-9c5f-2ea59a4b8a38",
    "details": {}
  }
}
```

Current implementation note:
- The Go server returns `error.code` and `error.message`.
- `request_id` and structured `details` are not currently populated.

Canonical error codes:
- `invalid_request`
- `unauthorized`
- `forbidden`
- `not_found`
- `conflict`
- `invalid_transition`
- `idempotency_mismatch`
- `rate_limited`
- `internal_error`

---

## 9. Usage and Billing Endpoints (Minimal v1)

## 9.1 GET `/api/v1/usage/current`

Returns current cycle usage for Time Bank model.

Response `200`:
```json
{
  "plan_tier": "starter|standard|pro",
  "cycle_start": "2026-02-01T00:00:00Z",
  "cycle_end": "2026-03-01T00:00:00Z",
  "included_seconds": 54000,
  "consumed_seconds": 12600,
  "remaining_seconds": 41400,
  "overage_seconds": 0
}
```

## 9.2 POST `/api/v1/relay/health` (relay internal)

Used by relay service to report liveness and billing reconciliation data.

Auth:
- Relay service credential (not client JWT).

Request body:
```json
{
  "session_id": "ses_01JABCDEF...",
  "instance_id": "i-0abc123...",
  "ingest_active": true,
  "egress_active": true,
  "session_uptime_seconds": 1820,
  "observed_at": "2026-02-21T20:30:20Z"
}
```

Purpose:
- Watchdog safety checks (C1).
- Outage true-up using `session_uptime_seconds`.

---

## 10. Rate Limits (v1 Defaults)

- `POST /relay/start`: 6 per minute per user.
- `POST /relay/stop`: 20 per minute per user.
- `GET /relay/active`: 60 per minute per user.
- `GET /usage/current`: 30 per minute per user.

Responses include:
- `X-RateLimit-Limit`
- `X-RateLimit-Remaining`
- `X-RateLimit-Reset`

Current implementation note:
- Rate limiting and `X-RateLimit-*` headers are specified targets and are not yet implemented in the local Go server.

---

## 11. Versioning and Compatibility

- Breaking changes require `/api/v2`.
- Additive fields are allowed in v1.
- Unknown response fields must be ignored by clients.
- Rust core must send `X-Aegis-Client-Version` for server-side compatibility policy.

---

## 12. Per-Link Relay Telemetry (Planned)

### Purpose

When a streamer bonds multiple cellular/WiFi connections (e.g., T-Mobile 5G + Verizon 5G + venue WiFi) through the aegis relay, OBS currently only sees the bonded output. This feature surfaces per-link ingest health from the relay back to OBS so the streamer can see individual link quality in the dock.

### Data Flow

```
Phone (T-Mo + VZW + WiFi)
    │  SRT per-link feeds
    ▼
Aegis Relay (AWS, ephemeral)
    │  bonds feeds, tracks per-link health
    │  pushes per-link stats via ws_url or relay/health
    ▼
Control Plane (Go)
    │  aggregates per-link stats per session
    │  exposes via API to Rust core
    ▼
Rust Core (obs-telemetry-bridge)
    │  includes per-link stats in status_snapshot
    │  pushes via IPC to plugin
    ▼
OBS Plugin → Dock JS Bridge → Aegis Dock UI
    streamer sees per-link + bonded health
```

### 12.1 Relay → Control Plane: Per-Link Stats Push

Extend `POST /api/v1/relay/health` (section 9.2) or add a new endpoint for the relay to push per-link SRT telemetry.

Proposed addition to relay health payload:
```json
{
  "session_id": "ses_01JABCDEF...",
  "instance_id": "i-0abc123...",
  "ingest_active": true,
  "egress_active": true,
  "session_uptime_seconds": 1820,
  "observed_at": "2026-02-21T20:30:20Z",
  "links": [
    {
      "link_id": "link_01",
      "label": "T-Mobile 5G",
      "source_ip": "100.64.x.x",
      "protocol": "srt",
      "status": "active",
      "bitrate_kbps": 8200,
      "rtt_ms": 42,
      "packet_loss_pct": 0.1,
      "jitter_ms": 3.2,
      "connected_at": "2026-02-21T20:00:05Z"
    },
    {
      "link_id": "link_02",
      "label": "Verizon 5G",
      "source_ip": "100.65.x.x",
      "protocol": "srt",
      "status": "active",
      "bitrate_kbps": 5100,
      "rtt_ms": 55,
      "packet_loss_pct": 0.3,
      "jitter_ms": 5.1,
      "connected_at": "2026-02-21T20:00:03Z"
    },
    {
      "link_id": "link_03",
      "label": "Venue WiFi",
      "source_ip": "192.168.1.x",
      "protocol": "srt",
      "status": "degraded",
      "bitrate_kbps": 1300,
      "rtt_ms": 120,
      "packet_loss_pct": 2.4,
      "jitter_ms": 18.7,
      "connected_at": "2026-02-21T20:00:08Z"
    }
  ],
  "bonded": {
    "total_bitrate_kbps": 14600,
    "active_link_count": 3,
    "health": "healthy"
  }
}
```

Per-link fields:
- `link_id`: stable identifier for the link within the session
- `label`: human-readable name (carrier/network name if detectable, otherwise positional)
- `source_ip`: ingest source IP (for correlation; may be masked in API responses)
- `protocol`: always `srt` for now
- `status`: `active` | `degraded` | `disconnected`
- `bitrate_kbps`: current instantaneous upload bitrate for this link
- `rtt_ms`: round-trip time
- `packet_loss_pct`: packet loss percentage (0-100)
- `jitter_ms`: interarrival jitter
- `connected_at`: when this link first connected

Bonded aggregate:
- `total_bitrate_kbps`: sum of all active links
- `active_link_count`: number of links with status != disconnected
- `health`: `healthy` | `degraded` | `critical` (derived from link statuses)

### 12.2 Control Plane → Rust Core: Per-Link Stats API

New endpoint or extension to `GET /api/v1/relay/active`:

Option A — Extend `/relay/active` response with `links[]` and `bonded` fields (additive, v1-compatible).

Option B — New `GET /api/v1/relay/telemetry` endpoint polled by Rust core at 1-2s intervals.

Recommended: **Option A** (fewer endpoints, already polled by Rust core for session status).

### 12.3 Rust Core → Dock Bridge: IPC Mapping

The `status_snapshot` IPC frame should include per-link data so the bridge can project it into `getState().connections.items[]`:

```js
// Existing dock bridge contract (connections.items[])
{
  name: "T-Mobile 5G",       // from link.label
  type: "srt",               // from link.protocol
  signal: 4,                 // derived from rtt_ms + packet_loss_pct (0-5 scale)
  bitrate: 8200,             // from link.bitrate_kbps
  status: "active"           // from link.status
}
```

Additional fields to gate on existence (future):
- `rtt_ms`, `packet_loss_pct`, `jitter_ms` — if bridge exposes them, dock can show detailed per-link metrics

### 12.4 Scaling Considerations

The relay must handle dynamic link count (1 to 14+ simultaneous SRT feeds):
- `links[]` array is unbounded but practically limited by relay instance capacity
- Control plane should not assume a fixed link count
- Dock UI already renders a scrollable connection list — no hard limit on display

### 12.5 Auto-Scaling Relay Capacity

When bonded link count grows (e.g., multi-camera production at a convention), the relay instance type may need to scale:
- Control plane should track `active_link_count` from health reports
- Future: trigger instance resize or migration when link count exceeds instance capacity threshold
- Current v1 scope: single instance per session, instance type selected at provisioning time

---

## 13. Multi-Encode / Multi-Upload Per-Output Telemetry (Planned)

### Purpose

Streamers running multi-encode setups (e.g., horizontal 1920x1080 for Twitch/Kick/YouTube + vertical 1080x1920 for TikTok/YT Shorts) need per-encoder and per-upload health visible in the dock. Today, the Rust core collects full per-output metrics (bitrate, FPS, drop%, lag) via OBS WebSocket, and Grafana gets per-output histograms — but IPC v1 reduces everything to a single aggregate bitrate. The dock never sees per-output detail.

### Current State (v0.0.3)

**What works:**
- `MetricsHub` collects per-output stats every 500ms via `client.outputs().list()` + `client.outputs().status(name)`
- `StreamOutput` struct: `{ name, bitrate_kbps, drop_pct, fps, encoding_lag_ms }`
- `GrafanaExporter` pushes per-output histograms tagged by output name
- `output_names` config HashMap allows renaming OBS output IDs to display names
- v0.0.1 HTML dashboard renders per-output cards with bitrate bars

**What's lost in IPC:**
```rust
// ipc/mod.rs line 475-479 — all per-output detail crushed to one number
let bitrate_kbps = frame.streams.iter()
    .map(|s| s.bitrate_kbps)
    .fold(0u32, |acc, v| acc.saturating_add(v));
```

**Bridge is ready but unfed:**
- `aegis-dock-bridge.js` has `normalizeOutputBitrates()` expecting per-output arrays
- Projects to `getState().bitrate.outputs[]` with `{ platform, kbps, status }`
- Currently receives empty array

### 13.1 StreamOutput Expansion

Extend `StreamOutput` (model/mod.rs) with encoder and visibility metadata:

```rust
pub struct StreamOutput {
    pub name: String,              // OBS output ID (e.g., "adv_stream")
    pub display_name: String,      // User-configured name (e.g., "Twitch")
    pub active: bool,              // Whether this output is currently streaming
    pub bitrate_kbps: u32,
    pub drop_pct: f32,
    pub fps: f32,
    pub encoding_lag_ms: f32,
    // New fields:
    pub encoder_name: Option<String>,    // e.g., "x264", "nvenc", "obs_x264"
    pub encoder_group: Option<String>,   // User-assigned group (e.g., "Horizontal", "Vertical")
    pub resolution: Option<String>,      // e.g., "1920x1080", "1080x1920" (from output settings)
    pub hidden: bool,                    // User toggle to hide from dock display
}
```

**Encoder detection strategy:**
- OBS WebSocket v5 `GetOutputSettings` may expose encoder ID per output
- If not available, fall back to config-based mapping in `config.toml`
- Resolution can be inferred from output settings or encoder config

### 13.2 Config Schema for Output Metadata

Extend `config.toml` to support per-output display config:

```toml
# Existing: simple name mapping
# [output_names]
# adv_stream = "Twitch"

# New: rich per-output config
[[outputs]]
obs_id = "adv_stream"
display_name = "Twitch"
encoder_group = "Horizontal"
hidden = false

[[outputs]]
obs_id = "adv_stream_2"
display_name = "Kick"
encoder_group = "Horizontal"
hidden = false

[[outputs]]
obs_id = "adv_stream_3"
display_name = "YT Horizontal"
encoder_group = "Horizontal"
hidden = false

[[outputs]]
obs_id = "adv_stream_4"
display_name = "TikTok"
encoder_group = "Vertical"
hidden = false

[[outputs]]
obs_id = "adv_stream_5"
display_name = "YT Shorts"
encoder_group = "Vertical"
hidden = false

[[outputs]]
obs_id = "virtualcam_output"
display_name = "Virtual Camera"
hidden = true

[[outputs]]
obs_id = "adv_file_output"
display_name = "Recording"
hidden = true
```

**StreamElements output ID notes:**
- Twitch: typically `main` or `adv_stream`
- Kick: typically `kick`
- TikTok, YT Horizontal, YT Vertical: random/opaque IDs assigned by StreamElements
- Users must manually map these IDs to display names in config (or via settings UI)
- Future: auto-detect via StreamElements API integration if available

### 13.3 IPC v1 Expansion: Per-Output Array in StatusSnapshot

Add `outputs` array to `StatusSnapshotPayload`:

```rust
#[derive(Debug, Clone, Serialize, Deserialize)]
struct StatusSnapshotPayload {
    mode: SnapshotMode,
    state_mode: StateModeV1,
    health: SnapshotHealth,
    bitrate_kbps: u32,                     // Keep aggregate for backward compat
    rtt_ms: u32,
    override_enabled: bool,
    relay: RelaySnapshot,
    #[serde(skip_serializing_if = "Option::is_none")]
    settings: Option<StatusSnapshotSettingsPayload>,
    // NEW: per-output detail
    #[serde(skip_serializing_if = "Option::is_none")]
    outputs: Option<Vec<SnapshotOutput>>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct SnapshotOutput {
    id: String,                            // OBS output ID
    name: String,                          // Display name from config
    active: bool,
    bitrate_kbps: u32,
    drop_pct: f32,
    fps: f32,
    encoding_lag_ms: f32,
    #[serde(skip_serializing_if = "Option::is_none")]
    encoder: Option<String>,               // Encoder name if detected
    #[serde(skip_serializing_if = "Option::is_none")]
    group: Option<String>,                 // Encoder group from config
    #[serde(skip_serializing_if = "Option::is_none")]
    resolution: Option<String>,            // Output resolution if detected
    hidden: bool,
}
```

This is additive — existing bridge code reading `bitrate_kbps` still works. The `outputs` array is optional so older plugins that don't parse it are unaffected.

### 13.4 Bridge Contract: `getState().outputs`

New top-level section in the bridge projection:

```js
{
  outputs: {
    groups: [
      {
        name: "Horizontal",               // from encoder_group config
        encoder: "x264",                   // from first output in group (if available)
        resolution: "1920x1080",           // from first output in group
        totalBitrateKbps: 18500,           // sum of active outputs in group
        avgLagMs: 2.1,                     // avg encoding_lag_ms across group
        items: [
          {
            id: "adv_stream",
            name: "Twitch",
            active: true,
            bitrateKbps: 6200,
            dropPct: 0.01,
            fps: 60.0,
            lagMs: 2.1,
            status: "healthy"              // derived: healthy/degraded/offline
          },
          {
            id: "adv_stream_2",
            name: "Kick",
            active: true,
            bitrateKbps: 6100,
            dropPct: 0.02,
            fps: 60.0,
            lagMs: 2.0,
            status: "healthy"
          },
          {
            id: "adv_stream_3",
            name: "YT Horizontal",
            active: true,
            bitrateKbps: 6200,
            dropPct: 0.01,
            fps: 60.0,
            lagMs: 2.2,
            status: "healthy"
          }
        ]
      },
      {
        name: "Vertical",
        encoder: "x264",
        resolution: "1080x1920",
        totalBitrateKbps: 8400,
        avgLagMs: 3.0,
        items: [
          {
            id: "adv_stream_4",
            name: "TikTok",
            active: true,
            bitrateKbps: 4200,
            dropPct: 0.03,
            fps: 30.0,
            lagMs: 3.1,
            status: "healthy"
          },
          {
            id: "adv_stream_5",
            name: "YT Shorts",
            active: true,
            bitrateKbps: 4200,
            dropPct: 0.02,
            fps: 30.0,
            lagMs: 2.9,
            status: "healthy"
          }
        ]
      }
    ],
    hidden: [
      { id: "virtualcam_output", name: "Virtual Camera", active: false },
      { id: "adv_file_output", name: "Recording", active: false }
    ],
    totalBitrateKbps: 26900,
    activeCount: 5,
    hiddenCount: 2
  }
}
```

**Bridge logic:**
- Group outputs by `group` field from IPC payload
- Outputs with no group go into an "Ungrouped" default group
- Outputs with `hidden: true` go into the `hidden` array (dock can show/hide via toggle)
- Per-output `status` derived from `drop_pct` + `active`:
  - `active && drop_pct < 0.01` → `"healthy"`
  - `active && drop_pct < 0.05` → `"degraded"`
  - `active && drop_pct >= 0.05` → `"critical"`
  - `!active` → `"offline"`

### 13.5 Dock UI Layout

Target dock rendering for multi-encode/multi-upload:

```
Encoders & Uploads
  ┌─ Horizontal (1920×1080) ────────────────┐
  │  Pool: 18.5 Mbps   Lag: 2.1ms          │
  │                                          │
  │  ● Twitch       6.2 Mbps  60fps  0.01% │
  │  ● Kick         6.1 Mbps  60fps  0.02% │
  │  ● YT Horiz     6.2 Mbps  60fps  0.01% │
  ├─ Vertical (1080×1920) ──────────────────┤
  │  Pool: 8.4 Mbps    Lag: 3.0ms          │
  │                                          │
  │  ● TikTok       4.2 Mbps  30fps  0.03% │
  │  ● YT Shorts    4.2 Mbps  30fps  0.02% │
  └──────────────────────────────────────────┘
  Hidden (2): Recording, Virtual Camera [Show]
```

**Per-output row:** status dot (color-coded) + display name + bitrate + FPS + drop%
**Per-group header:** group name + resolution + pool bitrate + avg lag
**Hidden section:** collapsed by default, expandable, shows count + names
**Inactive outputs within active groups:** dimmed row, "--" for metrics

### 13.6 Settings UI for Output Config

The dock settings or Rust core settings page should allow:
1. **Rename outputs** — map OBS output ID to display name
2. **Assign encoder group** — dropdown or text field per output
3. **Toggle visibility** — show/hide per output
4. **Auto-detect** — button to scan OBS outputs and populate list with current IDs

This replaces the v0.0.1 rename modal with a richer config experience.

### 13.7 Implementation Priority

1. **IPC expansion** (Codex) — Add `outputs: Vec<SnapshotOutput>` to `StatusSnapshotPayload`, populate from `frame.streams` + config metadata. Backward-compatible (field is optional).
2. **Config schema** (Codex) — Extend `config.toml` with `[[outputs]]` table array. Migration: convert existing `output_names` HashMap to new format.
3. **Bridge projection** (Codex) — Add grouping/normalization logic to `aegis-dock-bridge.js` for `getState().outputs`.
4. **Dock UI** (Claude Code) — New `EncodersUploads` section in `aegis-dock.jsx` rendering grouped outputs with hide/show toggle.
5. **Settings UI** (Claude Code) — Output rename/group/visibility config in dock settings.
6. **Encoder detection** (Codex, stretch) — Attempt `GetOutputSettings` via OBS WebSocket to auto-detect encoder name and resolution per output.

---

## 14. Acceptance Criteria

API v1 is ready when:
1. Endpoints 5.1-5.4 are implemented with integration tests.
2. Idempotency and transition rules are verified by tests.
3. Error contract and rate limits are enforced consistently.
4. Relay health reconciliation data is persisted and queryable.
5. Per-link relay telemetry (section 12) flows from relay to dock.
6. Per-output multi-encode telemetry (section 13) flows from Rust core to dock via IPC.
