# Telemy - OBS Stream Monitor

Telemy is a real-time monitoring solution for multi-encode OBS streaming setups. It bridges OBS Studio with Grafana Cloud for remote telemetry while providing a live health dashboard directly inside OBS.

## Repository Layout

- `obs-telemetry-bridge/`: Rust telemetry engine and dashboard server
- `README.md`: consolidated project, usage, development, and fix notes

## What It Does

- Monitors OBS outputs (streams, recordings) with per-output metrics
- Displays a real-time dashboard as an OBS Browser Source
- Exports telemetry to Grafana Cloud via OpenTelemetry OTLP/HTTP
- Includes a bundled Grafana dashboard with import support
- Stores secrets using Windows DPAPI encryption
- Supports Windows system tray and autostart

## Architecture

OBS Studio -> WebSocket v5 -> Telemy Engine (metrics collection) ->
- Axum server (dashboard + settings + WS)
- OTLP exporter (Grafana Cloud)
- Tray integration (Windows)

## Components

1. MetricsHub: collects OBS, system, GPU, and network metrics on an interval.
2. HTTP/WebSocket server: serves dashboard, settings, and telemetry stream.
3. Grafana exporter: pushes histogram metrics via OTLP/HTTP.
4. System tray: Open Dashboard and Quit actions.

## Monitored Metrics

- OBS per-output metrics: bitrate, FPS, drop percent, encoding lag
- OBS global metrics: streaming/recording status, studio mode, active FPS
- OBS quality metrics: render missed frames, encoder skipped frames, disk space
- System metrics: CPU, memory, GPU utilization/temperature (NVIDIA NVML)
- Network metrics: upload/download throughput and latency probe

## Quick Start

From the repo root:

```bash
cd obs-telemetry-bridge
cargo build --release
./target/release/obs-telemetry-bridge config-init
./target/release/obs-telemetry-bridge vault-set obs_password "YOUR_OBS_PASSWORD"
./target/release/obs-telemetry-bridge
```

Add the printed `http://127.0.0.1:7070/obs?token=...` URL to OBS as a Browser Source.

## CLI Commands

- `obs-telemetry-bridge config-init`
- `obs-telemetry-bridge vault-set <key> <value>`
- `obs-telemetry-bridge vault-get <key>`
- `obs-telemetry-bridge vault-list`
- `obs-telemetry-bridge autostart-enable`
- `obs-telemetry-bridge autostart-disable`

## Settings and Security

- Settings page: `http://127.0.0.1:7070/settings?token=...`
- Vault location: `%APPDATA%/Telemy/vault.json`
- Vault keys:
  - `server_token` (dashboard access)
  - `obs_password` (OBS WebSocket password)
  - `grafana_auth` (Grafana auth material)

All secrets are encrypted with Windows DPAPI.

## Grafana Cloud Integration

- Configure endpoint, instance ID, and token via Settings page.
- Download bundled dashboard from `GET /grafana-dashboard?token=...`
- Auto-import dashboard via `POST /grafana-dashboard/import?token=...`

### Exported Metrics

- `telemy.health`
- `telemy.system.cpu_percent`
- `telemy.system.mem_percent`
- `telemy.system.gpu_percent`
- `telemy.system.gpu_temp_c`
- `telemy.network.upload_mbps`
- `telemy.network.download_mbps`
- `telemy.network.latency_ms`
- `telemy.output.bitrate_kbps`
- `telemy.output.drop_pct`
- `telemy.output.fps`
- `telemy.output.encoding_lag_ms`
- `telemy.obs.render_missed_frames`
- `telemy.obs.render_total_frames`
- `telemy.obs.output_skipped_frames`
- `telemy.obs.output_total_frames`
- `telemy.obs.active_fps`
- `telemy.obs.disk_space_mb`

## Development Guide

### Module Structure

```text
obs-telemetry-bridge/src/
|- main.rs
|- app/mod.rs
|- config/mod.rs
|- security/mod.rs
|- model/mod.rs
|- metrics/mod.rs
|- server/mod.rs
|- exporters/mod.rs
`- tray/mod.rs
```

### Key Dependencies

- Runtime/server: `tokio`, `axum`, `tower`
- OBS integration: `obws`
- Metrics/system: `opentelemetry`, `opentelemetry-otlp`, `sysinfo`, `nvml-wrapper`
- Config/serialization: `serde`, `serde_json`, `toml`
- Windows integration: `windows`, `tray-item`, `winres`

### Build and Validation

```bash
cd obs-telemetry-bridge
cargo test
cargo clippy -- -D warnings
cargo build --release
```

## OBS 207 Compatibility Fix

Date: 2026-02-19

Issue observed:
- `DeserializeMessage(Error("invalid value: 207 ..."))`

Cause:
- Older `obws` did not recognize OBS status code `207` (`NotReady`).

Resolution:
- Upgraded `obws` from `0.11.5` to `0.14.0`.

Validation completed:
- `cargo test` passed
- `cargo clippy -- -D warnings` passed
- `cargo build --release` passed

## Requirements

- Windows 10/11 (64-bit)
- OBS Studio 28+ with WebSocket v5 enabled
- Grafana Cloud account (optional)
- NVIDIA GPU (optional, for GPU metrics)

## License

TBD
