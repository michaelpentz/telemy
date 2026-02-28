use crate::aegis::{
    ControlPlaneClient, RelaySession, RelayStartClientContext, RelayStartRequest, RelayStopRequest,
};
use crate::config::{Config, ThemeConfig};
use crate::ipc::{CoreIpcCommand, CoreIpcCommandSender, IpcDebugStatus, IpcDebugStatusHandle};
use crate::model::TelemetryFrame;
use crate::security::Vault;
use axum::{
    extract::{
        ws::{Message, WebSocket, WebSocketUpgrade},
        Json, Query, State,
    },
    http::{HeaderMap, StatusCode},
    response::{Html, IntoResponse},
    routing::{get, post},
    Form, Router,
};
use base64::{engine::general_purpose, Engine as _};
use rand::{distributions::Alphanumeric, Rng};
use serde::{Deserialize, Serialize};
use std::{
    collections::HashMap,
    net::SocketAddr,
    sync::{Arc, Mutex},
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use tokio::net::TcpListener;
use tokio::sync::watch;

#[derive(Clone)]
#[allow(dead_code)]
struct ServerState {
    token: String,
    rx: watch::Receiver<TelemetryFrame>,
    theme: ThemeConfig,
    vault: Arc<Mutex<Vault>>,
    grafana_configured: Arc<Mutex<bool>>,
    aegis_session_snapshot: Arc<Mutex<Option<RelaySession>>>,
    ipc_cmd_tx: CoreIpcCommandSender,
    ipc_debug_status: IpcDebugStatusHandle,
}

pub async fn start(
    addr: SocketAddr,
    token: String,
    rx: watch::Receiver<TelemetryFrame>,
    mut shutdown_rx: watch::Receiver<bool>,
    theme: ThemeConfig,
    vault: Arc<Mutex<Vault>>,
    grafana_configured: bool,
    aegis_session_snapshot: Arc<Mutex<Option<RelaySession>>>,
    ipc_cmd_tx: CoreIpcCommandSender,
    ipc_debug_status: IpcDebugStatusHandle,
) -> Result<(), Box<dyn std::error::Error>> {
    let state = Arc::new(ServerState {
        token,
        rx,
        theme,
        vault,
        grafana_configured: Arc::new(Mutex::new(grafana_configured)),
        aegis_session_snapshot,
        ipc_cmd_tx,
        ipc_debug_status,
    });

    let app = Router::new()
        .route("/health", get(health_check))
        .route("/obs", get(obs_page))
        .route("/ws", get(ws_handler))
        .route("/setup", get(setup_page))
        .route("/settings", get(settings_page))
        .route("/settings", post(settings_submit))
        .route("/output-names", get(get_output_names))
        .route("/output-names", post(save_output_names))
        .route("/grafana-dashboard", get(grafana_dashboard_download))
        .route("/grafana-dashboard/import", post(grafana_dashboard_import))
        .route("/aegis/status", get(get_aegis_status))
        .route("/aegis/start", post(post_aegis_start))
        .route("/aegis/stop", post(post_aegis_stop))
        .route("/ipc/status", get(get_ipc_status))
        .route("/ipc/switch-scene", post(post_ipc_switch_scene))
        .with_state(state);

    let listener = TcpListener::bind(addr).await?;
    axum::serve(listener, app)
        .with_graceful_shutdown(async move {
            let _ = shutdown_rx.changed().await;
        })
        .await?;

    Ok(())
}

async fn obs_page(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    // Support both Authorization header (for API access) and query param (for browser/Dock access)
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Allow) {
        return StatusCode::UNAUTHORIZED.into_response();
    }

    let css = theme_css(&state.theme);

    let html = r##"<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <title>OBS Telemetry</title>
  <style>
    :root {
      {{THEME_VARS}}
    }
    body {
      margin: 0;
      font-family: var(--font);
      background:
        radial-gradient(circle at 10% 0%, rgba(51,209,122,0.09), transparent 42%),
        radial-gradient(circle at 100% 0%, rgba(246,211,45,0.07), transparent 34%),
        linear-gradient(180deg, #07090d 0%, var(--bg) 38%, #090d14 100%);
      color: #e6f0ff;
    }
    .wrap { max-width: 1180px; margin: 0 auto; padding: 18px 16px 24px; }
    .row { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
    .badge {
      padding: 7px 10px;
      background: linear-gradient(180deg, rgba(255,255,255,0.02), rgba(255,255,255,0));
      border-radius: 999px;
      font-size: 12px;
      border: 1px solid var(--line);
      box-shadow: inset 0 0 0 1px rgba(255,255,255,0.01);
    }
    .shell { display: grid; gap: 12px; }
    .hero {
      background: linear-gradient(180deg, rgba(255,255,255,0.025), rgba(255,255,255,0.01));
      border: 1px solid var(--line);
      border-radius: 14px;
      padding: 14px;
      box-shadow: 0 14px 32px rgba(0,0,0,0.24);
    }
    .hero-header { display:flex; gap:12px; justify-content:space-between; align-items:flex-start; flex-wrap:wrap; }
    .hero-title { font-size: 18px; font-weight: 700; letter-spacing: 0.02em; }
    .hero-sub { color: var(--muted); font-size: 12px; margin-top: 4px; }
    .hero-right { display:flex; gap:8px; flex-wrap:wrap; align-items:center; }
    .link-badge { text-decoration:none; color:inherit; cursor:pointer; }
    .grid { display: grid; grid-template-columns: 1fr; gap: 8px; }
    .panel-card {
      background: linear-gradient(180deg, rgba(255,255,255,0.02), rgba(255,255,255,0.005));
      border: 1px solid var(--line);
      border-radius: 12px;
      padding: 12px;
    }
    .section-head { display:flex; justify-content:space-between; align-items:center; gap:8px; margin-bottom:8px; }
    .section-title { font-size: 12px; color: var(--muted); text-transform: uppercase; letter-spacing: 0.08em; }
    .output { background: rgba(255,255,255,0.015); border: 1px solid var(--line); border-radius: 8px; padding: 8px 10px; }
    .output-inactive { background: rgba(255,255,255,0.01); border: 1px solid var(--line); border-radius: 8px; padding: 8px 10px; opacity: 0.5; }
    .name { font-size: 13px; margin-bottom: 6px; }
    .bar { height: 8px; background: #0f141c; border: 1px solid var(--line); border-radius: 4px; overflow: hidden; }
    .fill { height: 100%; background: var(--good); width: 0%; }
    canvas { width: 100%; height: 140px; background: #0d121a; border: 1px solid var(--line); border-radius: 8px; }
    .muted { color: var(--muted); }
    .edit-btn { cursor: pointer; color: var(--muted); font-size: 11px; text-decoration: underline; margin-left: 10px; }
    .edit-btn:hover { color: var(--good); }
    .modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.8); z-index: 1000; }
    .modal-content { background: var(--panel); margin: 50px auto; padding: 20px; width: 90%; max-width: 600px; border: 1px solid var(--line); border-radius: 8px; max-height: 80vh; overflow-y: auto; }
    .modal-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
    .modal-title { font-size: 16px; font-weight: bold; }
    .close-btn { cursor: pointer; font-size: 20px; color: var(--muted); }
    .close-btn:hover { color: var(--bad); }
    .name-row { display: flex; gap: 10px; margin-bottom: 10px; align-items: center; }
    .name-row input { flex: 1; background: var(--bg); border: 1px solid var(--line); color: #e6f0ff; padding: 6px; border-radius: 4px; }
    .name-row .id-label { width: 150px; font-size: 11px; color: var(--muted); word-break: break-all; }
    .save-btn { background: var(--good); color: #0b0e12; border: none; padding: 10px 20px; border-radius: 4px; cursor: pointer; font-weight: bold; margin-top: 10px; }
    .save-btn:hover { opacity: 0.9; }
    .add-btn { background: rgba(255,255,255,0.015); color: var(--good); border: 1px solid var(--good); padding: 7px 12px; border-radius: 999px; cursor: pointer; font-size: 12px; margin-bottom: 10px; }
    .add-btn:hover { background: rgba(51,209,122,0.08); }
    .test-mode { border: 1px solid var(--warn); color: var(--warn); font-weight: bold; }
    .rec-badge { border: 1px solid var(--bad); color: var(--bad); font-weight: bold; }
    .toggle-row { display: flex; align-items: center; gap: 6px; margin-top: 10px; font-size: 11px; color: var(--muted); }
    .toggle-row input { accent-color: var(--good); }
    .stats-row { display: flex; gap: 10px; flex-wrap: wrap; margin-top: 8px; }
    .stat { padding: 6px 8px; background: rgba(255,255,255,0.015); border-radius: 8px; font-size: 11px; border: 1px solid var(--line); color: var(--muted); }
    .dashboard-grid { display:grid; grid-template-columns: 1.15fr 0.85fr; gap:12px; align-items:start; }
    .summary-grid { display:grid; grid-template-columns: repeat(3, minmax(0,1fr)); gap:10px; }
    .summary-box { border:1px solid var(--line); border-radius:10px; padding:10px; background: rgba(255,255,255,0.015); }
    .summary-label { color: var(--muted); font-size: 10px; text-transform: uppercase; letter-spacing: 0.08em; margin-bottom: 6px; }
    .summary-value { font-size: 12px; line-height: 1.45; }
    .details-shell { margin-top: 10px; border: 1px solid var(--line); border-radius: 10px; background: rgba(255,255,255,0.01); overflow: hidden; }
    .details-shell > summary { cursor: pointer; list-style: none; padding: 10px 12px; color: var(--muted); font-size: 12px; user-select: none; }
    .details-shell > summary::-webkit-details-marker { display: none; }
    .details-shell > summary::before { content: "▸ "; color: var(--good); }
    .details-shell[open] > summary::before { content: "▾ "; }
    .details-content { padding: 0 12px 12px; }
    .aegis-controls { display:flex; gap:8px; flex-wrap:wrap; align-items:center; }
    .aegis-actions { margin-top: 8px; }
    .toolbar-row { display:flex; justify-content:space-between; gap:8px; align-items:center; flex-wrap:wrap; margin-top:8px; }
    .toolbar-links { display:flex; align-items:center; gap:2px; flex-wrap:wrap; }
    @media (max-width: 860px) {
      .dashboard-grid { grid-template-columns: 1fr; }
      .summary-grid { grid-template-columns: 1fr; }
      .hero-header { align-items: stretch; }
      .hero-right { width: 100%; }
      .hero-right .badge, .hero-right .link-badge { width: fit-content; }
    }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="shell">
      <div class="hero">
        <div class="hero-header">
          <div>
            <div class="hero-title">Telemy Control Surface</div>
            <div class="hero-sub">Legacy dashboard shell with v0.0.3 Aegis controls and live status plumbing</div>
          </div>
          <div class="hero-right">
            <div class="badge" id="status">DISCONNECTED</div>
            <div class="badge" id="time">--</div>
            <a href="/settings?token={{TOKEN}}" class="badge link-badge">Settings</a>
          </div>
        </div>
        <div class="row" style="margin-top:10px;">
          <div class="badge" id="health">Health: --</div>
          <div class="badge" id="obs">OBS: --</div>
          <div class="badge" id="testmode" style="display:none;" class="test-mode">STUDIO MODE</div>
          <div class="badge rec-badge" id="recbadge" style="display:none;">REC</div>
          <div class="badge" id="sys">SYS: --</div>
          <div class="badge" id="net">NET: --</div>
          <div class="badge" id="aegis">AEGIS: --</div>
        </div>
      </div>

      <div class="dashboard-grid">
        <div class="panel-card">
          <div class="section-head">
            <div class="section-title">Live Summary</div>
            <div class="muted" style="font-size:11px;">Connection, system, and main stream info</div>
          </div>
          <div class="summary-grid">
            <div class="summary-box">
              <div class="summary-label">Connection</div>
              <div class="summary-value" id="summaryConn">OBS: --<br>Latency: --<br>Aegis: --</div>
            </div>
            <div class="summary-box">
              <div class="summary-label">System</div>
              <div class="summary-value" id="summarySystem">CPU: --<br>RAM: --<br>GPU/VRAM: --</div>
            </div>
            <div class="summary-box">
              <div class="summary-label">Main Stream / Encoder</div>
              <div class="summary-value" id="summaryMain">Bitrate: --<br>Drops: --<br>Lag/FPS: --</div>
            </div>
          </div>
          <details class="details-shell" id="diagDetails">
            <summary>Expanded Diagnostics</summary>
            <div class="details-content">
              <div class="section-head" style="margin-top:8px;">
                <div class="section-title">OBS Health Trend</div>
                <div class="muted" style="font-size:11px;">Graph shows overall health (1.0 = best)</div>
              </div>
              <canvas id="graph" width="600" height="140"></canvas>
              <div class="stats-row" id="statsRow">
                <div class="stat" id="statDisk">Disk: --</div>
                <div class="stat" id="statRender">Render missed: --</div>
                <div class="stat" id="statOutput">Encoder skipped: --</div>
                <div class="stat" id="statFps">FPS: --</div>
              </div>
            </div>
          </details>
        </div>

        <div class="panel-card">
          <div class="section-head">
            <div class="section-title">Aegis Relay Controls</div>
          </div>
          <div class="aegis-controls">
            <button class="add-btn" id="aegisStartBtn" style="margin-bottom:0;">Aegis Start</button>
            <button class="add-btn" id="aegisStopBtn" style="margin-bottom:0;">Aegis Stop</button>
            <span class="edit-btn" id="refreshAegisBtn" style="margin-left:0;">Refresh Aegis</span>
          </div>
          <div class="row aegis-actions" style="margin-top:8px;">
            <input id="ipcSceneName" type="text" value="BRB" placeholder="Scene name"
              style="background:var(--bg); border:1px solid var(--line); color:#e6f0ff; padding:7px 9px; border-radius:8px; min-width:110px;">
            <input id="ipcSceneReason" type="text" value="manual_debug" placeholder="Reason"
              style="background:var(--bg); border:1px solid var(--line); color:#e6f0ff; padding:7px 9px; border-radius:8px; min-width:130px;">
            <label style="display:flex; align-items:center; gap:6px; color:#9cb0d0; font-size:12px;">
              <input id="ipcAllowEmptyScene" type="checkbox">
              empty (debug)
            </label>
            <button class="add-btn" id="ipcSwitchSceneBtn" style="margin-bottom:0;">IPC Switch Scene</button>
          </div>
          <div class="stats-row aegis-actions">
            <div class="stat" id="aegisActionMsg" style="min-width:220px;">Aegis action: idle</div>
            <div class="stat" id="ipcStatusMsg" style="min-width:280px;">IPC: --</div>
          </div>
          <div class="toolbar-row">
            <div class="toggle-row" style="margin-top:0;">
              <input type="checkbox" id="hideInactive" /> <label for="hideInactive">Hide inactive outputs</label>
            </div>
            <div class="toolbar-links">
              <span class="edit-btn" id="editNamesBtn" style="margin-left:0;">Edit Output Names</span>
            </div>
          </div>
        </div>
      </div>

      <details class="panel-card details-shell" id="outputsDetails" open>
        <summary>Outputs</summary>
        <div class="details-content">
          <div class="section-head">
            <div class="section-title">Outputs</div>
          </div>
          <div class="grid" id="outputs"></div>
        </div>
      </details>
    </div>
  </div>
  
  <!-- Modal for editing output names -->
  <div class="modal" id="nameModal">
    <div class="modal-content">
      <div class="modal-header">
        <span class="modal-title">Edit Output Names</span>
        <span class="close-btn" id="closeModal">&times;</span>
      </div>
      <div id="nameEditor"></div>
      <button class="save-btn" id="saveNames">Save Changes</button>
      <div id="saveMsg" style="margin-top:10px; font-size:13px;"></div>
    </div>
  </div>
  
  <script>
    // Default pretty names for known outputs
    const defaultNames = {
      'adv_stream': 'Main Stream',
      'adv_file_output': 'Recording',
      'virtualcam_output': 'Virtual Camera'
    };
    
    // Output name mappings - will be loaded dynamically
    let outputNameMap = {};
    
    const params = new URLSearchParams(window.location.search);
    const token = params.get('token');
    const ws = new WebSocket(`ws://${window.location.host}/ws?token=${token}`);
    
    // Load output names from server
    async function loadOutputNames() {
      try {
        const res = await fetch(`/output-names`, {
          headers: {
            "Authorization": "Bearer " + token
          }
        });
        if (res.ok) {
          outputNameMap = await res.json();
        }
      } catch (e) {
        console.error('Failed to load output names:', e);
      }
    }
    
    // Load names on startup
    loadOutputNames();

    async function loadAegisStatus(refresh = false) {
      try {
        const url = refresh ? "/aegis/status?refresh=1" : "/aegis/status";
        const res = await fetch(url, {
          headers: {
            "Authorization": "Bearer " + token
          }
        });
        if (!res.ok) return;
        const data = await res.json();
        const session = data.session;
        if (!data.enabled) {
          aegisEl.textContent = "AEGIS: disabled";
          aegisEl.style.borderColor = "var(--line)";
          return;
        }
        if (!session) {
          aegisEl.textContent = "AEGIS: none";
          aegisEl.style.borderColor = "var(--line)";
          return;
        }
        const region = session.region ? ` @ ${session.region}` : "";
        aegisEl.textContent = `AEGIS: ${session.status}${region}`;
        aegisEl.style.borderColor = session.status === "active" ? "var(--good)" : "var(--warn)";
      } catch (e) {
        aegisEl.textContent = "AEGIS: error";
        aegisEl.style.borderColor = "var(--bad)";
      }
    }

    async function aegisAction(path) {
      try {
        aegisActionMsg.textContent = `Aegis action: ${path === "/aegis/start" ? "starting..." : "stopping..."}`;
        const res = await fetch(path, {
          method: "POST",
          headers: {
            "Authorization": "Bearer " + token
          }
        });
        const data = await res.json().catch(() => ({}));
        if (!res.ok) {
          aegisActionMsg.textContent = `Aegis action error: ${data.error || res.status}`;
          return;
        }
        aegisActionMsg.textContent = `Aegis action: ${data.message || "ok"}`;
        await loadAegisStatus(true);
      } catch (e) {
        aegisActionMsg.textContent = `Aegis action error: ${e.message}`;
      }
    }

    async function loadIpcStatus() {
      try {
        const res = await fetch("/ipc/status", {
          headers: {
            "Authorization": "Bearer " + token
          }
        });
        if (!res.ok) {
          ipcStatusMsg.textContent = `IPC: status error (${res.status})`;
          return;
        }
        const data = await res.json();
        const conn = data.session_connected ? "connected" : "disconnected";
        const pending = Number(data.pending_switch_count || 0);
        let tail = "";
        if (data.last_switch_result) {
          const r = data.last_switch_result;
          tail = ` | last=${r.status}${r.error ? ` (${r.error})` : ""}`;
        } else if (data.last_switch_request) {
          const r = data.last_switch_request;
          tail = ` | queued=${r.scene_name}`;
        }
        ipcStatusMsg.textContent = `IPC: ${conn} | pending=${pending}${tail}`;
      } catch (e) {
        ipcStatusMsg.textContent = `IPC: status error (${e.message})`;
      }
    }

    async function ipcSwitchScene() {
      try {
        const sceneName = (ipcSceneNameEl.value || "").trim();
        const reason = (ipcSceneReasonEl.value || "").trim();
        const allowEmpty = !!(ipcAllowEmptySceneEl && ipcAllowEmptySceneEl.checked);
        if (!sceneName && !allowEmpty) {
          aegisActionMsg.textContent = "Aegis action error: scene name required";
          return;
        }
        const displayScene = sceneName || "<empty>";
        aegisActionMsg.textContent = `Aegis action: queueing IPC switch '${displayScene}'...`;
        const res = await fetch("/ipc/switch-scene", {
          method: "POST",
          headers: {
            "Authorization": "Bearer " + token,
            "Content-Type": "application/json"
          },
          body: JSON.stringify({
            scene_name: sceneName,
            reason: reason || "manual_debug",
            deadline_ms: 550,
            allow_empty: allowEmpty
          })
        });
        const data = await res.json().catch(() => ({}));
        if (!res.ok) {
          aegisActionMsg.textContent = `Aegis action error: ${data.message || res.status}`;
          return;
        }
        aegisActionMsg.textContent = `Aegis action: ${data.message || "IPC switch queued"}`;
      } catch (e) {
        aegisActionMsg.textContent = `Aegis action error: ${e.message}`;
      }
    }

    const statusEl = document.getElementById("status");
    const timeEl = document.getElementById("time");
    const healthEl = document.getElementById("health");
    const obsEl = document.getElementById("obs");
    const testModeEl = document.getElementById("testmode");
    const recBadgeEl = document.getElementById("recbadge");
    const sysEl = document.getElementById("sys");
    const netEl = document.getElementById("net");
    const aegisEl = document.getElementById("aegis");
    const statDisk = document.getElementById("statDisk");
    const statRender = document.getElementById("statRender");
    const statOutput = document.getElementById("statOutput");
    const statFps = document.getElementById("statFps");
    const hideInactiveEl = document.getElementById("hideInactive");
    const summaryConnEl = document.getElementById("summaryConn");
    const summarySystemEl = document.getElementById("summarySystem");
    const summaryMainEl = document.getElementById("summaryMain");
    const outputsEl = document.getElementById("outputs");
    const canvas = document.getElementById("graph");
    const ctx = canvas.getContext("2d");
    const values = [];
    const maxPoints = 120;

    function healthColor(v) {
      if (v >= 0.95) return "var(--good)";
      if (v >= 0.90) return "var(--warn)";
      return "var(--bad)";
    }

    function draw() {
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      
      // Draw grid lines
      ctx.strokeStyle = "#1f2a3a";
      ctx.lineWidth = 1;
      ctx.beginPath();
      // 0.5 line (50%)
      ctx.moveTo(30, canvas.height / 2);
      ctx.lineTo(canvas.width, canvas.height / 2);
      // 0.0 line (0%)
      ctx.moveTo(30, canvas.height - 1);
      ctx.lineTo(canvas.width, canvas.height - 1);
      // 1.0 line (100%)
      ctx.moveTo(30, 1);
      ctx.lineTo(canvas.width, 1);
      ctx.stroke();
      
      // Draw labels
      ctx.fillStyle = "#8da3c1";
      ctx.font = "10px Arial";
      ctx.textAlign = "right";
      ctx.textBaseline = "middle";
      ctx.fillText("100%", 25, 6);
      ctx.fillText("50%", 25, canvas.height / 2);
      ctx.fillText("0%", 25, canvas.height - 6);
      
      // Draw graph
      ctx.strokeStyle = "#33d17a";
      ctx.lineWidth = 2;
      ctx.beginPath();
      
      const graphWidth = canvas.width - 30;
      values.forEach((v, i) => {
        const x = 30 + (i / Math.max(1, maxPoints - 1)) * graphWidth;
        const y = canvas.height - (v * canvas.height);
        // Clamp y to canvas bounds
        const clampedY = Math.max(0, Math.min(canvas.height, y));
        
        if (i === 0) ctx.moveTo(x, clampedY); else ctx.lineTo(x, clampedY);
      });
      ctx.stroke();
    }

    function renderOutputs(outputs) {
      outputsEl.innerHTML = "";
      const hideInactive = hideInactiveEl.checked;
      outputs.forEach(o => {
        const isActive = o.bitrate_kbps > 0 || o.fps > 0;

        if (hideInactive && !isActive) return;

        let displayName = outputNameMap[o.name] || defaultNames[o.name] || o.name;
        if (!isActive) displayName += " (Inactive)";

        const box = document.createElement("div");
        box.className = isActive ? "output" : "output-inactive";
        box.dataset.outputId = o.name;

        const name = document.createElement("div");
        name.className = "name";
        name.textContent = `${displayName} | ${o.bitrate_kbps} kbps | ${o.fps.toFixed(0)} fps | ${(o.drop_pct*100).toFixed(2)}% drop | ${o.encoding_lag_ms.toFixed(1)} ms lag`;

        const bar = document.createElement("div");
        bar.className = "bar";
        const fill = document.createElement("div");
        fill.className = "fill";
        const health = 1 - o.drop_pct;
        fill.style.width = `${Math.max(0, Math.min(100, health*100))}%`;
        fill.style.background = healthColor(health);
        bar.appendChild(fill);
        box.appendChild(name);
        box.appendChild(bar);
        outputsEl.appendChild(box);
      });
    }

    function pickMainOutput(outputs) {
      if (!outputs || outputs.length === 0) return null;
      return outputs.find(o => o.name === "adv_stream")
        || outputs.find(o => o.bitrate_kbps > 0 || o.fps > 0)
        || outputs[0];
    }

    function updateSummaryPanels(data) {
      const aegisText = (aegisEl.textContent || "AEGIS: --").replace(/^AEGIS:\s*/, "");
      const obsConn = data.obs.connected ? "Connected" : "Disconnected";
      const obsMode = data.obs.streaming ? "Streaming" : "Idle";
      summaryConnEl.innerHTML = `OBS: ${obsConn} (${obsMode})<br>Latency: ${data.network.latency_ms.toFixed(0)} ms<br>Aegis: ${aegisText}`;

      const gpuPctText = data.system.gpu_percent != null ? `${data.system.gpu_percent.toFixed(0)}%` : "n/a";
      const gpuTempText = data.system.gpu_temp_c != null ? ` ${data.system.gpu_temp_c.toFixed(0)}C` : "";
      summarySystemEl.innerHTML = `CPU: ${data.system.cpu_percent.toFixed(0)}%<br>RAM: ${data.system.mem_percent.toFixed(0)}%<br>GPU/VRAM: ${gpuPctText}${gpuTempText} / n/a`;

      const main = pickMainOutput(data.outputs);
      if (!main) {
        summaryMainEl.innerHTML = "Bitrate: --<br>Drops: --<br>Lag/FPS: --";
        return;
      }
      summaryMainEl.innerHTML =
        `Bitrate: ${main.bitrate_kbps} kbps (${main.name})<br>` +
        `Drops: ${(main.drop_pct * 100).toFixed(2)}%<br>` +
        `Lag/FPS: ${main.encoding_lag_ms.toFixed(1)} ms / ${main.fps.toFixed(1)} fps`;
    }

    ws.onopen = () => { statusEl.textContent = "CONNECTED"; };
    ws.onclose = () => { statusEl.textContent = "DISCONNECTED"; };
    ws.onmessage = (event) => {
      const data = JSON.parse(event.data);
      timeEl.textContent = new Date(data.ts * 1000).toLocaleTimeString();
      healthEl.textContent = `Health: ${(data.health*100).toFixed(1)}%`;
      healthEl.style.borderColor = healthColor(data.health);
      obsEl.textContent = `OBS: ${data.obs.streaming ? "LIVE" : "IDLE"} | dropped ${data.obs.total_dropped_frames}`;

      // Studio mode badge
      testModeEl.style.display = data.obs.studio_mode ? "block" : "none";

      // Recording badge
      recBadgeEl.style.display = data.obs.recording ? "block" : "none";

      // System: include GPU temp if available
      const gpuPct = data.system.gpu_percent ?? 0;
      const gpuTemp = data.system.gpu_temp_c != null ? ` ${data.system.gpu_temp_c.toFixed(0)}C` : "";
      sysEl.textContent = `SYS: CPU ${data.system.cpu_percent.toFixed(0)}% | MEM ${data.system.mem_percent.toFixed(0)}% | GPU ${gpuPct}%${gpuTemp}`;

      // Network: show both upload and download
      netEl.textContent = `NET: UP ${data.network.upload_mbps.toFixed(1)} | DN ${data.network.download_mbps.toFixed(1)} Mb/s | LAT ${data.network.latency_ms.toFixed(0)} ms`;

      // OBS Stats row
      const diskGb = (data.obs.available_disk_space_mb / 1024).toFixed(1);
      statDisk.textContent = `Disk: ${diskGb} GB`;
      statRender.textContent = `Render missed: ${data.obs.render_missed_frames} / ${data.obs.render_total_frames}`;
      statOutput.textContent = `Encoder skipped: ${data.obs.output_skipped_frames} / ${data.obs.output_total_frames}`;
      statFps.textContent = `FPS: ${data.obs.active_fps.toFixed(1)}`;
      updateSummaryPanels(data);

      values.push(data.health);
      if (values.length > maxPoints) values.shift();
      draw();
      renderOutputs(data.outputs);
    };
    
    // Modal functionality for editing output names
    const modal = document.getElementById("nameModal");
    const editBtn = document.getElementById("editNamesBtn");
    const closeBtn = document.getElementById("closeModal");
    const nameEditor = document.getElementById("nameEditor");
    const saveBtn = document.getElementById("saveNames");
    const saveMsg = document.getElementById("saveMsg");
    const refreshAegisBtn = document.getElementById("refreshAegisBtn");
    const aegisStartBtn = document.getElementById("aegisStartBtn");
    const aegisStopBtn = document.getElementById("aegisStopBtn");
    const ipcSceneNameEl = document.getElementById("ipcSceneName");
    const ipcSceneReasonEl = document.getElementById("ipcSceneReason");
    const ipcAllowEmptySceneEl = document.getElementById("ipcAllowEmptyScene");
    const ipcSwitchSceneBtn = document.getElementById("ipcSwitchSceneBtn");
    const aegisActionMsg = document.getElementById("aegisActionMsg");
    const ipcStatusMsg = document.getElementById("ipcStatusMsg");

    loadAegisStatus();
    loadIpcStatus();
    setInterval(() => loadAegisStatus(false), 10000);
    setInterval(() => loadIpcStatus(), 2000);
    refreshAegisBtn.onclick = () => loadAegisStatus(true);
    aegisStartBtn.onclick = () => aegisAction("/aegis/start");
    aegisStopBtn.onclick = () => aegisAction("/aegis/stop");
    ipcSwitchSceneBtn.onclick = () => ipcSwitchScene();
    
    editBtn.onclick = () => {
      modal.style.display = "block";
      populateNameEditor();
    };
    
    closeBtn.onclick = () => {
      modal.style.display = "none";
    };
    
    window.onclick = (e) => {
      if (e.target === modal) modal.style.display = "none";
    };
    
    function populateNameEditor() {
      nameEditor.innerHTML = "";
      
      // Add currently visible outputs
      const currentOutputs = Array.from(document.querySelectorAll(".output, .output-inactive"));
      const seenIds = new Set();
      
      currentOutputs.forEach(box => {
        // Use the real ID stored in dataset
        const id = box.dataset.outputId;
        
        if (id && !seenIds.has(id) && !defaultNames[id]) {
          seenIds.add(id);
          const currentName = outputNameMap[id] || id;
          addNameRow(id, currentName);
        }
      });
      
      if (seenIds.size === 0) {
        nameEditor.innerHTML = "<div class=\"muted\">No custom outputs detected yet. Start streaming to see outputs.</div>";
      }
    }
    
    function addNameRow(id, name) {
      const row = document.createElement("div");
      row.className = "name-row";
      row.innerHTML = `
        <span class="id-label">${id}</span>
        <input type="text" data-id="${id}" value="${name}" placeholder="Display name">
      `;
      nameEditor.appendChild(row);
    }
    
    saveBtn.onclick = async () => {
      const inputs = nameEditor.querySelectorAll("input");
      const mappings = {};
      
      inputs.forEach(input => {
        const id = input.getAttribute("data-id");
        const name = input.value.trim();
        if (name && name !== id) {
          mappings[id] = name;
        }
      });
      
      try {
        const res = await fetch("/output-names", {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            "Authorization": "Bearer " + token
          },
          body: JSON.stringify(mappings)
        });
        
        if (res.ok) {
          saveMsg.textContent = "Saved! Refresh the page to see changes.";
          saveMsg.style.color = "var(--good)";
          setTimeout(() => {
            modal.style.display = "none";
            location.reload();
          }, 1500);
        } else {
          saveMsg.textContent = "Failed to save.";
          saveMsg.style.color = "var(--bad)";
        }
      } catch (err) {
        saveMsg.textContent = "Error: " + err.message;
        saveMsg.style.color = "var(--bad)";
      }
    };
  </script>
</body>
</html>"##;

    let html = html
        .replace("{{THEME_VARS}}", &css)
        .replace("{{TOKEN}}", &html_escape(&state.token));
    Html(html).into_response()
}

#[derive(Deserialize)]
struct SettingsForm {
    obs_host: String,
    obs_port: u16,
    obs_password: Option<String>,
    grafana_interval: u64,
    grafana_endpoint: Option<String>,
    grafana_instance_id: Option<String>,
    grafana_api_token: Option<String>,
}

async fn settings_page(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Allow) {
        return StatusCode::UNAUTHORIZED.into_response();
    }

    let config = match Config::load() {
        Ok(c) => c,
        Err(e) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                format!("Failed to load config: {}", e),
            )
                .into_response()
        }
    };
    let css = theme_css(&state.theme);

    let grafana_configured = *state.grafana_configured.lock().unwrap();
    let grafana_status = if grafana_configured {
        r#"<div class="status status-ok">Grafana Cloud: Connected</div>"#
    } else {
        r#"<div class="status status-off">Grafana Cloud: Not Configured</div>"#
    };

    let grafana_endpoint = config.grafana.endpoint.as_deref().unwrap_or("");

    let html = format!(
        r#"<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <title>Telemy - Settings</title>
  <style>
    :root {{ {css} }}
    body {{ margin:0; font-family:var(--font); background:var(--bg); color:#e6f0ff; }}
    .wrap {{ max-width:480px; margin:40px auto; padding:0 16px; }}
    h1 {{ font-size:20px; margin-bottom:20px; }}
    h2 {{ font-size:16px; margin-top:28px; margin-bottom:8px; border-top:1px solid var(--line); padding-top:18px; }}
    label {{ display:block; font-size:13px; color:var(--muted); margin-bottom:4px; margin-top:14px; }}
    input {{ width:100%; box-sizing:border-box; padding:8px 10px; background:var(--panel);
             border:1px solid var(--line); border-radius:4px; color:#e6f0ff; font-size:14px;
             font-family:var(--font); }}
    input:focus {{ outline:none; border-color:var(--good); }}
    button {{ margin-top:20px; padding:10px 20px; background:var(--good); color:#0b0e12;
              border:none; border-radius:4px; font-size:14px; font-weight:bold; cursor:pointer; }}
    button:hover {{ opacity:0.9; }}
    .msg {{ margin-top:14px; padding:8px 12px; border-radius:6px; font-size:13px; display:none; }}
    .msg-ok {{ background:#1a2e1a; border:1px solid var(--good); color:var(--good); display:block; }}
    .msg-err {{ background:#2e1a1a; border:1px solid var(--bad); color:var(--bad); display:block; }}
    .back {{ font-size:12px; color:var(--muted); text-decoration:none; margin-bottom:20px; display:inline-block; }}
    .back:hover {{ color:#e6f0ff; }}
    .help {{ color:var(--muted); font-size:11px; margin-top:2px; }}
    .status {{ padding:8px 12px; border-radius:6px; margin-bottom:12px; font-size:13px; }}
    .status-ok {{ background:#1a2e1a; border:1px solid var(--good); color:var(--good); }}
    .status-off {{ background:#2e1a1a; border:1px solid var(--bad); color:var(--bad); }}
    .note {{ color:var(--muted); font-size:12px; margin-top:8px; }}
  </style>
</head>
<body>
  <div class="wrap">
    <a href="/obs?token={token}" class="back">&larr; Back to Dashboard</a>
    <h1>Settings</h1>
    <div id="msg" class="msg"></div>
    <form id="settingsForm">

      <h2>OBS Connection</h2>
      <label for="obs_host">OBS Host</label>
      <input id="obs_host" name="obs_host" type="text" value="{obs_host}" required />

      <label for="obs_port">OBS WebSocket Port</label>
      <input id="obs_port" name="obs_port" type="number" value="{obs_port}" required />

      <label for="obs_password">OBS WebSocket Password</label>
      <input id="obs_password" name="obs_password" type="password" placeholder="Leave blank to keep current" />
      <div class="help">Only fill in to change the stored password</div>

      <h2>Grafana Cloud</h2>
      {grafana_status}

      <label for="grafana_endpoint">OTLP Endpoint</label>
      <input id="grafana_endpoint" name="grafana_endpoint" type="url" value="{grafana_endpoint}"
             placeholder="https://otlp-gateway-prod-us-east-0.grafana.net/otlp" />
      <div class="help">Found in Grafana Cloud &rarr; OpenTelemetry &rarr; Configure</div>

      <label for="grafana_instance_id">Instance ID</label>
      <input id="grafana_instance_id" name="grafana_instance_id" type="text"
             placeholder="123456" />
      <div class="help">Your Grafana Cloud stack instance number</div>

      <label for="grafana_api_token">API Token</label>
      <input id="grafana_api_token" name="grafana_api_token" type="password"
             placeholder="glc_eyJ..." />
      <div class="help">Generate under Security &rarr; API Keys with MetricsPublisher role</div>

      <label for="grafana_interval">Push Interval (ms)</label>
      <input id="grafana_interval" name="grafana_interval" type="number" value="{grafana_interval}" required />

      <div class="note">Restart Telemy after saving for connection changes to take effect.</div>

      <button type="submit">Save Changes</button>
    </form>

    <h2>Grafana Dashboard</h2>
    <div class="note" style="margin-bottom:12px;">Import a pre-built Telemy dashboard into Grafana to visualize your metrics.</div>
    <a href="/grafana-dashboard?token={token}" download="telemy-dashboard.json"
       style="display:inline-block; padding:8px 16px; background:var(--panel); border:1px solid var(--line);
              border-radius:4px; color:#e6f0ff; text-decoration:none; font-size:13px; cursor:pointer;">
      Download Dashboard JSON
    </a>
    <div class="help" style="margin-top:6px;">Import this file in Grafana &rarr; Dashboards &rarr; Import</div>

    <details style="margin-top:16px;">
      <summary style="cursor:pointer; color:var(--muted); font-size:13px;">Auto-import via Grafana API (optional)</summary>
      <div style="margin-top:10px;">
        <label for="grafana_url">Grafana URL</label>
        <input id="grafana_url" type="url" placeholder="https://yourstack.grafana.net" />
        <div class="help">Your Grafana instance URL (not the OTLP endpoint)</div>

        <label for="grafana_org_key">Service Account Token</label>
        <input id="grafana_org_key" type="password" placeholder="glsa_..." />
        <div class="help">Needs Dashboard Editor permissions. Create under Administration &rarr; Service Accounts.</div>

        <button type="button" id="importBtn"
                style="margin-top:12px; padding:8px 16px; background:var(--panel); border:1px solid var(--good);
                       color:var(--good); border-radius:4px; font-size:13px; cursor:pointer;">
          Import Dashboard
        </button>
        <div id="importMsg" class="msg" style="margin-top:8px;"></div>
      </div>
    </details>
  </div>
  <script>
    const params = new URLSearchParams(window.location.search);
    const token = params.get("token");

    document.getElementById("settingsForm").addEventListener("submit", async (e) => {{
      e.preventDefault();
      const msg = document.getElementById("msg");
      const data = new URLSearchParams(new FormData(e.target));
      try {{
        const res = await fetch("/settings", {{
          method: "POST",
          headers: {{
            "Content-Type": "application/x-www-form-urlencoded",
            "Authorization": "Bearer " + token
          }},
          body: data,
        }});
        const text = await res.text();
        msg.textContent = text;
        msg.className = res.ok ? "msg msg-ok" : "msg msg-err";
      }} catch (err) {{
        msg.textContent = "Request failed: " + err.message;
        msg.className = "msg msg-err";
      }}
    }});

    document.getElementById("importBtn").addEventListener("click", async () => {{
      const importMsg = document.getElementById("importMsg");
      const grafanaUrl = document.getElementById("grafana_url").value.trim();
      const grafanaKey = document.getElementById("grafana_org_key").value.trim();
      if (!grafanaUrl || !grafanaKey) {{
        importMsg.textContent = "Both Grafana URL and API key are required.";
        importMsg.className = "msg msg-err";
        return;
      }}
      const data = new URLSearchParams({{ grafana_url: grafanaUrl, grafana_api_key: grafanaKey }});
      try {{
        const res = await fetch("/grafana-dashboard/import?token=" + token, {{
          method: "POST",
          headers: {{
            "Content-Type": "application/x-www-form-urlencoded",
            "Authorization": "Bearer " + token
          }},
          body: data,
        }});
        const text = await res.text();
        importMsg.textContent = text;
        importMsg.className = res.ok ? "msg msg-ok" : "msg msg-err";
      }} catch (err) {{
        importMsg.textContent = "Request failed: " + err.message;
        importMsg.className = "msg msg-err";
      }}
    }});
  </script>
</body>
</html>"#,
        css = css,
        token = html_escape(&state.token),
        obs_host = html_escape(&config.obs.host),
        obs_port = config.obs.port,
        grafana_status = grafana_status,
        grafana_endpoint = html_escape(grafana_endpoint),
        grafana_interval = config.grafana.push_interval_ms
    );

    Html(html).into_response()
}

async fn settings_submit(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
    Form(form): Form<SettingsForm>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized".to_string()).into_response();
    }

    let mut config = match Config::load() {
        Ok(c) => c,
        Err(e) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                format!("Failed to load config: {}", e),
            )
                .into_response()
        }
    };

    // OBS settings
    config.obs.host = form.obs_host;
    config.obs.port = form.obs_port;

    // OBS password — only update if user provided a new one
    if let Some(ref pw) = form.obs_password {
        if !pw.is_empty() {
            let mut vault = state.vault.lock().unwrap();
            if let Err(e) = vault.store("obs_password", pw) {
                return (
                    StatusCode::INTERNAL_SERVER_ERROR,
                    format!("Failed to store OBS password: {}", e),
                )
                    .into_response();
            }
            config.obs.password_key = Some("obs_password".to_string());
        }
    }

    // Grafana settings
    config.grafana.push_interval_ms = form.grafana_interval;

    // Grafana credentials — only update if all three fields are provided
    let endpoint = form
        .grafana_endpoint
        .as_deref()
        .unwrap_or("")
        .trim()
        .to_string();
    let instance_id = form
        .grafana_instance_id
        .as_deref()
        .unwrap_or("")
        .trim()
        .to_string();
    let api_token = form
        .grafana_api_token
        .as_deref()
        .unwrap_or("")
        .trim()
        .to_string();

    if !endpoint.is_empty() && !instance_id.is_empty() && !api_token.is_empty() {
        let credentials = format!("{}:{}", instance_id, api_token);
        let encoded = general_purpose::STANDARD.encode(credentials.as_bytes());
        let auth_value = format!("Basic {}", encoded);

        {
            let mut vault = state.vault.lock().unwrap();
            if let Err(e) = vault.store("grafana_auth", &auth_value) {
                return (
                    StatusCode::INTERNAL_SERVER_ERROR,
                    format!("Failed to store Grafana credentials: {}", e),
                )
                    .into_response();
            }
        }

        config.grafana.enabled = true;
        config.grafana.endpoint = Some(endpoint);
        config.grafana.auth_value_key = Some("grafana_auth".to_string());
        *state.grafana_configured.lock().unwrap() = true;
    } else if !endpoint.is_empty() {
        // Allow updating just the endpoint without re-entering credentials
        config.grafana.endpoint = Some(endpoint);
    }

    match config.save() {
        Ok(_) => (
            StatusCode::OK,
            "Settings saved. Restart required for connection changes to take effect.".to_string(),
        )
            .into_response(),
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("Failed to save config: {}", e),
        )
            .into_response(),
    }
}

async fn setup_page(query: Query<HashMap<String, String>>) -> impl IntoResponse {
    // Redirect /setup to /settings (Grafana config is now in settings)
    let token = query.0.get("token").cloned().unwrap_or_default();
    axum::response::Redirect::temporary(&format!("/settings?token={}", html_escape(&token)))
        .into_response()
}

async fn ws_handler(
    State(state): State<Arc<ServerState>>,
    ws: WebSocketUpgrade,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    // Native browser WebSocket clients cannot set Authorization headers directly.
    // Keep query-token fallback here for local dashboard compatibility.
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Allow) {
        return StatusCode::UNAUTHORIZED.into_response();
    }

    let rx = state.rx.clone();
    ws.on_upgrade(move |socket| handle_socket(socket, rx))
}

async fn handle_socket(mut socket: WebSocket, rx: watch::Receiver<TelemetryFrame>) {
    let mut ticker = tokio::time::interval(Duration::from_millis(500));

    loop {
        tokio::select! {
            _ = ticker.tick() => {
                let frame = rx.borrow().clone();
                let payload = serde_json::json!({
                    "ts": frame.timestamp_unix,
                    "health": frame.health,
                    "obs": frame.obs,
                    "system": frame.system,
                    "network": frame.network,
                    "outputs": frame.streams,
                });
                if socket.send(Message::Text(payload.to_string())).await.is_err() {
                    break;
                }
            }
            msg = socket.recv() => {
                match msg {
                    Some(Ok(Message::Close(_))) | None => break,
                    _ => {}
                }
            }
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum QueryTokenPolicy {
    Allow,
    Deny,
}

fn is_token_valid(
    headers: &HeaderMap,
    query: &HashMap<String, String>,
    token: &str,
    query_policy: QueryTokenPolicy,
) -> bool {
    // First check Authorization header (preferred for API access)
    // Format: "Bearer <token>"
    if let Some(auth_header) = headers.get("authorization") {
        if let Ok(auth_str) = auth_header.to_str() {
            if let Some(provided_token) = auth_str.strip_prefix("Bearer ") {
                return provided_token == token;
            }
        }
    }

    if query_policy == QueryTokenPolicy::Allow {
        // Fall back to query parameter for browser/Dock GET routes.
        return query.get("token").map(|t| t == token).unwrap_or(false);
    }

    false
}

async fn health_check() -> impl IntoResponse {
    (
        StatusCode::OK,
        axum::Json(serde_json::json!({
            "status": "healthy",
            "timestamp": std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap_or_default()
                .as_secs()
        })),
    )
}

fn html_escape(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&#x27;")
}

fn theme_css(theme: &ThemeConfig) -> String {
    format!(
        "--font: {}; --bg: {}; --panel: {}; --muted: {}; --good: {}; --warn: {}; --bad: {}; --line: {};",
        theme.font_family,
        theme.bg,
        theme.panel,
        theme.muted,
        theme.good,
        theme.warn,
        theme.bad,
        theme.line
    )
}

#[derive(Deserialize)]
struct OutputNamesPayload {
    #[serde(flatten)]
    names: HashMap<String, String>,
}

async fn get_output_names(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized").into_response();
    }

    // Load current config to get latest names
    match Config::load() {
        Ok(config) => (StatusCode::OK, axum::Json(config.output_names)).into_response(),
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("Failed to load config: {}", e),
        )
            .into_response(),
    }
}

async fn save_output_names(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
    axum::Json(payload): axum::Json<OutputNamesPayload>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized").into_response();
    }

    // Load current config
    let mut config = match Config::load() {
        Ok(cfg) => cfg,
        Err(e) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                format!("Failed to load config: {}", e),
            )
                .into_response();
        }
    };

    // Merge new names with existing
    for (id, name) in payload.names {
        if name.trim().is_empty() {
            config.output_names.remove(&id);
        } else {
            config.output_names.insert(id, name);
        }
    }

    // Save config
    match config.save() {
        Ok(()) => (StatusCode::OK, "Output names saved").into_response(),
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("Failed to save config: {}", e),
        )
            .into_response(),
    }
}

const GRAFANA_DASHBOARD_JSON: &str = include_str!("../../assets/grafana-dashboard.json");

async fn grafana_dashboard_download(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Allow) {
        return StatusCode::UNAUTHORIZED.into_response();
    }

    (
        StatusCode::OK,
        [
            ("content-type", "application/json"),
            (
                "content-disposition",
                "attachment; filename=\"telemy-dashboard.json\"",
            ),
        ],
        GRAFANA_DASHBOARD_JSON,
    )
        .into_response()
}

#[derive(Deserialize)]
struct GrafanaImportForm {
    grafana_url: String,
    grafana_api_key: String,
}

async fn grafana_dashboard_import(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
    Form(form): Form<GrafanaImportForm>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized".to_string()).into_response();
    }

    let url = form.grafana_url.trim().trim_end_matches('/');
    let api_key = form.grafana_api_key.trim();

    if url.is_empty() || api_key.is_empty() {
        return (
            StatusCode::BAD_REQUEST,
            "Grafana URL and API key are required".to_string(),
        )
            .into_response();
    }

    let import_url = format!("{}/api/dashboards/db", url);

    let client = match reqwest::Client::builder()
        .timeout(Duration::from_secs(10))
        .build()
    {
        Ok(c) => c,
        Err(e) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                format!("HTTP client error: {}", e),
            )
                .into_response()
        }
    };

    let res = client
        .post(&import_url)
        .header("Authorization", format!("Bearer {}", api_key))
        .header("Content-Type", "application/json")
        .body(GRAFANA_DASHBOARD_JSON)
        .send()
        .await;

    match res {
        Ok(resp) => {
            let status = resp.status();
            let body = resp.text().await.unwrap_or_default();
            if status.is_success() {
                (
                    StatusCode::OK,
                    "Dashboard imported successfully into Grafana.".to_string(),
                )
                    .into_response()
            } else {
                (
                    StatusCode::BAD_GATEWAY,
                    format!("Grafana returned {}: {}", status, body),
                )
                    .into_response()
            }
        }
        Err(e) => (
            StatusCode::BAD_GATEWAY,
            format!("Failed to reach Grafana: {}", e),
        )
            .into_response(),
    }
}

