# QA Checklist: OBS Dock Scene Rules

**Version:** 0.0.3  
**Date:** 2026-02-27  
**Scope:** Manual validation of React Dock UI, Auto-Scene Logic, and Theme Integration.

---

## 1. Dynamic Rule Management
- [ ] **Add Rule**: Click the `+` or `Add Rule` button.
  - *Expectation*: A new rule row appears with default values.
- [ ] **Remove Rule**: Click the trash/delete icon on a rule row.
  - *Expectation*: Rule is removed immediately; no UI hang.
- [ ] **Label Edit**: Modify the rule label.
  - *Expectation*: Text updates in the UI and persists during the session.

## 2. Rule Configuration & Linking
- [ ] **Scene Link Dropdown**: Open the scene selection dropdown for a rule.
  - *Expectation*: Lists all real scenes currently in the active OBS collection.
- [ ] **Threshold Value**: Modify the Mbps threshold for a rule.
  - *Expectation*: Value updates; compact row summary reflects the change (e.g., `15 Mbps`).
- [ ] **Threshold Checkbox Behavior**: Toggle the `Threshold` checkbox.
  - *Expectation*: 
    - **Checked**: Rule is eligible for auto-switching when bitrate drops below the value.
    - **Unchecked**: Rule is ignored by the auto-switch engine (manual/command only).

## 3. Manual Override & Mode Transitions
- [ ] **Auto-Switch Lockout**: Set mode to `ARMED` (Auto-Switching active). Click any scene button in the dock.
  - *Expectation*: 
    - Dock mode immediately switches to `MANUAL`.
    - Scene switch is executed.
    - Auto-switch engine does NOT immediately switch back (even if bitrate is below threshold).
- [ ] **Manual Scene Switch Result**: Observe logs during a manual switch.
  - *Expectation*: `action_result` payload shows `status: completed` and `ok: true`.

## 4. Theme & Readability
- [ ] **Dark/Yami Theme**: Switch OBS to a dark theme.
  - *Expectation*: High-contrast text; active rows clearly highlighted.
- [ ] **Light Theme**: Switch OBS to a light theme (e.g., "System" or "Light").
  - *Expectation*: Text is readable (not white-on-white); active/selected rows use contrast-safe background/border colors.

---

## Expected Log/Event Signatures

### Success Signatures
- **Action Result**: `[aegis-obs-shim] action_result: type=switch_scene status=completed ok=true`
- **IPC Update**: `[aegis-obs-shim] status_snapshot received` (observe mode change in payload)
- **UI Update**: `[aegis-dock-app] Rendering rule list (N rules)`

### Failure Signatures
- **Scene Missing**: `[aegis-obs-shim] switch_scene failed: scene_not_found`
- **Action Rejected**: `[aegis-obs-shim] action_result: status=rejected error="..."`

---

## Pass/Fail Status
- **Overall Result**: [ PASS / FAIL ]
- **Notes**: 
