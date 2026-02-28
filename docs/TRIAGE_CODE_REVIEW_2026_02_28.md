# Triage: Code Review Findings (2026-02-28)

This document triages the 15 findings from the Claude Code review of `telemy-v0.0.3`.

## Summary of Findings

| Severity | Count | Key Issues |
| :--- | :---: | :--- |
| **CRITICAL** | 3 | Security: IPC Access Control, Token Logging, Plaintext Vault (non-Win) |
| **IMPORTANT** | 7 | Functional: Idempotency Mismatch; Stability: Mutex Poisoning, Async Mutex |
| **MINOR** | 5 | Technical Debt: Duplication, Large Files, Script Paths |

---

## 1. Categorization by Owner

### Codex: Rust Core (`obs-telemetry-bridge`)
- **[CRITICAL] #1: IPC Named Pipe: NULL DACL** (Security)
- **[CRITICAL] #2: Server token logged in plaintext** (Security)
- **[CRITICAL] #3: Non-Windows vault plaintext storage** (Security)
- **[IMPORTANT] #4: Idempotency key format mismatch** (Functional - Rust side)
- **[IMPORTANT] #5: Mutex poison handling** (Resilience)
- **[IMPORTANT] #6: Grafana exporter error swallowing** (Resilience)
- **[IMPORTANT] #7: Bitrate calculation (average vs instantaneous)** (Correctness)
- **[IMPORTANT] #9: `std::sync::Mutex` in async context** (Stability)
- **[MINOR] #11: Duplicated client/key generation logic** (Maintenance)
- **[MINOR] #12: `server/mod.rs` file size** (Maintenance)
- **[MINOR] #13: `SeqCst` ordering in tray** (Optimization)

### Codex: Plugin Shim (`obs-plugin-shim`)
- **[IMPORTANT] #8: Hand-rolled MessagePack in C++** (Reliability/Maintenance)
- **[IMPORTANT] #10: Build artifacts in Git** (Repo Hygiene)

### Codex: Control Plane (`aegis-control-plane`)
- **[IMPORTANT] #4: Idempotency key format mismatch** (Functional - Go side)
- **[MINOR] #14: Unnecessary transaction in `GetSessionByID`** (Optimization)

### Shared / DevOps
- **[MINOR] #15: Hardcoded paths in PowerShell scripts** (Portability)

---

## 2. Dependencies

| Issue | Dependency | Impact |
| :--- | :--- | :--- |
| **#4 (Idempotency)** | **None** | **BLOCKER** for `relay/start` integration. Rust and Go must align on UUID-v4. |
| **#12 (Splitting server/mod.rs)** | **#9 (Async Mutex)** | Recommended to fix mutex usage while refactoring the module structure. |
| **#11 (Deduplication)** | **#4 (Idempotency)** | Fix the format in both places first, then deduplicate. |

---

## 3. Proposed Fix Ordering

### Phase 1: Immediate Functional & Security (High Priority)
1. **#4: Idempotency key format mismatch**
   - *Action:* Change Rust `generate_idempotency_key` to output UUID-v4. Ensure Go side validates strictly.
   - *Why:* Unblocks all relay-start functional testing.
2. **#1: IPC Named Pipe: NULL DACL**
   - *Action:* Implement restricted DACL for the named pipe.
   - *Why:* Prevents local privilege escalation/unauthorized command injection.
3. **#2: Server token logging**
   - *Action:* Truncate or remove token logging in `app/mod.rs`.
   - *Why:* Prevents accidental credential exposure in logs.

### Phase 2: Core Stability & Security Fallbacks
4. **#3: Non-Windows vault plaintext**
   - *Action:* Implement warning logs or basic obfuscation/keychain integration for Linux/macOS.
5. **#5: Mutex poison handling** & **#9: Async Mutex usage**
   - *Action:* Audit `server/mod.rs` and `app/mod.rs`. Switch to `tokio::sync::Mutex` where appropriate.
6. **#6: Grafana exporter error handling**
   - *Action:* Propagate errors from `exporter.record()`.

### Phase 3: Correctness & Reliability
7. **#7: Instantaneous Bitrate**
   - *Action:* Update `metrics/mod.rs` to use delta-based calculation.
8. **#8: C++ MessagePack library**
   - *Action:* Replace hand-rolled C++ logic with `msgpack-c`.
9. **#10: Gitignore updates**
   - *Action:* Fix `build*/` patterns to catch `build-obs-cef/`.

### Phase 4: Maintenance & Debt
10. **#11, #12, #13, #14, #15**
    - *Action:* Module refactoring, deduplication, and script cleanup.