#[derive(Serialize)]
struct AegisStatusResponse {
    enabled: bool,
    session: Option<RelaySession>,
    refreshed: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
}

#[derive(Serialize)]
struct AegisActionResponse {
    ok: bool,
    message: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    session: Option<RelaySession>,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
}

#[derive(Debug, Deserialize)]
struct IpcSwitchSceneRequest {
    scene_name: String,
    #[serde(default)]
    reason: Option<String>,
    #[serde(default)]
    deadline_ms: Option<u64>,
    #[serde(default)]
    allow_empty: Option<bool>,
}

#[derive(Debug, Serialize)]
struct IpcSwitchSceneResponse {
    ok: bool,
    message: String,
}

async fn get_ipc_status(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized").into_response();
    }

    let snapshot: IpcDebugStatus = state.ipc_debug_status.lock().unwrap().clone();
    (StatusCode::OK, axum::Json(snapshot)).into_response()
}

async fn get_aegis_status(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Allow) {
        return StatusCode::UNAUTHORIZED.into_response();
    }

    let refresh_requested = query
        .0
        .get("refresh")
        .map(|v| v == "1" || v.eq_ignore_ascii_case("true"))
        .unwrap_or(false);

    let config = match Config::load() {
        Ok(cfg) => cfg,
        Err(err) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                axum::Json(AegisStatusResponse {
                    enabled: false,
                    session: state.aegis_session_snapshot.lock().unwrap().clone(),
                    refreshed: false,
                    error: Some(format!("config load failed: {err}")),
                }),
            )
                .into_response();
        }
    };

    if !config.aegis.enabled {
        return (
            StatusCode::OK,
            axum::Json(AegisStatusResponse {
                enabled: false,
                session: None,
                refreshed: false,
                error: None,
            }),
        )
            .into_response();
    }

    if refresh_requested {
        let client = {
            let vault = state.vault.lock().unwrap();
            build_aegis_client_from_config(&config, &vault).map_err(|err| err.to_string())
        };
        let refreshed = match client {
            Ok(client) => match client.relay_active().await {
                Ok(session) => {
                    *state.aegis_session_snapshot.lock().unwrap() = session.clone();
                    Ok(session)
                }
                Err(err) => Err(format!("{err}")),
            },
            Err(err) => Err(format!("{err}")),
        };

        return match refreshed {
            Ok(session) => (
                StatusCode::OK,
                axum::Json(AegisStatusResponse {
                    enabled: true,
                    session,
                    refreshed: true,
                    error: None,
                }),
            )
                .into_response(),
            Err(err) => (
                StatusCode::BAD_GATEWAY,
                axum::Json(AegisStatusResponse {
                    enabled: true,
                    session: state.aegis_session_snapshot.lock().unwrap().clone(),
                    refreshed: false,
                    error: Some(err),
                }),
            )
                .into_response(),
        };
    }

    (
        StatusCode::OK,
        axum::Json(AegisStatusResponse {
            enabled: true,
            session: state.aegis_session_snapshot.lock().unwrap().clone(),
            refreshed: false,
            error: None,
        }),
    )
        .into_response()
}

