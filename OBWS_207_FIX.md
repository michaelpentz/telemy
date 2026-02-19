# OBS 207 Compatibility Fix (2026-02-19)

## Issue
Runtime logs showed:
- `DeserializeMessage(Error("invalid value: 207 ..."))`

This happens when OBS returns request status code `207` (`NotReady`) but older `obws` versions do not recognize that status code.

## Resolution
- Upgraded `obws` from `0.11.5` to `0.14.0`.
- Updated lockfile dependencies accordingly.

## Validation
Executed in `obs-telemetry-bridge`:
- `cargo test` -> passed (6/6)
- `cargo clippy -- -D warnings` -> passed
- `cargo build --release` -> passed

## Notes
No app-level logic changes were required for this fix. The compatibility issue was resolved at the dependency layer.
