# Aegis Database Schema v1 (PostgreSQL)

## 1. Scope

This schema supports `telemy-v0.0.3` cloud control plane.

Covers:
- Identity and API access
- Relay session lifecycle
- Idempotent provisioning
- Usage metering (Time Bank)
- Outage reconciliation and billing auditability

Database:
- PostgreSQL 15+

---

## 2. Conventions

- Primary keys: text IDs with prefixes (`usr_`, `ses_`, `rly_`, `key_`, `use_`).
- Timestamps: `timestamptz` in UTC.
- Monetary amounts: integer cents.
- Durations: integer seconds.
- Status fields: constrained text (check constraints or enums).

---

## 3. Tables

## 3.1 `users`

Purpose:
- Account-level identity and plan state.

Columns:
- `id` text primary key
- `email` text not null unique
- `display_name` text null
- `plan_tier` text not null default `starter`
- `plan_status` text not null default `active`
- `cycle_start_at` timestamptz not null
- `cycle_end_at` timestamptz not null
- `included_seconds` integer not null default 0
- `created_at` timestamptz not null default now()
- `updated_at` timestamptz not null default now()

Checks:
- `plan_tier in ('starter','standard','pro')`
- `plan_status in ('active','past_due','canceled','trial')`
- `included_seconds >= 0`

Indexes:
- unique on `email`
- btree on `(plan_status, cycle_end_at)`

## 3.2 `api_keys`

Purpose:
- API key metadata for bootstrap auth and key management.

Columns:
- `id` text primary key
- `user_id` text not null references `users(id)` on delete cascade
- `key_hash` text not null unique
- `label` text null
- `last_used_at` timestamptz null
- `revoked_at` timestamptz null
- `created_at` timestamptz not null default now()

Indexes:
- btree on `(user_id, revoked_at)`

## 3.3 `relay_instances`

Purpose:
- Track actual cloud relay infrastructure lifecycle.

Columns:
- `id` text primary key
- `session_id` text null unique
- `aws_instance_id` text not null unique
- `region` text not null
- `ami_id` text not null
- `instance_type` text not null
- `public_ip` inet null
- `state` text not null
- `launched_at` timestamptz not null
- `terminated_at` timestamptz null
- `last_health_at` timestamptz null
- `created_at` timestamptz not null default now()

Checks:
- `state in ('provisioning','running','terminating','terminated','error')`

Indexes:
- btree on `(region, state)`
- btree on `(last_health_at)`

## 3.4 `sessions`

Purpose:
- Authoritative relay session lifecycle and usage anchors.

Columns:
- `id` text primary key
- `user_id` text not null references `users(id)` on delete cascade
- `relay_instance_id` text null references `relay_instances(id)`
- `status` text not null
- `region` text not null
- `idempotency_key` uuid null
- `requested_by` text not null default `dashboard`
- `started_at` timestamptz not null
- `grace_started_at` timestamptz null
- `stopped_at` timestamptz null
- `max_session_seconds` integer not null default 57600
- `grace_window_seconds` integer not null default 600
- `duration_seconds` integer not null default 0
- `reconciled_seconds` integer not null default 0
- `created_at` timestamptz not null default now()
- `updated_at` timestamptz not null default now()

Checks:
- `status in ('provisioning','active','grace','stopped')`
- `max_session_seconds > 0`
- `grace_window_seconds > 0`
- `duration_seconds >= 0`
- `reconciled_seconds >= 0`

Indexes:
- partial unique on active-like states:
  - unique `(user_id)` where `status in ('provisioning','active','grace')`
- btree on `(user_id, started_at desc)`
- btree on `(status, updated_at)`
- btree on `(idempotency_key)` where `idempotency_key is not null`

## 3.5 `idempotency_records`

Purpose:
- Store dedupe semantics for `POST /relay/start`.

Columns:
- `id` bigserial primary key
- `user_id` text not null references `users(id)` on delete cascade
- `endpoint` text not null
- `idempotency_key` uuid not null
- `request_hash` text not null
- `response_json` jsonb not null
- `session_id` text null references `sessions(id)`
- `created_at` timestamptz not null default now()
- `expires_at` timestamptz not null

Constraints:
- unique `(user_id, endpoint, idempotency_key)`

Indexes:
- btree on `(expires_at)`

Retention:
- TTL cleanup job removes expired records (default 1 hour after create).

## 3.6 `usage_records`

Purpose:
- Meter usage per billing cycle and preserve audit trail.