async fn post_aegis_start(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized").into_response();
    }

    let config = match Config::load() {
        Ok(cfg) => cfg,
        Err(err) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                axum::Json(AegisActionResponse {
                    ok: false,
                    message: "config load failed".to_string(),
                    session: state.aegis_session_snapshot.lock().unwrap().clone(),
                    error: Some(err.to_string()),
                }),
            )
                .into_response()
        }
    };

    let client = {
        let vault = state.vault.lock().unwrap();
        build_aegis_client_from_config(&config, &vault).map_err(|err| err.to_string())
    };

    let client = match client {
        Ok(client) => client,
        Err(err) => {
            return (
                StatusCode::BAD_REQUEST,
                axum::Json(AegisActionResponse {
                    ok: false,
                    message: "aegis client config invalid".to_string(),
                    session: state.aegis_session_snapshot.lock().unwrap().clone(),
                    error: Some(err),
                }),
            )
                .into_response()
        }
    };

    let request = RelayStartRequest {
        region_preference: Some("auto".to_string()),
        client_context: Some(RelayStartClientContext {
            obs_connected: None,
            mode: Some("studio".to_string()),
            requested_by: Some("dashboard".to_string()),
        }),
    };
    let idem = generate_idempotency_key();

    match client.relay_start(&idem, &request).await {
        Ok(session) => {
            *state.aegis_session_snapshot.lock().unwrap() = Some(session.clone());
            (
                StatusCode::OK,
                axum::Json(AegisActionResponse {
                    ok: true,
                    message: format!("relay start ok ({})", session.status),
                    session: Some(session),
                    error: None,
                }),
            )
                .into_response()
        }
        Err(err) => (
            StatusCode::BAD_GATEWAY,
            axum::Json(AegisActionResponse {
                ok: false,
                message: "relay start failed".to_string(),
                session: state.aegis_session_snapshot.lock().unwrap().clone(),
                error: Some(err.to_string()),
            }),
        )
            .into_response(),
    }
}

