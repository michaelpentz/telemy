# Development Guide — Telemy

Technical architecture and implementation details for developers.

## Module Structure

```
obs-telemetry-bridge/src/
├── main.rs              # Entry point, CLI dispatch (config-init, vault-*, autostart-*)
├── app/mod.rs           # Orchestrates all tokio tasks, creates watch channel
├── config/mod.rs        # TOML config loading, AppConfig struct, config.example.toml template
├── security/mod.rs      # Windows DPAPI vault (encrypt/decrypt via CryptProtectData)
├── model/mod.rs         # Data structures: TelemetryFrame, ObsFrame, SystemFrame, etc.
├── metrics/mod.rs       # MetricsHub: OBS + system + GPU + network collection
├── server/mod.rs        # Axum HTTP server: dashboard, WebSocket, settings, Grafana routes
├── exporters/mod.rs     # GrafanaExporter: OpenTelemetry OTLP histogram export
└── tray/mod.rs          # Windows system tray icon
```

## Key Dependencies

| Crate | Version | Purpose |
|-------|---------|---------|
| tokio | 1.35 | Async runtime (rt-multi-thread, macros, time, signal) |
| axum | 0.7 | HTTP server with WebSocket support |
| obws | 0.11 | OBS WebSocket v5 client |
| sysinfo | 0.30 | CPU and memory metrics |
| nvml-wrapper | 0.9 | NVIDIA GPU utilization and temperature |
| opentelemetry | 0.21 | Metrics SDK |
| opentelemetry_sdk | 0.21 | SDK with rt-tokio feature |
| opentelemetry-otlp | 0.14 | OTLP/HTTP exporter |
| serde / serde_json | 1.0 | Serialization |
| toml | 0.8 | Config file parsing |
| windows | 0.52 | Win32 DPAPI, registry |
| tray-item | 0.9 | System tray icon |
| reqwest | 0.11 | HTTP client for Grafana API (rustls-tls) |
| tower | 0.4 | Rate limiting middleware |
| base64 | 0.21 | Encoding Grafana Basic auth |
| rand | 0.8 | Token generation |
| tracing / tracing-subscriber | 0.1 / 0.3 | Structured logging |
| winres | 0.1 | Build script: embed application icon |

## Data Flow

```
main.rs
  └── app::run()
        ├── config::load()           → AppConfig
        ├── security::Vault::open()  → decrypt secrets
        ├── MetricsHub::new()
        ├── watch::channel()         → (tx, rx)
        │
        ├── tokio::spawn(metrics_loop)
        │     └── loop { hub.collect() → tx.send(frame) }
        │
        ├── tokio::spawn(server)
        │     └── Axum router with watch::Receiver<TelemetryFrame>
        │           ├── GET /obs         → dashboard HTML (embedded)
        │           ├── GET /ws          → WebSocket (sends JSON frames)
        │           ├── GET /settings    → settings page HTML
        │           ├── POST /settings   → save config + vault
        │           ├── GET /grafana-dashboard      → download JSON
        │           ├── POST /grafana-dashboard/import → Grafana API push
        │           └── GET /setup       → redirect to /settings
        │
        ├── tokio::spawn(exporter)
        │     └── loop { rx.changed() → exporter.record(frame) }
        │
        └── tray::run() (blocking on main thread)
```

## MetricsHub Collection

Each `collect()` call produces a `TelemetryFrame`:

1. **OBS Process Detection** — If `auto_detect_process` is enabled, checks if `obs64.exe` is running (throttled to every 2s). Skips OBS connection attempts when OBS isn't running.

2. **OBS WebSocket** — Connects via obws when OBS is detected. Collects:
   - `client.outputs().list()` — enumerate all outputs
   - `client.outputs().status(name)` — per-output bytes, frames, duration
   - `client.streaming().status()` — streaming active, total/skipped frames
   - `client.recording().status()` — recording active
   - `client.general().stats()` — render missed/total frames, output skipped/total frames, active FPS, available disk space, average frame render time (used as encoding lag)
   - `client.ui().studio_mode_enabled()` — studio mode detection

