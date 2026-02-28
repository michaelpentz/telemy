create table if not exists users (
  id text primary key,
  email text not null unique,
  display_name text,
  plan_tier text not null default 'starter',
  plan_status text not null default 'active',
  cycle_start_at timestamptz not null,
  cycle_end_at timestamptz not null,
  included_seconds integer not null default 0,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  check (plan_tier in ('starter', 'standard', 'pro')),
  check (plan_status in ('active', 'past_due', 'canceled', 'trial')),
  check (included_seconds >= 0)
);

create table if not exists relay_instances (
  id text primary key,
  session_id text unique,
  aws_instance_id text not null unique,
  region text not null,
  ami_id text not null,
  instance_type text not null,
  public_ip inet,
  srt_port integer not null default 9000,
  ws_url text not null default '',
  state text not null,
  launched_at timestamptz not null,
  terminated_at timestamptz,
  last_health_at timestamptz,
  created_at timestamptz not null default now(),
  check (state in ('provisioning', 'running', 'terminating', 'terminated', 'error'))
);

create table if not exists sessions (
  id text primary key,
  user_id text not null references users(id) on delete cascade,
  relay_instance_id text references relay_instances(id),
  status text not null,
  region text not null,
  idempotency_key uuid,
  requested_by text not null default 'dashboard',
  pair_token text not null default '',
  relay_ws_token text not null default '',
  started_at timestamptz not null,
  grace_started_at timestamptz,
  stopped_at timestamptz,
  max_session_seconds integer not null default 57600,
  grace_window_seconds integer not null default 600,
  duration_seconds integer not null default 0,
  reconciled_seconds integer not null default 0,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  check (status in ('provisioning', 'active', 'grace', 'stopped')),
  check (max_session_seconds > 0),
  check (grace_window_seconds > 0),
  check (duration_seconds >= 0),
  check (reconciled_seconds >= 0)
);

create unique index if not exists sessions_one_active_per_user
  on sessions(user_id)
  where status in ('provisioning', 'active', 'grace');

create table if not exists idempotency_records (
  id bigserial primary key,
  user_id text not null references users(id) on delete cascade,
  endpoint text not null,
  idempotency_key uuid not null,
  request_hash text not null,
  response_json jsonb not null,
  session_id text references sessions(id),
  created_at timestamptz not null default now(),
  expires_at timestamptz not null,
  unique (user_id, endpoint, idempotency_key)
);

create table if not exists usage_records (
  id text primary key,
  user_id text not null references users(id) on delete cascade,
  session_id text not null references sessions(id) on delete cascade,
  cycle_start_at timestamptz not null,
  cycle_end_at timestamptz not null,
  measured_seconds integer not null default 0,
  reconciled_seconds integer not null default 0,
  billable_seconds integer not null default 0,
  overage_seconds integer not null default 0,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  check (measured_seconds >= 0),
  check (reconciled_seconds >= 0),
  check (billable_seconds >= 0),
  check (overage_seconds >= 0)
);

create table if not exists relay_health_events (
  id bigserial primary key,
  session_id text not null references sessions(id) on delete cascade,
  relay_instance_id text not null references relay_instances(id) on delete cascade,
  observed_at timestamptz not null,
  ingest_active boolean not null,
  egress_active boolean not null,
  session_uptime_seconds integer not null,
  payload_json jsonb not null,
  created_at timestamptz not null default now(),
  check (session_uptime_seconds >= 0)
);

create index if not exists idx_sessions_user_created on sessions(user_id, created_at desc);
create index if not exists idx_sessions_status_updated on sessions(status, updated_at);
create index if not exists idx_idempotency_expires on idempotency_records(expires_at);
create index if not exists idx_usage_user_cycle on usage_records(user_id, cycle_start_at, cycle_end_at);
create index if not exists idx_relay_health_session on relay_health_events(session_id, observed_at desc);