async fn post_aegis_stop(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized").into_response();
    }

    let config = match Config::load() {
        Ok(cfg) => cfg,
        Err(err) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                axum::Json(AegisActionResponse {
                    ok: false,
                    message: "config load failed".to_string(),
                    session: state.aegis_session_snapshot.lock().unwrap().clone(),
                    error: Some(err.to_string()),
                }),
            )
                .into_response()
        }
    };

    let client = {
        let vault = state.vault.lock().unwrap();
        build_aegis_client_from_config(&config, &vault).map_err(|err| err.to_string())
    };
    let client = match client {
        Ok(client) => client,
        Err(err) => {
            return (
                StatusCode::BAD_REQUEST,
                axum::Json(AegisActionResponse {
                    ok: false,
                    message: "aegis client config invalid".to_string(),
                    session: state.aegis_session_snapshot.lock().unwrap().clone(),
                    error: Some(err),
                }),
            )
                .into_response()
        }
    };

    let current = match client.relay_active().await {
        Ok(session) => session,
        Err(err) => {
            return (
                StatusCode::BAD_GATEWAY,
                axum::Json(AegisActionResponse {
                    ok: false,
                    message: "relay active lookup failed".to_string(),
                    session: state.aegis_session_snapshot.lock().unwrap().clone(),
                    error: Some(err.to_string()),
                }),
            )
                .into_response()
        }
    };

    let Some(session) = current else {
        *state.aegis_session_snapshot.lock().unwrap() = None;
        return (
            StatusCode::OK,
            axum::Json(AegisActionResponse {
                ok: true,
                message: "no active relay session".to_string(),
                session: None,
                error: None,
            }),
        )
            .into_response();
    };

    let stop_req = RelayStopRequest {
        session_id: session.session_id.clone(),
        reason: "user_requested".to_string(),
    };
    match client.relay_stop(&stop_req).await {
        Ok(_) => {
            *state.aegis_session_snapshot.lock().unwrap() = None;
            (
                StatusCode::OK,
                axum::Json(AegisActionResponse {
                    ok: true,
                    message: format!("relay stop ok ({})", stop_req.session_id),
                    session: None,
                    error: None,
                }),
            )
                .into_response()
        }
        Err(err) => (
            StatusCode::BAD_GATEWAY,
            axum::Json(AegisActionResponse {
                ok: false,
                message: "relay stop failed".to_string(),
                session: state.aegis_session_snapshot.lock().unwrap().clone(),
                error: Some(err.to_string()),
            }),
        )
            .into_response(),
    }
}