3. **System Metrics** — via sysinfo: global CPU usage, memory usage percentage

4. **GPU Metrics** — via NVML: GPU utilization %, temperature (gracefully returns None if no NVIDIA GPU)

5. **Network** — Aggregates all interface bytes via sysinfo Networks, computes delta-based upload/download Mbps. TCP connect latency probe to configured target.

6. **Health Score** — `1.0 - avg_drop_pct` across all outputs, clamped to [0, 1].

## Grafana Exporter

Uses OpenTelemetry SDK with OTLP/HTTP exporter. All metrics are `f64_histogram` instruments under the `telemy` meter. The `PeriodicReader` flushes at a configurable interval (default 5000ms).

Per-output metrics use `KeyValue::new("output", name)` labels for Grafana grouping.

Authentication: Basic auth header encoded as `base64(instance_id:api_token)`, stored encrypted in vault under `grafana_auth` key.

## Server

All HTML is embedded in `server/mod.rs` as string literals (no template engine). The dashboard uses vanilla JS with a WebSocket connection for real-time updates.

### Dashboard Features
- Status badges: LIVE (green), REC (red), Studio Mode
- Per-output bars with bitrate, FPS, drop %, lag color-coded by thresholds
- Stats row: disk space, render missed frames, encoder skipped frames, active FPS
- System/network badges: CPU, GPU (with temp), memory, upload, download, latency
- Hide-inactive checkbox toggle

### Settings Page
- OBS Connection: host, port, password (vault-stored)
- Grafana Cloud: endpoint, instance ID, API token (vault-stored), push interval
- Grafana Dashboard: download JSON button, collapsible auto-import form

### Grafana Dashboard Routes
- `GET /grafana-dashboard` — serves `assets/grafana-dashboard.json` (embedded via `include_str!`) as a file download
- `POST /grafana-dashboard/import` — accepts `grafana_url` and `grafana_sa_token` form fields, POSTs the dashboard JSON to `{url}/api/dashboards/db` using the Grafana HTTP API

## Security Vault

`%APPDATA%/Telemy/vault.json` stores key-value pairs as DPAPI-encrypted base64 blobs. Each entry is independently encrypted. Keys used:

| Key | Purpose |
|-----|---------|
| `server_token` | Dashboard access token |
| `obs_password` | OBS WebSocket password |
| `grafana_auth` | Basic auth header value for OTLP push |

The settings page can write to the vault (OBS password, Grafana credentials) without requiring CLI access.

## Configuration

TOML file at `config.toml`. Generated via `config-init` command from embedded template.

```toml
[obs]
host = "localhost"
port = 4455
password_key = "obs_password"      # vault key reference
auto_detect_process = true
process_name = "obs64.exe"

[server]
port = 7070
token_key = "server_token"         # vault key reference

[vault]
path = "vault.json"                # relative to %APPDATA%/Telemy/

[grafana]
enabled = false
endpoint = ""
auth_key = "grafana_auth"          # vault key reference
push_interval_ms = 5000

[theme]
font_family = "Arial, sans-serif"
bg = "#0b0e12"
panel = "#111723"
muted = "#8da3c1"
good = "#33d17a"
warn = "#f6d32d"
bad = "#e01b24"
line = "#1f2a3a"
```

## Build

`build.rs` uses winres to embed `assets/telemy.ico` as the Windows application icon.

`assets/grafana-dashboard.json` is embedded into the binary at compile time via `include_str!("../../assets/grafana-dashboard.json")` in `server/mod.rs`.

## Testing

```bash
cargo test             # Unit tests (model serialization, health calculation, config parsing)
cargo clippy           # Lint
cargo build --release  # Verify release build
```

## Platform Notes

- **Windows-only**: DPAPI, registry autostart, system tray, and `obs64.exe` process detection are all Windows-specific
- **NVIDIA optional**: GPU metrics degrade gracefully (return `None`) without NVML
- **OBS optional at runtime**: App runs and serves dashboard even without OBS connected; OBS metrics show as disconnected/zero
