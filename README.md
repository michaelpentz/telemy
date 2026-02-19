# Telemy — OBS Stream Monitor

A real-time monitoring solution for multi-encode OBS streaming setups. Telemy bridges OBS Studio with Grafana Cloud for remote telemetry while providing a live health dashboard directly inside OBS.

## What It Does

- Monitors all OBS outputs (streams, recordings) with per-output metrics
- Displays a real-time dashboard as an OBS Browser Source
- Exports 18 telemetry metrics to Grafana Cloud via OpenTelemetry OTLP
- Provides a bundled Grafana dashboard with one-click import
- Manages credentials securely using Windows DPAPI encryption
- Runs as a system tray application with auto-start support

## Architecture

Telemy is a standalone Rust executable that runs alongside OBS Studio:

```
                          OBS Studio
                              |
                         WebSocket v5
                              |
                     +--------+---------+
                     |   Telemy Engine  |
                     |                  |
                     |  MetricsHub      |--- sysinfo (CPU, RAM)
                     |  (collects       |--- NVML (GPU util, temp)
                     |   every 500ms)   |--- TCP probe (latency)
                     |                  |
                     +---+---------+----+
                         |         |
                  watch channel    |
                    |         |    |
                    v         v    v
                 Server    Exporter   Tray
                 (Axum)    (OTLP)     Icon
                    |         |
                    v         v
              OBS Browser  Grafana
              Source        Cloud
```

### Components

1. **MetricsHub** — Collects OBS WebSocket stats (outputs, streaming/recording status, general stats, studio mode), system metrics (CPU, memory, GPU, temperature), and network metrics (throughput, latency). Uses delta-based byte tracking for accurate network rates.

2. **HTTP/WebSocket Server** — Axum 0.7 on port 7070. Serves the embedded dashboard, WebSocket telemetry stream, unified settings page, and Grafana dashboard import routes. Token-authenticated.

3. **Grafana Exporter** — Pushes telemetry to Grafana Cloud via OpenTelemetry OTLP/HTTP. 18 histogram metrics covering health, system, network, per-output streams, and OBS internal stats.

4. **System Tray** — Windows tray icon with "Open Dashboard" and "Quit".

### Monitored Metrics

**OBS Stats** (via WebSocket v5):
- Per-output: bitrate, FPS, frame drop %, encoding lag
- Global: streaming/recording status, studio mode, active FPS
- Render missed frames, encoder skipped frames
- Available disk space

**System Metrics**:
- CPU usage, memory usage
- GPU utilization and temperature (NVIDIA NVML)
- Network upload/download throughput, latency

## Quick Start

See [obs-telemetry-bridge/README.md](obs-telemetry-bridge/README.md) for full setup instructions.

```bash
cd obs-telemetry-bridge
cargo build --release
./target/release/obs-telemetry-bridge config-init
./target/release/obs-telemetry-bridge vault-set obs_password "YOUR_OBS_PASSWORD"
./target/release/obs-telemetry-bridge
```

Add the printed URL as an OBS Browser Source.

## Grafana Cloud Integration

Telemy includes a pre-built Grafana dashboard with 13 panels. Setup options:

1. **Settings Page** — Configure OTLP endpoint, instance ID, and API token from the web UI at `/settings`
2. **Dashboard Download** — Download the bundled JSON from `/grafana-dashboard`
3. **Auto-Import** — Push the dashboard directly to Grafana Cloud from the settings page

## Security

- Windows DPAPI encryption for all secrets (OBS password, Grafana tokens, server token)
- Credentials stored as encrypted blobs in `%APPDATA%/Telemy/vault.json`
- Dashboard access requires a randomly-generated token
- Passwords never pre-filled in settings forms

## Requirements

- Windows 10/11 (64-bit)
- OBS Studio 28+ with WebSocket server enabled (obs-websocket v5)
- Grafana Cloud account (optional, for remote monitoring)
- NVIDIA GPU (optional, for GPU metrics)

## Technology Stack

- **Language**: Rust 2021 edition (tokio async runtime)
- **OBS Integration**: obws 0.11 (obs-websocket v5 protocol)
- **HTTP Server**: Axum 0.7 with WebSocket support
- **GPU Metrics**: nvml-wrapper 0.9 (NVIDIA NVML)
- **System Metrics**: sysinfo 0.30
- **Cloud Export**: OpenTelemetry OTLP/HTTP (opentelemetry 0.21)
- **Security**: Windows DPAPI via `windows` crate
- **Configuration**: TOML via `serde` + `toml`

## Development

```bash
cd obs-telemetry-bridge
cargo build            # Debug build
cargo test             # Run tests
cargo clippy           # Lint
cargo build --release  # Release build
```

See [DEVELOPMENT.md](DEVELOPMENT.md) for architecture details.

## License

TBD