async fn post_ipc_switch_scene(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
    Json(body): Json<IpcSwitchSceneRequest>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized").into_response();
    }

    let scene_name = body.scene_name.trim();
    let allow_empty = body.allow_empty.unwrap_or(false);
    if scene_name.is_empty() && !allow_empty {
        return (
            StatusCode::BAD_REQUEST,
            axum::Json(IpcSwitchSceneResponse {
                ok: false,
                message: "scene_name is required (set allow_empty=true for debug negative-path validation)".to_string(),
            }),
        )
            .into_response();
    }

    let reason = body
        .reason
        .as_deref()
        .unwrap_or("manual_debug")
        .trim()
        .to_string();
    let deadline_ms = body.deadline_ms.unwrap_or(550).clamp(50, 5000);

    match state.ipc_cmd_tx.send(CoreIpcCommand::SwitchScene {
        scene_name: scene_name.to_string(),
        reason: if reason.is_empty() {
            "manual_debug".to_string()
        } else {
            reason
        },
        deadline_ms,
    }) {
        Ok(_receiver_count) => (
            StatusCode::OK,
            axum::Json(IpcSwitchSceneResponse {
                ok: true,
                message: format!(
                    "queued ipc switch_scene '{}' (deadline={}ms{})",
                    scene_name,
                    deadline_ms,
                    if scene_name.is_empty() { ", empty scene debug case" } else { "" }
                ),
            }),
        )
            .into_response(),
        Err(err) => (
            StatusCode::SERVICE_UNAVAILABLE,
            axum::Json(IpcSwitchSceneResponse {
                ok: false,
                message: format!("ipc switch_scene unavailable: {err}"),
            }),
        )
            .into_response(),
    }
}

