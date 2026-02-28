# Aegis Control Plane (v0.0.3)

Go backend scaffold for Aegis relay lifecycle and usage APIs.

Canonical specs: `../docs/README.md`

## Run

Set environment variables:

```powershell
$env:AEGIS_LISTEN_ADDR=":8080"
$env:AEGIS_DATABASE_URL="postgres://postgres:postgres@localhost:5432/aegis?sslmode=disable"
$env:AEGIS_JWT_SECRET="dev-secret-change-me"
$env:AEGIS_RELAY_SHARED_KEY="relay-dev-key"
$env:AEGIS_DEFAULT_REGION="us-east-1"
$env:AEGIS_SUPPORTED_REGIONS="us-east-1,eu-west-1"
$env:AEGIS_RELAY_PROVIDER="fake"
```

Run server:

```powershell
go run ./cmd/api
```

Run jobs worker (separate process):

```powershell
go run ./cmd/jobs
```

## Endpoints Implemented

- `GET /healthz`
- `GET /metrics` (Prometheus exposition format)
- `POST /api/v1/relay/start`
- `GET /api/v1/relay/active`
- `POST /api/v1/relay/stop`
- `GET /api/v1/relay/manifest`
- `GET /api/v1/usage/current`
- `POST /api/v1/relay/health` (relay shared-key auth)

## Provisioning and Teardown

- `POST /api/v1/relay/start`
  - idempotent session create/get
  - provider provision call
  - transition `provisioning -> active`
  - persists relay instance metadata and session tokens
- `POST /api/v1/relay/stop`
  - provider deprovision call (when needed)
  - idempotent terminal transition to `stopped`
  - marks relay instance terminated in DB

## Notes

- Client endpoints require `Authorization: Bearer <cp_access_jwt>`.
- `POST /api/v1/relay/start` requires `Idempotency-Key` header.
- SQL migrations live in `migrations/` (currently `0001_init.sql`, `0002_relay_manifest.sql`).
- Relay provider modes:
  - `fake` (default, local dev)
  - `aws` (EC2 provisioning)
- Startup seeds `relay_manifests` from supported regions:
  - `fake` mode uses placeholder AMI IDs (`ami-fake-<region>`) if `AEGIS_AWS_AMI_MAP` is not set
  - `aws` mode requires real `AEGIS_AWS_AMI_MAP` entries
- `POST /api/v1/relay/stop` triggers provider deprovision and then marks relay/session terminated.
- Background jobs run in-process:
- Background jobs should run via `cmd/jobs`:
  - idempotency TTL cleanup (5m)
  - session usage rollup (1m)
  - outage reconciliation true-up (2m)
- AWS mode env:
  - `AEGIS_RELAY_PROVIDER=aws`
  - `AEGIS_AWS_AMI_MAP=us-east-1=ami-xxxx,eu-west-1=ami-yyyy`
  - optional: `AEGIS_AWS_INSTANCE_TYPE`, `AEGIS_AWS_SUBNET_ID`, `AEGIS_AWS_SECURITY_GROUP_IDS`, `AEGIS_AWS_KEY_NAME`
  - AWS credentials are read by the default AWS SDK chain (env vars, shared config, IAM role).

## Tests

Run:

```powershell
go test ./...
```

Current coverage focus:
- API stop handler idempotency and deprovision error behavior
- Relay AWS terminate error classification
- Store transaction behavior for `active/grace -> stopped` and already-stopped idempotency
