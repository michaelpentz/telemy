# Release Notes - Telemy v0.0.3

## [2026-02-27] Dock UX & Auto-Scene Rule Updates

### Summary
This update introduces a major overhaul of the Auto Scene switching logic and UI, providing operators with granular control over threshold rules and improving readability across OBS themes.

### New Features & Improvements
- **Operator-Editable Auto-Scene Rules**: 
  - Dynamically **add or remove** custom switching rules directly in the dock.
  - Per-rule configuration: Custom labels, threshold (Mbps), and direct linking to real OBS scenes.
- **Compact & Expandable Rules UI**:
  - Rules now use a space-efficient collapsed row showing a summary (e.g., `12 Mbps -> Gameplay`).
  - An `Edit` affordance opens advanced controls, preventing UI crowding in narrow dock layouts.
- **Threshold Participation Control**:
  - Added a `Threshold` checkbox per rule. 
  - Enabled rules participate in bitrate-based auto-switching; disabled rules remain manual-only or command-triggered.
- **Manual Switch Lockout**:
  - Clicking a scene button now automatically disarms auto-switching (flips mode from `ARMED` to `MANUAL`).
  - This prevents immediate threshold-driven overrides after an operator makes a manual choice.
- **OBS Theme Integration & Readability**:
  - Full support for dynamic OBS theme switching (Yami, Dark, Light, etc.).
  - Specific readability pass for **Light Themes**; active and selected rows now use contrast-safe styling.
- **Real OBS Scene Inventory**:
  - Rule scene-link dropdowns are now populated with live scene data from the current OBS collection.

### Known Limitations
- **Threshold Guidance**: The persistent help text block was removed for space; guidance will move to tooltips in a future pass.
- **Layout**: Extreme vertical/narrow dock widths may still require minor padding adjustments.

---

## Upcoming Features (v1 Expansion - In Progress)

The following features are currently being specified and implemented for the `v1` API expansion:

- **Per-Link Relay Telemetry**:
  - Surfaces individual bonded connection health (T-Mobile, Verizon, WiFi, etc.) from the Aegis relay back into the OBS dock.
  - Full per-link metrics including instantaneous bitrate, RTT, packet loss, and jitter.
- **Multi-Encode / Multi-Upload Telemetry**:
  - Displays per-encoder and per-upload health in the dock (e.g., separate metrics for Horizontal vs. Vertical streams).
  - Grouped display by encoder with hide/show support for inactive outputs like Recording and Virtual Camera.
  - IPC v1 expansion to carry per-output arrays instead of single aggregate bitrate.