Columns:
- `id` text primary key
- `user_id` text not null references `users(id)` on delete cascade
- `session_id` text not null references `sessions(id)` on delete cascade
- `cycle_start_at` timestamptz not null
- `cycle_end_at` timestamptz not null
- `measured_seconds` integer not null default 0
- `reconciled_seconds` integer not null default 0
- `billable_seconds` integer not null default 0
- `overage_seconds` integer not null default 0
- `created_at` timestamptz not null default now()
- `updated_at` timestamptz not null default now()

Checks:
- all duration columns >= 0

Indexes:
- btree on `(user_id, cycle_start_at, cycle_end_at)`
- btree on `(session_id)`

## 3.7 `relay_health_events`

Purpose:
- Persist relay heartbeat payloads for watchdog logic and reconciliation.

Columns:
- `id` bigserial primary key
- `session_id` text not null references `sessions(id)` on delete cascade
- `relay_instance_id` text not null references `relay_instances(id)` on delete cascade
- `observed_at` timestamptz not null
- `ingest_active` boolean not null
- `egress_active` boolean not null
- `session_uptime_seconds` integer not null
- `payload_json` jsonb not null
- `created_at` timestamptz not null default now()

Checks:
- `session_uptime_seconds >= 0`

Indexes:
- btree on `(session_id, observed_at desc)`
- btree on `(relay_instance_id, observed_at desc)`

## 3.8 `billing_adjustments`

Purpose:
- Immutable audit log for outage true-up corrections.

Columns:
- `id` text primary key
- `user_id` text not null references `users(id)` on delete cascade
- `session_id` text not null references `sessions(id)` on delete cascade
- `adjustment_seconds` integer not null
- `reason` text not null
- `source` text not null
- `created_at` timestamptz not null default now()

Checks:
- `reason in ('outage_reconciliation','manual_correction','dispute_resolution')`
- `source in ('relay_health','admin_tool','automated_job')`

Indexes:
- btree on `(user_id, created_at desc)`
- btree on `(session_id, created_at desc)`

---

## 4. Lifecycle and Integrity Rules

1. One active session per user:
- Enforced by partial unique index in `sessions`.

2. State transitions:
- Service layer enforces legal transitions:
  - `provisioning -> active`
  - `active -> grace`
  - `grace -> active`
  - `active|grace -> stopped`

3. Idempotency:
- `idempotency_records` stores request hash and canonical response.
- Same key + different hash => conflict response.

4. Outage reconciliation:
- Recovery job compares:
  - backend-calculated session duration
  - max `session_uptime_seconds` from relay health events
- Positive gap emits:
  - `billing_adjustments` row
  - update to `sessions.reconciled_seconds`
  - update to `usage_records.reconciled_seconds` and `billable_seconds`

---

## 5. Suggested Enum DDL (Optional)

If using PostgreSQL enums:
- `session_status_enum`
- `relay_state_enum`
- `plan_tier_enum`
- `plan_status_enum`

Alternative:
- keep text + check constraints for easier migrations.

---

## 6. Migration Order (v1)

1. Create `users`
2. Create `api_keys`
3. Create `relay_instances`
4. Create `sessions`
5. Create `idempotency_records`
6. Create `usage_records`
7. Create `relay_health_events`
8. Create `billing_adjustments`
9. Create indexes and constraints
10. Seed default plan metadata (app-level or DB table in v2)

---

## 7. Operational Jobs

1. `idempotency_ttl_cleanup`:
- Runs every 5 minutes.
- Deletes expired `idempotency_records`.

2. `session_usage_rollup`:
- Runs every minute.
- Updates live `duration_seconds` for active/grace sessions.

3. `outage_reconciliation`:
- Runs every 2 minutes.
- Applies `session_uptime_seconds` true-ups after backend recovery.

4. `health_event_retention`:
- Runs daily.
- Compacts or archives old `relay_health_events` outside retention window.

---

## 8. Query Patterns

1. Fetch active session for user:
```sql
select *
from sessions
where user_id = $1
  and status in ('provisioning', 'active', 'grace')
order by created_at desc
limit 1;
```

2. Validate idempotency replay:
```sql
select *
from idempotency_records
where user_id = $1
  and endpoint = '/api/v1/relay/start'
  and idempotency_key = $2
  and expires_at > now();
```

3. Compute cycle usage:
```sql
select
  coalesce(sum(billable_seconds), 0) as consumed_seconds
from usage_records
where user_id = $1
  and cycle_start_at = $2
  and cycle_end_at = $3;
```

---

## 9. Acceptance Criteria

Schema v1 is accepted when:
1. Unique active-session constraint prevents duplicates under load tests.
2. Idempotency replay path is deterministic and conflict-safe.
3. Outage reconciliation produces auditable adjustment rows.
4. Usage rollups support Time Bank enforcement at API read time.
5. All key queries meet latency targets with expected scale indexes.

