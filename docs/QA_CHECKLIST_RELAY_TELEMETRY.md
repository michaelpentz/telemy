# QA Checklist: Per-Link Relay Telemetry

This checklist verifies the flow of per-link SRT stats from the Aegis Relay to the OBS Dock.

## 1. Prerequisites
- [ ] Aegis Relay (AWS) instance active.
- [ ] Bonding-capable source (e.g., Larix Broadcaster on a phone with 5G + WiFi) connected to the relay.
- [ ] Control Plane (Go) running and receiving health reports from the relay.
- [ ] Rust Core (`obs-telemetry-bridge`) polling the control plane.

## 2. Relay -> Control Plane Integration
- [ ] Verify `POST /api/v1/relay/health` from the relay contains the `links[]` array.
- [ ] Verify `links[]` correctly identifies active connections (e.g., T-Mobile, WiFi).
- [ ] Verify `bitrate_kbps`, `rtt_ms`, `packet_loss_pct`, and `jitter_ms` are present for each link.

## 3. Control Plane -> Rust Core Integration
- [ ] Verify `GET /api/v1/relay/active` response includes the `links[]` array.
- [ ] Verify the control plane correctly aggregates stats into the `bonded` object.

## 4. Rust Core -> IPC Bridge Integration
- [ ] Inspect the `status_snapshot` IPC payload.
- [ ] Verify `relay.links[]` matches the data from the control plane.
- [ ] Verify `connections.items[]` (in the bridge projection) is populated using link data.
    - [ ] `name` maps to `link.label`.
    - [ ] `bitrate` maps to `link.bitrate_kbps`.
    - [ ] `status` maps to `link.status`.

## 5. UI Rendering (Aegis Dock - Connections Section)
- [ ] **Link List:** Verify individual links appear as separate rows (e.g., T-Mobile, Verizon, WiFi).
- [ ] **Metrics:** Verify instantaneous bitrate is shown per link.
- [ ] **Signal Bars:** Verify signal bar counts reflect link quality (RTT/loss).
- [ ] **Dynamic Updates:**
    - [ ] Disconnect one link (e.g., turn off WiFi); verify its row shows `disconnected` or disappears.
    - [ ] Reconnect the link; verify it reappears in the list.
- [ ] **Bonded Aggregate:**
    - [ ] Verify the bonded bitrate matches the sum of all link bitrates.
    - [ ] Verify the "Bonded" status matches the relay's reported `bonded.health`.

## 6. Edge Cases & Scaling
- [ ] **High Link Count:** Connect 4+ links; verify the Connections section in the dock is scrollable and readable.
- [ ] **Degraded State:** Simulate high packet loss on one link; verify its status changes to `degraded` and the signal bars reflect this.
- [ ] **Relay Failover:** Stop the relay session; verify the dock shows the transition to `inactive` or `grace`.
