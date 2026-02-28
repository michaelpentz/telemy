create table if not exists relay_manifests (
  region text primary key,
  ami_id text not null,
  default_instance_type text not null,
  updated_at timestamptz not null default now()
);
