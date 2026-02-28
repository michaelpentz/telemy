# Documentation Index (v0.0.3)

This folder is the canonical source for Aegis/Telemy design and protocol specs.

## Read First

1. `STATE_MACHINE_v1.md`: runtime transition rules and scene decision invariants (plugin/core behavior target).
2. `IPC_PROTOCOL_v1.md`: plugin/core Named Pipe protocol and recovery behavior.
3. `API_SPEC_v1.md`: backend HTTP contracts, auth, idempotency, and error model.
4. `DB_SCHEMA_v1.md`: PostgreSQL tables, indexes, and operational jobs.
5. `OPERATIONS_METRICS.md`: scrape targets, metric families, and starter alert rules.

## Current Focus (2026-02-23)

- Primary implementation priority: hybrid OBS plugin + Rust core via IPC (`STATE_MACHINE_v1.md`, `IPC_PROTOCOL_v1.md`).
- Browser dashboard is a temporary validation/control surface, not the final v0.0.3 end-user UX.
- Backend/cloud path is ahead of client/plugin maturity and has been live-validated in AWS mode.

## Status and Planning

- `docs/CURRENT_STATUS.md`: concise current project status and immediate priorities.
- `docs/archive/HANDOFF_STATUS.md`: handoff index / compatibility stub (latest addenda pointers).
- `docs/archive/HANDOFF_HISTORY.md`: full historical handoff log and addenda archive.
- Archived `docs/archive/OVERHAUL-v0.0.3.md`: overhaul roadmap/checklist snapshot.
- Archived `docs/archive/ARCHITECTURE-v0.0.3-FIRST-PASS.md`: initial architecture mapping.
- Root `HANDOFF_STATUS.md` is a short redirect/snapshot for quick orientation.
- Do not add new root-level "next context" prompt files; update `docs/CURRENT_STATUS.md`, `docs/archive/HANDOFF_HISTORY.md`, and this index instead.

## Source of Truth Rules

- API behavior disagreements: `API_SPEC_v1.md` wins.
- Schema disagreements: migration SQL in `aegis-control-plane/migrations/` wins, then `DB_SCHEMA_v1.md`.
- Runtime behavior disagreements: `STATE_MACHINE_v1.md` and `IPC_PROTOCOL_v1.md` win for their domains.

## Updating Docs

- Keep spec changes and code changes in the same PR.
- Prefer updating an existing canonical file over adding a new standalone markdown file.
- When adding endpoints or jobs, update both:
  - relevant spec file in `docs/`
  - operational notes in `aegis-control-plane/README.md`
- When stopping work, add durable status to `docs/archive/HANDOFF_HISTORY.md` (new addendum with date), refresh `docs/CURRENT_STATUS.md` if priorities/state changed, and do not create a root prompt file.

## Client/Plugin Implementation Path (Current)

1. Define plugin-facing runtime behavior in `STATE_MACHINE_v1.md`.
2. Implement plugin/core IPC transport and messages per `IPC_PROTOCOL_v1.md`.
3. Build Rust core IPC server/runtime and status snapshot emission.
4. Integrate OBS plugin shim against IPC (dock UI + scene-switch/control hooks).
5. Keep browser dashboard as temporary validation/control surface until plugin path is sufficient.
