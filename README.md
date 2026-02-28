# Telemy v0.0.3 Workspace

This repository contains the local OBS telemetry app plus the new Aegis cloud control-plane work for `v0.0.3`.

## Repository Layout

- `obs-telemetry-bridge/`: Rust app (OBS metrics, dashboard, exporter, tray, local runtime).
- `aegis-control-plane/`: Go backend (relay lifecycle APIs, DB store, AWS/fake provisioners, jobs).
- `docs/`: Versioned contracts/specs for API, DB schema, state machine, and IPC.
- `AGENTS.md`: contributor workflow and coding conventions.

## Start Here

- Documentation index: `docs/README.md`
- Current implementation status (quick redirect): `HANDOFF_STATUS.md`
- Concise current status: `docs/CURRENT_STATUS.md`
- Handoff index + latest addenda pointers: `docs/archive/HANDOFF_STATUS.md`
- Full historical handoff log: `docs/archive/HANDOFF_HISTORY.md`
- Archived planning/history (reference only): `docs/archive/OVERHAUL-v0.0.3.md`, `docs/archive/ARCHITECTURE-v0.0.3-FIRST-PASS.md`

## Quick Commands

Rust app:
```powershell
cd obs-telemetry-bridge
cargo build
cargo test
```

Go backend:
```powershell
cd aegis-control-plane
go test ./...
go run ./cmd/api
```

## Runtime Notes

- OBS integration is local and Windows-focused.
- Aegis backend requires PostgreSQL and environment configuration (see `aegis-control-plane/README.md`).
- Background jobs (idempotency cleanup, usage rollup, outage reconciliation) run in-process with the backend service.

## Security

- Never commit credentials, tokens, or populated vault/config secrets.
- Use env vars for backend secrets (`AEGIS_DATABASE_URL`, `AEGIS_JWT_SECRET`, `AEGIS_RELAY_SHARED_KEY`).

## Current Handoff Note

- Backend/cloud side is ahead of client/plugin maturity in this workspace snapshot.
- Latest backend execution and ops handoff details (including EC2 timeout-fix deployment and access hardening) are in `docs/archive/HANDOFF_HISTORY.md`.
- Next major project focus remains the v0.0.3 client/plugin/app overhaul before broad beta testing.
- Keep transient operator prompts out of the repo root; capture durable context in `docs/archive/HANDOFF_HISTORY.md` (and refresh `docs/CURRENT_STATUS.md` as needed).

