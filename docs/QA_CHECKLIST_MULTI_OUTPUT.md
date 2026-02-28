# QA Checklist: Multi-Encode / Multi-Upload Telemetry

This checklist verifies the end-to-end flow of per-output metrics from OBS to the Aegis Dock.

## 1. Prerequisites
- [ ] OBS running with multiple stream outputs (e.g., standard stream + multiple StreamElements virtual outputs).
- [ ] `config.toml` updated with `[[outputs]]` mapping.
- [ ] Rust Core (`obs-telemetry-bridge`) running with IPC v1 expansion support.
- [ ] Aegis Dock open in OBS.

## 2. Configuration & Mapping
- [ ] Verify `config.toml` can map OBS output IDs (e.g., `adv_stream`) to display names (e.g., `Twitch`).
- [ ] Verify outputs can be assigned to groups (`Horizontal`, `Vertical`).
- [ ] Verify the `hidden: true` flag correctly moves outputs to the "Hidden" section in the dock.

## 3. Data Integrity (Rust -> IPC)
- [ ] Use an IPC sniffer or debug logs to verify `status_snapshot` contains the `outputs[]` array.
- [ ] Verify `bitrate_kbps` in the `outputs[]` items matches the values seen in OBS's own stats.
- [ ] Verify `fps`, `drop_pct`, and `encoding_lag_ms` are populated for each active output.
- [ ] Verify `bitrate_kbps` (top-level) still contains the aggregate bitrate for backward compatibility.

## 4. Bridge Projection (JS)
- [ ] Inspect `window.aegisDockNative.getState().outputs`.
- [ ] Verify outputs are correctly grouped by `encoder_group`.
- [ ] Verify `totalBitrateKbps` for each group is the sum of its active items.
- [ ] Verify `status` (`healthy`, `degraded`, `critical`) correctly reflects `drop_pct` thresholds.

## 5. UI Rendering (Aegis Dock)
- [ ] **Grouping:** Verify "Horizontal" and "Vertical" sections appear separately with their respective outputs.
- [ ] **Metrics:** Verify bitrate, FPS, and drop% are visible for each row.
- [ ] **Status:** Verify the status dot color matches the health of the output.
- [ ] **Inactive State:** Stop one output; verify its row dims and metrics show "--" but it remains in the group.
- [ ] **Hidden Section:**
    - [ ] Verify the "Hidden" section shows the correct count of hidden outputs.
    - [ ] Verify clicking "Show" reveals the hidden outputs (Recording, Virtual Camera).
    - [ ] Verify "Hide" collapses the section.
- [ ] **Resolution/Encoder:** Verify group headers show the detected resolution (e.g., 1920x1080) and encoder type.

## 6. Regression Testing
- [ ] Verify the main bonded bitrate graph still functions correctly.
- [ ] Verify the failover engine state (STUDIO/IRL) is unaffected by the expanded IPC payload.
- [ ] Verify the dock remains responsive when toggling multiple stream outputs on/off in OBS.
