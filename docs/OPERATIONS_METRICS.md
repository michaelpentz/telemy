# Operations Metrics (Aegis Control Plane)

This document defines how to scrape and alert on `aegis-control-plane` runtime metrics.

## Endpoint

- Path: `GET /metrics`
- Format: Prometheus text exposition
- Scope: API process metrics and, when running `cmd/jobs`, worker process metrics

Current implementation note (2026-02-22 audit):
- `cmd/api` exposes `GET /metrics`.
- `cmd/jobs` currently does not run an HTTP listener, so there is no separate jobs scrape endpoint yet.
- Job metrics are only visible from the process that hosts the metrics handler.

## Important Metrics

Relay lifecycle:
- `aegis_relay_provision_total{provider,region,status}`
- `aegis_relay_provision_latency_ms_bucket|sum|count{provider,region,status}`
- `aegis_relay_deprovision_total{provider,region,status}`
- `aegis_relay_deprovision_latency_ms_bucket|sum|count{provider,region,status}`

Background jobs:
- `aegis_job_runs_total{job,status}`
- `aegis_job_duration_ms_bucket|sum|count{job}`

AWS reliability:
- `aegis_aws_operations_total{op,region,status}`
- `aegis_aws_operation_latency_ms_bucket|sum|count{op,region,status}`
- `aegis_aws_retries_total{op,region,reason}`
- `aegis_aws_retry_exhausted_total{op,region}`

## Prometheus Scrape Example

```yaml
scrape_configs:
  - job_name: aegis-api
    static_configs:
      - targets: ["127.0.0.1:8080"]
```

Future / target setup (not implemented in current `cmd/jobs`):

```yaml
scrape_configs:
  - job_name: aegis-jobs
    static_configs:
      - targets: ["127.0.0.1:8081"]
```

If API and jobs run in separate processes, add a jobs HTTP metrics listener and scrape both.

## Starter Alert Rules

1. Job failure rate:
- Alert if `rate(aegis_job_runs_total{status="error"}[5m]) > 0` for 10m.

2. Provisioning latency (p95):
- Alert if p95 of `aegis_relay_provision_latency_ms` exceeds `15000ms` for 15m.

3. AWS retry exhaustion:
- Alert if `increase(aegis_aws_retry_exhausted_total[10m]) > 0`.

4. Retry burst by region:
- Alert if `sum by (region) (increase(aegis_aws_retries_total[5m]))` crosses your regional threshold.

## Operational Notes

- `status="error"` reflects failed operation paths.
- `status="ignored"` (terminate op) means AWS reported already-terminal state and no action was required.
- Keep label cardinality low; do not add user/session IDs to metric labels.
