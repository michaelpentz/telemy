# Repository Guidelines

## Project Structure & Module Organization
This repository is split into three main areas:
- `aegis-control-plane/`: Go backend (API, relay provisioning, DB store, migrations).
- `obs-telemetry-bridge/`: Rust bridge/service code for OBS telemetry integration.
- `docs/`: Versioned design and protocol docs (`API_SPEC_v1.md`, `DB_SCHEMA_v1.md`, etc.).

Go backend layout uses standard package boundaries:
- `cmd/api/` server entrypoint
- `internal/api`, `internal/store`, `internal/relay`, `internal/auth`, `internal/jobs`
- `migrations/` SQL schema changes (append-only, sequential files)

## Build, Test, and Development Commands
- Backend run:
```powershell
cd aegis-control-plane
go run ./cmd/api
```
- Backend tests:
```powershell
cd aegis-control-plane
go test ./...
```
- Format Go code:
```powershell
cd aegis-control-plane
gofmt -w ./...
```
- Rust bridge checks (from `obs-telemetry-bridge/`):
```powershell
cargo build
cargo test
```

## Coding Style & Naming Conventions
- Go: `gofmt` formatting, tabs/default Go style, short receiver names, exported identifiers in `PascalCase`.
- Rust: `rustfmt` defaults, snake_case for functions/modules, `CamelCase` for types.
- SQL migrations: `000X_description.sql` naming; never rewrite old migrations.
- Keep package/module responsibilities narrow (API handlers should call store/provisioner, not embed SQL).

## Testing Guidelines
- Go tests use the standard `testing` package plus `pgxmock` where DB behavior is validated.
- Name tests as `Test<Feature>_<Scenario>` (examples: `TestRelayStop_...`, `TestRetryAWS_...`).
- Add tests for new error paths, idempotency behavior, and state transitions.
- Run `go test ./...` before opening a PR.

## Commit & Pull Request Guidelines
- Prefer conventional, imperative commit subjects seen in history: `fix: ...`, `chore: ...`.
- Keep commits focused (one concern per commit where possible).
- PRs should include:
- concise summary of behavior changes
- touched paths (e.g., `internal/store/store.go`, `migrations/0003_...sql`)
- test evidence (`go test ./...`, `cargo test` where applicable)
- linked issue/task when available

## Security & Configuration Tips
- Do not commit secrets. Use env vars (`AEGIS_DATABASE_URL`, `AEGIS_JWT_SECRET`, AWS credentials).
- Validate new config keys in `internal/config/config.go`.
- Treat relay credentials/tokens as sensitive in logs and responses.