fn build_aegis_client_from_config(
    config: &Config,
    vault: &Vault,
) -> Result<ControlPlaneClient, Box<dyn std::error::Error>> {
    let base_url = config
        .aegis
        .base_url
        .as_deref()
        .ok_or("missing aegis.base_url in config")?
        .trim();
    let jwt_key = config
        .aegis
        .access_jwt_key
        .as_deref()
        .ok_or("missing aegis.access_jwt_key in config")?
        .trim();
    if base_url.is_empty() {
        return Err("missing aegis.base_url in config".into());
    }
    if jwt_key.is_empty() {
        return Err("missing aegis.access_jwt_key in config".into());
    }
    let access_jwt = vault.retrieve(jwt_key)?;
    Ok(ControlPlaneClient::new(base_url, access_jwt.trim())?)
}

fn generate_idempotency_key() -> String {
    let ts = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis();
    let suffix: String = rand::thread_rng()
        .sample_iter(&Alphanumeric)
        .take(10)
        .map(char::from)
        .collect();
    format!("dash-{ts}-{suffix}")
}

#[cfg(test)]
mod tests {
    use super::{is_token_valid, QueryTokenPolicy};
    use axum::http::{HeaderMap, HeaderValue};
    use std::collections::HashMap;

    #[test]
    fn token_valid_accepts_bearer_header_when_query_denied() {
        let mut headers = HeaderMap::new();
        headers.insert(
            "authorization",
            HeaderValue::from_static("Bearer test-token"),
        );
        let query = HashMap::from([("token".to_string(), "wrong-token".to_string())]);

        let ok = is_token_valid(&headers, &query, "test-token", QueryTokenPolicy::Deny);
        assert!(ok);
    }

    #[test]
    fn token_valid_rejects_query_when_policy_denied() {
        let headers = HeaderMap::new();
        let query = HashMap::from([("token".to_string(), "test-token".to_string())]);

        let ok = is_token_valid(&headers, &query, "test-token", QueryTokenPolicy::Deny);
        assert!(!ok);
    }

    #[test]
    fn token_valid_accepts_query_when_policy_allowed() {
        let headers = HeaderMap::new();
        let query = HashMap::from([("token".to_string(), "test-token".to_string())]);

        let ok = is_token_valid(&headers, &query, "test-token", QueryTokenPolicy::Allow);
        assert!(ok);
    }
}
