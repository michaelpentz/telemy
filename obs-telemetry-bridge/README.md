# OBS Telemetry Bridge (v0.0.2)

Minimal Rust telemetry engine that:
- Collects OBS + system + network metrics in real-time
- Streams them to an in-OBS dashboard (local HTTP + WebSocket)
- Pushes metrics to Grafana Cloud (OTLP/HTTP)
- Provides a unified settings page for OBS and Grafana configuration
- Bundles a pre-built Grafana dashboard with one-click import

## Quick Start

1. Initialize a default config:

```
obs-telemetry-bridge config-init
```

2. Store secrets in the vault:

```
obs-telemetry-bridge vault-set obs_password "<OBS_WEBSOCKET_PASSWORD>"
obs-telemetry-bridge vault-set grafana_auth "Bearer <GRAFANA_TOKEN>"
```

3. Run the app:

```
obs-telemetry-bridge
```

It prints a local OBS dashboard URL like:

```
http://127.0.0.1:7070/obs?token=...
```

Add that URL to OBS as a **Browser Source**.

## Commands

- `obs-telemetry-bridge config-init`
- `obs-telemetry-bridge vault-set <key> <value>`
- `obs-telemetry-bridge vault-get <key>`
- `obs-telemetry-bridge vault-list`
- `obs-telemetry-bridge autostart-enable`
- `obs-telemetry-bridge autostart-disable`

## Logging

Set `RUST_LOG=info` (or `debug`, `trace`) to control log output.

## Dashboard

The OBS browser source dashboard displays:
- Per-output stream bars: bitrate, FPS, drop %, encoding lag
- OBS status badges: LIVE, REC, Studio Mode
- System stats: CPU%, GPU%, GPU temp, memory
- Network stats: upload/download Mbps, latency
- OBS stats row: disk space, render missed frames, encoder skipped frames, active FPS
- Hide-inactive toggle to filter idle outputs

## Settings Page

Access the settings page at `http://127.0.0.1:7070/settings?token=...`

Configurable from the web UI:
- **OBS Connection**: host, port, WebSocket password (stored encrypted in vault)
- **Grafana Cloud**: OTLP endpoint, instance ID, API token (stored encrypted), push interval
- **Grafana Dashboard**: download the bundled JSON or auto-import directly to Grafana Cloud

## Grafana Cloud

### Setup via Settings Page

The settings page includes a Grafana Cloud section where you can enter your OTLP endpoint, instance ID, and API token. Credentials are encoded as Basic auth and stored encrypted in the vault.

### Dashboard

A pre-built Grafana dashboard (`assets/grafana-dashboard.json`) with 13 panels is bundled into the binary:
- Stream Health gauge
- OBS Active FPS, Disk Space stats
- CPU / Memory / GPU timeseries
- GPU Temperature
- Network Throughput and Latency
- Per-output Bitrate, FPS, Drop Rate, Encoding Lag
- Render Missed Frames, Encoder Skipped Frames

**Download**: `GET /grafana-dashboard?token=...` serves the JSON file.

**Auto-import**: `POST /grafana-dashboard/import?token=...` pushes the dashboard to your Grafana Cloud instance using the Grafana HTTP API. Requires a Grafana service account token with dashboard create permissions (separate from the OTLP metrics token).

### Exported Metrics (18 total)

| Metric | Type | Description |
|--------|------|-------------|
| `telemy.health` | Histogram | Overall stream health (0-1) |
| `telemy.system.cpu_percent` | Histogram | CPU usage % |
| `telemy.system.mem_percent` | Histogram | Memory usage % |
| `telemy.system.gpu_percent` | Histogram | GPU utilization % |
| `telemy.system.gpu_temp_c` | Histogram | GPU temperature (Celsius) |
| `telemy.network.upload_mbps` | Histogram | Upload throughput (Mbps) |
| `telemy.network.download_mbps` | Histogram | Download throughput (Mbps) |
| `telemy.network.latency_ms` | Histogram | Network latency (ms) |
| `telemy.output.bitrate_kbps` | Histogram | Per-output bitrate (kbps) |
| `telemy.output.drop_pct` | Histogram | Per-output frame drop rate |
| `telemy.output.fps` | Histogram | Per-output FPS |
| `telemy.output.encoding_lag_ms` | Histogram | Encoding lag (ms) |
| `telemy.obs.render_missed_frames` | Histogram | Render missed frame count |
| `telemy.obs.render_total_frames` | Histogram | Render total frame count |
| `telemy.obs.output_skipped_frames` | Histogram | Encoder skipped frame count |
| `telemy.obs.output_total_frames` | Histogram | Encoder total frame count |
| `telemy.obs.active_fps` | Histogram | OBS active FPS |
| `telemy.obs.disk_space_mb` | Histogram | Available disk space (MB) |

## System Tray

The tray icon provides:
- Open Dashboard
- Quit

The tray icon is loaded from `telemy.ico` in the install folder.

Disable via:

```
[tray]
enable = false
```

## Theme

Customize the OBS dashboard theme in `config.toml`:

```
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

## OBS Auto-Detect

When enabled, the app will only attempt OBS WebSocket connection while `obs64.exe` is running.

```
[obs]
auto_detect_process = true
process_name = "obs64.exe"
```

## Auto-start

Enable automatic launch on Windows:

```
obs-telemetry-bridge autostart-enable
```

Disable:

```
obs-telemetry-bridge autostart-disable
```

## Notes

- The dashboard is local-only and protected by a token.
- If Grafana is enabled in `config.toml`, OTLP metrics export is enabled.
- GPU metrics require an NVIDIA GPU with NVML available.
- Network throughput uses delta-based byte tracking for accurate instantaneous rates.
- Encoding lag is derived from OBS `average_frame_render_time` (global stat applied to all outputs).

## Packaging (Windows)

A draft Inno Setup script is included at `installer/setup.iss`.

Recommended approach:
- Build a release binary with `cargo build --release`
- Package the binary, `config.example.toml`, and `telemy.ico`
- Use the Inno Setup script to build an installer

## Build

```
cargo build --release
```
