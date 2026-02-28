use crate::aegis::RelaySession;
use crate::model::TelemetryFrame;
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, VecDeque};
use std::io;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::sync::{broadcast, watch};
use uuid::Uuid;

const IPC_PROTOCOL_VERSION: u8 = 1;
const MAX_FRAME_SIZE: usize = 64 * 1024;
pub const CMD_PIPE_NAME: &str = r"\\.\pipe\aegis_cmd_v1";
pub const EVT_PIPE_NAME: &str = r"\\.\pipe\aegis_evt_v1";
pub type IpcDebugStatusHandle = Arc<Mutex<IpcDebugStatus>>;

#[cfg(not(test))]
const READ_POLL_TIMEOUT: Duration = Duration::from_millis(250);
#[cfg(test)]
const READ_POLL_TIMEOUT: Duration = Duration::from_millis(25);

#[cfg(not(test))]
const STATUS_PUSH_INTERVAL: Duration = Duration::from_millis(1000);
#[cfg(test)]
const STATUS_PUSH_INTERVAL: Duration = Duration::from_millis(100);

#[cfg(not(test))]
const HEARTBEAT_TIMEOUT: Duration = Duration::from_millis(3500);
#[cfg(test)]
const HEARTBEAT_TIMEOUT: Duration = Duration::from_millis(350);

const PROTOCOL_ERROR_WINDOW: Duration = Duration::from_secs(10);
const PROTOCOL_ERROR_RESET_THRESHOLD: usize = 5;

pub type CoreIpcCommandSender = broadcast::Sender<CoreIpcCommand>;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct IpcSwitchRequestDebug {
    pub request_id: String,
    pub scene_name: String,
    pub reason: String,
    pub deadline_ms: u64,
    pub ts_unix_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct IpcSwitchResultDebug {
    pub request_id: String,
    pub status: String,
    pub error: Option<String>,
    pub ts_unix_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct IpcDebugStatus {
    pub session_connected: bool,
    pub pending_switch_count: u32,
    pub last_switch_request: Option<IpcSwitchRequestDebug>,
    pub last_switch_result: Option<IpcSwitchResultDebug>,
    pub last_notice: Option<String>,
    pub updated_ts_unix_ms: Option<u64>,
}

pub fn new_debug_status() -> IpcDebugStatusHandle {
    Arc::new(Mutex::new(IpcDebugStatus::default()))
}

pub fn spawn_server(
    rx: watch::Receiver<TelemetryFrame>,
    aegis_session_snapshot: Arc<Mutex<Option<RelaySession>>>,
    debug_status: IpcDebugStatusHandle,
) -> CoreIpcCommandSender {
    let (core_cmd_tx, _core_cmd_rx) = broadcast::channel(64);
    #[cfg(windows)]
    {
        let server_cmd_tx = core_cmd_tx.clone();
        let debug_status_clone = debug_status.clone();
        let _ipc_task = tokio::spawn(async move {
            if let Err(err) = run_named_pipe_server(
                rx,
                aegis_session_snapshot,
                server_cmd_tx,
                debug_status_clone,
            )
            .await
            {
                tracing::warn!(error = %err, "ipc server stopped");
            }
        });
    }

    #[cfg(not(windows))]
    {
        let _ = (rx, aegis_session_snapshot);
        if let Ok(mut s) = debug_status.lock() {
            s.session_connected = false;
            s.updated_ts_unix_ms = Some(now_unix_ms());
        }
        tracing::info!("ipc server stub disabled on non-Windows platform");
    }

    core_cmd_tx
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
enum Priority {
    Critical,
    High,
    Normal,
    Low,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct Envelope<T> {
    v: u8,
    id: String,
    ts_unix_ms: u64,
    #[serde(rename = "type")]
    message_type: String,
    priority: Priority,
    payload: T,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct HelloPayload {
    plugin_version: String,
    protocol_version: u8,
    obs_pid: u32,
    #[serde(default)]
    capabilities: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct PingPayload {
    nonce: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
struct RequestStatusPayload {}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct HelloAckPayload {
    core_version: String,
    protocol_version: u8,
    capabilities: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct PongPayload {
    nonce: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct SceneSwitchResultPayload {
    request_id: String,
    ok: bool,
    error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct ObsShutdownNoticePayload {
    reason: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct SwitchScenePayload {
    request_id: String,
    scene_name: String,
    reason: String,
    deadline_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct SetModeRequestPayload {
    mode: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct SetSettingRequestPayload {
    key: String,
    value: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
enum SnapshotMode {
    Studio,
    Irl,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
enum SnapshotHealth {
    Good,
    Degraded,
    Offline,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
enum RelayStatus {
    Inactive,
    Provisioning,
    Active,
    Grace,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
enum StateModeV1 {
    Studio,
    IrlConnecting,
    IrlActive,
    IrlGrace,
    Degraded,
    Fatal,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct RelaySnapshot {
    status: RelayStatus,
    region: Option<String>,
    grace_remaining_seconds: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct StatusSnapshotPayload {
    mode: SnapshotMode,
    state_mode: StateModeV1,
    health: SnapshotHealth,
    bitrate_kbps: u32,
    rtt_ms: u32,
    override_enabled: bool,
    relay: RelaySnapshot,
    #[serde(skip_serializing_if = "Option::is_none")]
    settings: Option<StatusSnapshotSettingsPayload>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct StatusSnapshotSettingsPayload {
    #[serde(skip_serializing_if = "Option::is_none")]
    auto_scene_switch: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    low_quality_fallback: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    manual_override: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    chat_bot: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    alerts: Option<bool>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
enum UserNoticeLevel {
    Info,
    Warn,
    Error,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct UserNoticePayload {
    level: UserNoticeLevel,
    message: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
enum ProtocolErrorCode {
    FrameTooLarge,
    DecodeFailed,
    UnknownType,
    Timeout,
    InvalidPayload,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct ProtocolErrorPayload {
    code: ProtocolErrorCode,
    message: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    related_message_id: Option<String>,
}

#[derive(Debug, Clone)]
pub enum CoreIpcCommand {
    SwitchScene {
        scene_name: String,
        reason: String,
        deadline_ms: u64,
    },
}

#[derive(Debug, Clone)]
struct PendingSwitchScene {
    scene_name: String,
    deadline_at: Instant,
}

fn now_unix_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

fn make_envelope<T: Serialize>(message_type: &str, priority: Priority, payload: T) -> Envelope<T> {
    Envelope {
        v: IPC_PROTOCOL_VERSION,
        id: Uuid::new_v4().to_string(),
        ts_unix_ms: now_unix_ms(),
        message_type: message_type.to_string(),
        priority,
        payload,
    }
}

fn make_protocol_error(
    code: ProtocolErrorCode,
    message: impl Into<String>,
    related_message_id: Option<String>,
) -> Envelope<ProtocolErrorPayload> {
    make_envelope(
        "protocol_error",
        Priority::High,
        ProtocolErrorPayload {
            code,
            message: message.into(),
            related_message_id,
        },
    )
}

fn update_debug_status<F>(debug_status: &IpcDebugStatusHandle, f: F)
where
    F: FnOnce(&mut IpcDebugStatus),
{
    let mut s = debug_status.lock().unwrap();
    f(&mut s);
    s.updated_ts_unix_ms = Some(now_unix_ms());
}

struct ProtocolErrorTracker {
    recent: VecDeque<Instant>,
}

impl ProtocolErrorTracker {
    fn new() -> Self {
        Self {
            recent: VecDeque::new(),
        }
    }

    fn record_and_should_reset(&mut self) -> bool {
        let now = Instant::now();
        self.recent.push_back(now);
        while let Some(front) = self.recent.front() {
            if now.duration_since(*front) > PROTOCOL_ERROR_WINDOW {
                self.recent.pop_front();
            } else {
                break;
            }
        }
        self.recent.len() > PROTOCOL_ERROR_RESET_THRESHOLD
    }
}

fn build_status_snapshot(
    frame: &TelemetryFrame,
    relay_session: Option<&RelaySession>,
) -> StatusSnapshotPayload {
    build_status_snapshot_with_overrides(frame, relay_session, &SessionOverrides::default())
}

#[derive(Debug, Clone, Default)]
struct SessionOverrides {
    mode: Option<SnapshotMode>,
    auto_scene_switch: Option<bool>,
    low_quality_fallback: Option<bool>,
    manual_override: Option<bool>,
    chat_bot: Option<bool>,
    alerts: Option<bool>,
}

impl SessionOverrides {
    fn set_mode_if_changed(&mut self, mode: SnapshotMode) -> bool {
        if self.mode.as_ref() == Some(&mode) {
            return false;
        }
        self.mode = Some(mode);
        true
    }

    fn apply_setting_if_changed(&mut self, key: &str, value: bool) -> Result<bool, ()> {
        let changed = match key {
            "auto_scene_switch" => {
                if self.auto_scene_switch == Some(value) {
                    false
                } else {
                    self.auto_scene_switch = Some(value);
                    true
                }
            }
            "low_quality_fallback" => {
                if self.low_quality_fallback == Some(value) {
                    false
                } else {
                    self.low_quality_fallback = Some(value);
                    true
                }
            }
            "manual_override" => {
                if self.manual_override == Some(value) {
                    false
                } else {
                    self.manual_override = Some(value);
                    true
                }
            }
            "chat_bot" => {
                if self.chat_bot == Some(value) {
                    false
                } else {
                    self.chat_bot = Some(value);
                    true
                }
            }
            "alerts" => {
                if self.alerts == Some(value) {
                    false
                } else {
                    self.alerts = Some(value);
                    true
                }
            }
            _ => return Err(()),
        };
        Ok(changed)
    }

    fn has_any_settings(&self) -> bool {
        self.auto_scene_switch.is_some()
            || self.low_quality_fallback.is_some()
            || self.manual_override.is_some()
            || self.chat_bot.is_some()
            || self.alerts.is_some()
    }
}

fn build_status_snapshot_with_overrides(
    frame: &TelemetryFrame,
    relay_session: Option<&RelaySession>,
    overrides: &SessionOverrides,
) -> StatusSnapshotPayload {
    let state_mode = derive_state_mode(frame, relay_session);
    let mut mode = match state_mode {
        StateModeV1::IrlConnecting | StateModeV1::IrlActive | StateModeV1::IrlGrace => {
            SnapshotMode::Irl
        }
        _ => SnapshotMode::Studio,
    };
    if let Some(mode_override) = &overrides.mode {
        mode = mode_override.clone();
    }
    let health = if !frame.obs.connected {
        SnapshotHealth::Offline
    } else if frame.health < 0.5 {
        SnapshotHealth::Degraded
    } else {
        SnapshotHealth::Good
    };

    let bitrate_kbps = frame
        .streams
        .iter()
        .map(|s| s.bitrate_kbps)
        .fold(0u32, |acc, v| acc.saturating_add(v));

    let relay = match relay_session {
        Some(session) => {
            let status = match session.status.as_str() {
                "provisioning" => RelayStatus::Provisioning,
                "active" => RelayStatus::Active,
                "grace" => RelayStatus::Grace,
                _ => RelayStatus::Inactive,
            };
            RelaySnapshot {
                status,
                region: session.region.clone(),
                grace_remaining_seconds: session
                    .timers
                    .as_ref()
                    .and_then(|t| t.grace_remaining_seconds)
                    .unwrap_or(0),
            }
        }
        None => RelaySnapshot {
            status: RelayStatus::Inactive,
            region: None,
            grace_remaining_seconds: 0,
        },
    };

    StatusSnapshotPayload {
        mode,
        state_mode,
        health,
        bitrate_kbps,
        rtt_ms: frame.network.latency_ms.max(0.0).round() as u32,
        override_enabled: overrides.manual_override.unwrap_or(false),
        relay,
        settings: overrides.has_any_settings().then_some(StatusSnapshotSettingsPayload {
            auto_scene_switch: overrides.auto_scene_switch,
            low_quality_fallback: overrides.low_quality_fallback,
            manual_override: overrides.manual_override,
            chat_bot: overrides.chat_bot,
            alerts: overrides.alerts,
        }),
    }
}

fn derive_state_mode(frame: &TelemetryFrame, relay_session: Option<&RelaySession>) -> StateModeV1 {
    if !frame.obs.connected {
        return StateModeV1::Degraded;
    }

    match relay_session.map(|s| s.status.as_str()) {
        Some("provisioning") => StateModeV1::IrlConnecting,
        Some("active") => StateModeV1::IrlActive,
        Some("grace") => StateModeV1::IrlGrace,
        _ => StateModeV1::Studio,
    }
}

async fn handle_session_io<R, W>(
    cmd_reader: &mut R,
    evt_writer: &mut W,
    rx: watch::Receiver<TelemetryFrame>,
    aegis_session_snapshot: Arc<Mutex<Option<RelaySession>>>,
    mut core_cmd_rx: broadcast::Receiver<CoreIpcCommand>,
    debug_status: IpcDebugStatusHandle,
) -> io::Result<()>
where
    R: AsyncRead + Unpin,
    W: AsyncWrite + Unpin,
{
    let mut protocol_errors = ProtocolErrorTracker::new();
    let mut pending_switches: HashMap<String, PendingSwitchScene> = HashMap::new();
    let mut session_overrides = SessionOverrides::default();
    let mut handshake_complete = false;
    let mut last_ping_at = Instant::now();
    let mut last_status_push_at = Instant::now();
    loop {
        while let Ok(cmd) = core_cmd_rx.try_recv() {
            if !handshake_complete {
                tracing::debug!("dropping core ipc command before handshake");
                continue;
            }
            match cmd {
                CoreIpcCommand::SwitchScene {
                    scene_name,
                    reason,
                    deadline_ms,
                } => {
                    let request_id = Uuid::new_v4().to_string();
                    let request_ts = now_unix_ms();
                    let evt = make_envelope(
                        "switch_scene",
                        Priority::Critical,
                        SwitchScenePayload {
                            request_id: request_id.clone(),
                            scene_name: scene_name.clone(),
                            reason,
                            deadline_ms,
                        },
                    );
                    write_frame(evt_writer, &evt).await?;
                    pending_switches.insert(
                        request_id,
                        PendingSwitchScene {
                            scene_name,
                            deadline_at: Instant::now() + Duration::from_millis(deadline_ms),
                        },
                    );
                    let payload = evt.payload.clone();
                    update_debug_status(&debug_status, |s| {
                        s.pending_switch_count = pending_switches.len() as u32;
                        s.last_switch_request = Some(IpcSwitchRequestDebug {
                            request_id: payload.request_id,
                            scene_name: payload.scene_name,
                            reason: payload.reason,
                            deadline_ms: payload.deadline_ms,
                            ts_unix_ms: request_ts,
                        });
                    });
                }
            }
        }

        if !pending_switches.is_empty() {
            let now = Instant::now();
            let expired_ids: Vec<String> = pending_switches
                .iter()
                .filter_map(|(id, pending)| (now >= pending.deadline_at).then_some(id.clone()))
                .collect();
            for id in expired_ids {
                if let Some(expired) = pending_switches.remove(&id) {
                    tracing::warn!(
                        request_id = %id,
                        scene_name = %expired.scene_name,
                        "ipc switch_scene request timed out"
                    );
                    let notice = make_envelope(
                        "user_notice",
                        Priority::High,
                        UserNoticePayload {
                            level: UserNoticeLevel::Warn,
                            message: format!(
                                "Scene switch to '{}' timed out (request {})",
                                expired.scene_name, id
                            ),
                        },
                    );
                    let _ = write_frame(evt_writer, &notice).await;
                    update_debug_status(&debug_status, |s| {
                        s.pending_switch_count = pending_switches.len() as u32;
                        s.last_switch_result = Some(IpcSwitchResultDebug {
                            request_id: id.clone(),
                            status: "timeout".to_string(),
                            error: None,
                            ts_unix_ms: now_unix_ms(),
                        });
                        s.last_notice = Some(format!(
                            "Scene switch '{}' timed out ({})",
                            expired.scene_name, id
                        ));
                    });
                }
            }
        }

        if handshake_complete && last_status_push_at.elapsed() >= STATUS_PUSH_INTERVAL {
            let frame = rx.borrow().clone();
            let relay = aegis_session_snapshot.lock().unwrap().clone();
            let payload = build_status_snapshot_with_overrides(&frame, relay.as_ref(), &session_overrides);
            let snapshot = make_envelope("status_snapshot", Priority::Normal, payload);
            write_frame(evt_writer, &snapshot).await?;
            last_status_push_at = Instant::now();
        }

        if handshake_complete && last_ping_at.elapsed() >= HEARTBEAT_TIMEOUT {
            let protocol_error = make_protocol_error(
                ProtocolErrorCode::Timeout,
                "Heartbeat timeout (missing ping)",
                None,
            );
            let _ = write_frame(evt_writer, &protocol_error).await;
            tracing::warn!("ipc session closed after heartbeat timeout");
            update_debug_status(&debug_status, |s| {
                s.last_notice = Some("Heartbeat timeout (missing ping)".to_string());
            });
            return Ok(());
        }

        let incoming: Envelope<serde_json::Value> =
            match tokio::time::timeout(READ_POLL_TIMEOUT, read_frame(cmd_reader)).await {
                Err(_) => continue,
                Ok(read_res) => match read_res {
                    Ok(frame) => frame,
                    Err(err) if err.kind() == io::ErrorKind::InvalidData => {
                        let msg = err.to_string();
                        let code = if msg.contains("frame too large") {
                            ProtocolErrorCode::FrameTooLarge
                        } else {
                            ProtocolErrorCode::DecodeFailed
                        };
                        let protocol_error = make_protocol_error(code, msg, None);
                        let _ = write_frame(evt_writer, &protocol_error).await;
                        update_debug_status(&debug_status, |s| {
                            s.last_notice = Some("IPC decode/frame protocol error".to_string());
                        });
                        if protocol_errors.record_and_should_reset() {
                            tracing::warn!("ipc session reset after repeated protocol errors");
                            return Ok(());
                        }
                        continue;
                    }
                    Err(err) => return Err(err),
                },
            };
        if incoming.v != IPC_PROTOCOL_VERSION {
            let notice = make_envelope(
                "user_notice",
                Priority::High,
                UserNoticePayload {
                    level: UserNoticeLevel::Error,
                    message: format!(
                        "IPC protocol version mismatch: plugin={}, core={}",
                        incoming.v, IPC_PROTOCOL_VERSION
                    ),
                },
            );
            write_frame(evt_writer, &notice).await?;
            update_debug_status(&debug_status, |s| {
                s.last_notice = Some("IPC envelope version mismatch".to_string());
            });
            return Ok(());
        }

        match incoming.message_type.as_str() {
            "hello" => {
                let hello: HelloPayload = match decode_payload(&incoming) {
                    Ok(v) => v,
                    Err(err) => {
                        emit_protocol_error_for_payload(evt_writer, &incoming, err).await?;
                        if protocol_errors.record_and_should_reset() {
                            tracing::warn!("ipc session reset after repeated protocol errors");
                            return Ok(());
                        }
                        continue;
                    }
                };
                if hello.protocol_version != IPC_PROTOCOL_VERSION {
                    let notice = make_envelope(
                        "user_notice",
                        Priority::High,
                        UserNoticePayload {
                            level: UserNoticeLevel::Error,
                            message: format!(
                                "Protocol mismatch (plugin {}, core {})",
                                hello.protocol_version, IPC_PROTOCOL_VERSION
                            ),
                        },
                    );
                    write_frame(evt_writer, &notice).await?;
                    update_debug_status(&debug_status, |s| {
                        s.last_notice = Some("IPC protocol mismatch".to_string());
                    });
                    return Ok(());
                }

                let ack = make_envelope(
                    "hello_ack",
                    Priority::High,
                    HelloAckPayload {
                        core_version: env!("CARGO_PKG_VERSION").to_string(),
                        protocol_version: IPC_PROTOCOL_VERSION,
                        capabilities: vec![
                            "state_machine".to_string(),
                            "aegis".to_string(),
                            "ipc_stub".to_string(),
                        ],
                    },
                );
                write_frame(evt_writer, &ack).await?;
                handshake_complete = true;
                last_ping_at = Instant::now();
                last_status_push_at = Instant::now() - STATUS_PUSH_INTERVAL;
            }
            "ping" => {
                let ping: PingPayload = match decode_payload(&incoming) {
                    Ok(v) => v,
                    Err(err) => {
                        emit_protocol_error_for_payload(evt_writer, &incoming, err).await?;
                        if protocol_errors.record_and_should_reset() {
                            tracing::warn!("ipc session reset after repeated protocol errors");
                            return Ok(());
                        }
                        continue;
                    }
                };
                let pong =
                    make_envelope("pong", Priority::Normal, PongPayload { nonce: ping.nonce });
                write_frame(evt_writer, &pong).await?;
                last_ping_at = Instant::now();
            }
            "request_status" => {
                let _: RequestStatusPayload = match decode_payload(&incoming) {
                    Ok(v) => v,
                    Err(err) => {
                        emit_protocol_error_for_payload(evt_writer, &incoming, err).await?;
                        if protocol_errors.record_and_should_reset() {
                            tracing::warn!("ipc session reset after repeated protocol errors");
                            return Ok(());
                        }
                        continue;
                    }
                };
                let frame = rx.borrow().clone();
                let relay = aegis_session_snapshot.lock().unwrap().clone();
                let payload =
                    build_status_snapshot_with_overrides(&frame, relay.as_ref(), &session_overrides);
                let snapshot = make_envelope("status_snapshot", Priority::High, payload);
                write_frame(evt_writer, &snapshot).await?;
                last_status_push_at = Instant::now();
            }
            "set_mode_request" => {
                let req: SetModeRequestPayload = match decode_payload(&incoming) {
                    Ok(v) => v,
                    Err(err) => {
                        emit_protocol_error_for_payload(evt_writer, &incoming, err).await?;
                        if protocol_errors.record_and_should_reset() {
                            tracing::warn!("ipc session reset after repeated protocol errors");
                            return Ok(());
                        }
                        continue;
                    }
                };
                let normalized = match req.mode.as_str() {
                    "studio" => SnapshotMode::Studio,
                    "irl" => SnapshotMode::Irl,
                    _ => {
                        let protocol_error = make_protocol_error(
                            ProtocolErrorCode::InvalidPayload,
                            format!("Invalid mode for set_mode_request: {}", req.mode),
                            Some(incoming.id.clone()),
                        );
                        write_frame(evt_writer, &protocol_error).await?;
                        continue;
                    }
                };
                if !session_overrides.set_mode_if_changed(normalized) {
                    tracing::debug!(mode = %req.mode, "ipc set_mode_request no-op (unchanged override)");
                    continue;
                }
                let notice = make_envelope(
                    "user_notice",
                    Priority::Normal,
                    UserNoticePayload {
                        level: UserNoticeLevel::Info,
                        message: format!("Dock mode override set to {}", req.mode),
                    },
                );
                let _ = write_frame(evt_writer, &notice).await;
                let frame = rx.borrow().clone();
                let relay = aegis_session_snapshot.lock().unwrap().clone();
                let payload =
                    build_status_snapshot_with_overrides(&frame, relay.as_ref(), &session_overrides);
                let snapshot = make_envelope("status_snapshot", Priority::High, payload);
                write_frame(evt_writer, &snapshot).await?;
                last_status_push_at = Instant::now();
            }
            "set_setting_request" => {
                let req: SetSettingRequestPayload = match decode_payload(&incoming) {
                    Ok(v) => v,
                    Err(err) => {
                        emit_protocol_error_for_payload(evt_writer, &incoming, err).await?;
                        if protocol_errors.record_and_should_reset() {
                            tracing::warn!("ipc session reset after repeated protocol errors");
                            return Ok(());
                        }
                        continue;
                    }
                };
                let changed = match session_overrides.apply_setting_if_changed(&req.key, req.value) {
                    Ok(changed) => changed,
                    Err(()) => {
                        let protocol_error = make_protocol_error(
                            ProtocolErrorCode::InvalidPayload,
                            format!("Unsupported setting key for set_setting_request: {}", req.key),
                            Some(incoming.id.clone()),
                        );
                        write_frame(evt_writer, &protocol_error).await?;
                        continue;
                    }
                };
                if !changed {
                    tracing::debug!(
                        key = %req.key,
                        value = req.value,
                        "ipc set_setting_request no-op (unchanged override)"
                    );
                    continue;
                }
                let notice = make_envelope(
                    "user_notice",
                    Priority::Normal,
                    UserNoticePayload {
                        level: UserNoticeLevel::Info,
                        message: format!("Dock setting '{}' set to {}", req.key, req.value),
                    },
                );
                let _ = write_frame(evt_writer, &notice).await;
                let frame = rx.borrow().clone();
                let relay = aegis_session_snapshot.lock().unwrap().clone();
                let payload =
                    build_status_snapshot_with_overrides(&frame, relay.as_ref(), &session_overrides);
                let snapshot = make_envelope("status_snapshot", Priority::High, payload);
                write_frame(evt_writer, &snapshot).await?;
                last_status_push_at = Instant::now();
            }
            "scene_switch_result" => {
                let result: SceneSwitchResultPayload = match decode_payload(&incoming) {
                    Ok(v) => v,
                    Err(err) => {
                        emit_protocol_error_for_payload(evt_writer, &incoming, err).await?;
                        if protocol_errors.record_and_should_reset() {
                            tracing::warn!("ipc session reset after repeated protocol errors");
                            return Ok(());
                        }
                        continue;
                    }
                };
                tracing::info!(
                    request_id = %result.request_id,
                    ok = result.ok,
                    error = ?result.error,
                    "ipc scene_switch_result received"
                );
                if pending_switches.remove(&result.request_id).is_none() {
                    tracing::warn!(
                        request_id = %result.request_id,
                        "ipc scene_switch_result received for unknown request"
                    );
                    update_debug_status(&debug_status, |s| {
                        s.last_switch_result = Some(IpcSwitchResultDebug {
                            request_id: result.request_id.clone(),
                            status: "unknown_request".to_string(),
                            error: result.error.clone(),
                            ts_unix_ms: now_unix_ms(),
                        });
                        s.last_notice = Some("scene_switch_result for unknown request".to_string());
                    });
                } else {
                    update_debug_status(&debug_status, |s| {
                        s.pending_switch_count = pending_switches.len() as u32;
                        s.last_switch_result = Some(IpcSwitchResultDebug {
                            request_id: result.request_id.clone(),
                            status: if result.ok { "ok" } else { "error" }.to_string(),
                            error: result.error.clone(),
                            ts_unix_ms: now_unix_ms(),
                        });
                    });
                }
            }
            "obs_shutdown_notice" => {
                let notice: ObsShutdownNoticePayload = match decode_payload(&incoming) {
                    Ok(v) => v,
                    Err(err) => {
                        emit_protocol_error_for_payload(evt_writer, &incoming, err).await?;
                        if protocol_errors.record_and_should_reset() {
                            tracing::warn!("ipc session reset after repeated protocol errors");
                            return Ok(());
                        }
                        continue;
                    }
                };
                tracing::info!(reason = %notice.reason, "ipc obs shutdown notice received");
                update_debug_status(&debug_status, |s| {
                    s.last_notice = Some(format!("obs shutdown notice: {}", notice.reason));
                });
                return Ok(());
            }
            other => {
                let protocol_error = make_protocol_error(
                    ProtocolErrorCode::UnknownType,
                    format!("Unsupported IPC command in core stub: {other}"),
                    Some(incoming.id.clone()),
                );
                write_frame(evt_writer, &protocol_error).await?;
                update_debug_status(&debug_status, |s| {
                    s.last_notice = Some(format!("Unsupported IPC command: {other}"));
                });
                if protocol_errors.record_and_should_reset() {
                    tracing::warn!("ipc session reset after repeated protocol errors");
                    return Ok(());
                }
            }
        }
    }
}

async fn emit_protocol_error_for_payload<W>(
    evt_writer: &mut W,
    incoming: &Envelope<serde_json::Value>,
    err: io::Error,
) -> io::Result<()>
where
    W: AsyncWrite + Unpin,
{
    let protocol_error = make_protocol_error(
        ProtocolErrorCode::InvalidPayload,
        err.to_string(),
        Some(incoming.id.clone()),
    );
    write_frame(evt_writer, &protocol_error).await
}

fn decode_payload<T: for<'de> Deserialize<'de>>(
    envelope: &Envelope<serde_json::Value>,
) -> io::Result<T> {
    serde_json::from_value(envelope.payload.clone()).map_err(|err| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("payload decode failed: {err}"),
        )
    })
}

async fn read_frame<R>(reader: &mut R) -> io::Result<Envelope<serde_json::Value>>
where
    R: AsyncRead + Unpin,
{
    let len = reader.read_u32_le().await? as usize;
    if len > MAX_FRAME_SIZE {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("frame too large: {len}"),
        ));
    }
    let mut buf = vec![0u8; len];
    reader.read_exact(&mut buf).await?;
    rmp_serde::from_slice(&buf)
        .map_err(|err| io::Error::new(io::ErrorKind::InvalidData, format!("decode failed: {err}")))
}

async fn write_frame<W, T>(writer: &mut W, message: &Envelope<T>) -> io::Result<()>
where
    W: AsyncWrite + Unpin,
    T: Serialize,
{
    let payload = rmp_serde::to_vec_named(message).map_err(|err| {
        io::Error::new(io::ErrorKind::InvalidData, format!("encode failed: {err}"))
    })?;
    if payload.len() > MAX_FRAME_SIZE {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("encoded frame too large: {}", payload.len()),
        ));
    }
    writer.write_u32_le(payload.len() as u32).await?;
    writer.write_all(&payload).await?;
    writer.flush().await
}

#[cfg(windows)]
mod windows_impl {
    use super::*;
    use tokio::net::windows::named_pipe::{NamedPipeServer, ServerOptions};
    use windows::Win32::Foundation::BOOL;
    use windows::Win32::Security::{
        InitializeSecurityDescriptor, PSECURITY_DESCRIPTOR, SetSecurityDescriptorDacl,
        SECURITY_ATTRIBUTES, SECURITY_DESCRIPTOR,
    };

    pub async fn run_named_pipe_server(
        rx: watch::Receiver<TelemetryFrame>,
        aegis_session_snapshot: Arc<Mutex<Option<RelaySession>>>,
        core_cmd_tx: broadcast::Sender<CoreIpcCommand>,
        debug_status: IpcDebugStatusHandle,
    ) -> io::Result<()> {
        tracing::info!(
            cmd_pipe = CMD_PIPE_NAME,
            evt_pipe = EVT_PIPE_NAME,
            "ipc named-pipe server stub listening"
        );
        loop {
            let (cmd_pipe, evt_pipe) = {
                let mut sd = make_permissive_pipe_security_descriptor()?;
                let mut sa = SECURITY_ATTRIBUTES {
                    nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
                    lpSecurityDescriptor: &mut sd as *mut _ as *mut _,
                    bInheritHandle: BOOL(0),
                };

                let cmd_pipe = {
                    let mut opts = ServerOptions::new();
                    opts.access_outbound(false);
                    unsafe {
                        opts.create_with_security_attributes_raw(
                            CMD_PIPE_NAME,
                            &mut sa as *mut _ as *mut _,
                        )
                    }?
                };
                let evt_pipe = {
                    let mut opts = ServerOptions::new();
                    opts.access_inbound(false);
                    unsafe {
                        opts.create_with_security_attributes_raw(
                            EVT_PIPE_NAME,
                            &mut sa as *mut _ as *mut _,
                        )
                    }?
                };
                (cmd_pipe, evt_pipe)
            };

            let (mut cmd_pipe, mut evt_pipe) =
                tokio::try_join!(connect_pipe(cmd_pipe), connect_pipe(evt_pipe))?;
            let session_cmd_rx = core_cmd_tx.subscribe();
            update_debug_status(&debug_status, |s| {
                s.session_connected = true;
            });

            tracing::info!("ipc client connected");
            let session_result = handle_session_io(
                &mut cmd_pipe,
                &mut evt_pipe,
                rx.clone(),
                aegis_session_snapshot.clone(),
                session_cmd_rx,
                debug_status.clone(),
            )
            .await;
            update_debug_status(&debug_status, |s| {
                s.session_connected = false;
                s.pending_switch_count = 0;
            });
            match session_result {
                Ok(()) => tracing::info!("ipc client disconnected"),
                Err(err) if err.kind() == io::ErrorKind::UnexpectedEof => {
                    tracing::info!("ipc client disconnected")
                }
                Err(err) => tracing::warn!(error = %err, "ipc session error"),
            }
        }
    }

    async fn connect_pipe(server: NamedPipeServer) -> io::Result<NamedPipeServer> {
        server.connect().await?;
        Ok(server)
    }

    fn make_permissive_pipe_security_descriptor() -> io::Result<SECURITY_DESCRIPTOR> {
        let mut sd = SECURITY_DESCRIPTOR::default();
        unsafe {
            InitializeSecurityDescriptor(
                PSECURITY_DESCRIPTOR(&mut sd as *mut _ as *mut _),
                1,
            )
            .map_err(|err| io::Error::new(io::ErrorKind::Other, format!("InitializeSecurityDescriptor failed: {err}")))?;

            SetSecurityDescriptorDacl(
                PSECURITY_DESCRIPTOR(&mut sd as *mut _ as *mut _),
                BOOL(1),
                None,
                BOOL(0),
            )
            .map_err(|err| io::Error::new(io::ErrorKind::Other, format!("SetSecurityDescriptorDacl failed: {err}")))?;
        }
        Ok(sd)
    }
}

#[cfg(windows)]
use windows_impl::run_named_pipe_server;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::aegis::{RelaySession, RelayTimers};
    use tokio::io::{split, DuplexStream};

    #[test]
    fn derives_irl_grace_snapshot_and_state_mode() {
        let frame = TelemetryFrame {
            health: 0.9,
            obs: crate::model::ObsFrame {
                connected: true,
                ..Default::default()
            },
            network: crate::model::NetworkFrame {
                latency_ms: 72.4,
                ..Default::default()
            },
            streams: vec![crate::model::StreamOutput {
                bitrate_kbps: 4500,
                ..Default::default()
            }],
            ..Default::default()
        };

        let session = RelaySession {
            status: "grace".to_string(),
            region: Some("us-west-2".to_string()),
            timers: Some(RelayTimers {
                grace_remaining_seconds: Some(321),
                ..Default::default()
            }),
            ..Default::default()
        };

        let snapshot = build_status_snapshot(&frame, Some(&session));
        assert!(matches!(snapshot.mode, SnapshotMode::Irl));
        assert!(matches!(snapshot.state_mode, StateModeV1::IrlGrace));
        assert!(matches!(snapshot.relay.status, RelayStatus::Grace));
        assert_eq!(snapshot.bitrate_kbps, 4500);
        assert_eq!(snapshot.rtt_ms, 72);
        assert_eq!(snapshot.relay.grace_remaining_seconds, 321);
    }

    #[test]
    fn msgpack_envelope_roundtrips() {
        let env = make_envelope(
            "user_notice",
            Priority::Normal,
            UserNoticePayload {
                level: UserNoticeLevel::Info,
                message: "stub ok".to_string(),
            },
        );
        let bytes = rmp_serde::to_vec_named(&env).unwrap();
        let decoded: Envelope<UserNoticePayload> = rmp_serde::from_slice(&bytes).unwrap();
        assert_eq!(decoded.v, IPC_PROTOCOL_VERSION);
        assert_eq!(decoded.message_type, "user_notice");
        assert!(matches!(decoded.priority, Priority::Normal));
        assert_eq!(decoded.payload.message, "stub ok");
    }

    #[test]
    fn protocol_error_envelope_uses_spec_codes() {
        let env = make_protocol_error(
            ProtocolErrorCode::UnknownType,
            "unsupported",
            Some("msg-123".to_string()),
        );
        let bytes = rmp_serde::to_vec_named(&env).unwrap();
        let decoded: Envelope<ProtocolErrorPayload> = rmp_serde::from_slice(&bytes).unwrap();
        assert_eq!(decoded.message_type, "protocol_error");
        assert!(matches!(
            decoded.payload.code,
            ProtocolErrorCode::UnknownType
        ));
        assert_eq!(
            decoded.payload.related_message_id.as_deref(),
            Some("msg-123")
        );
    }

    fn hello_envelope() -> Envelope<HelloPayload> {
        make_envelope(
            "hello",
            Priority::High,
            HelloPayload {
                plugin_version: "0.0.3".to_string(),
                protocol_version: IPC_PROTOCOL_VERSION,
                obs_pid: 1234,
                capabilities: vec!["dock".to_string()],
            },
        )
    }

    fn ping_envelope(nonce: &str) -> Envelope<PingPayload> {
        make_envelope(
            "ping",
            Priority::Normal,
            PingPayload {
                nonce: nonce.to_string(),
            },
        )
    }

    fn request_status_envelope() -> Envelope<RequestStatusPayload> {
        make_envelope("request_status", Priority::High, RequestStatusPayload {})
    }

    fn set_mode_request_envelope(mode: &str) -> Envelope<SetModeRequestPayload> {
        make_envelope(
            "set_mode_request",
            Priority::High,
            SetModeRequestPayload {
                mode: mode.to_string(),
            },
        )
    }

    fn set_setting_request_envelope(key: &str, value: bool) -> Envelope<SetSettingRequestPayload> {
        make_envelope(
            "set_setting_request",
            Priority::High,
            SetSettingRequestPayload {
                key: key.to_string(),
                value,
            },
        )
    }

    async fn read_event(client: &mut DuplexStream) -> Envelope<serde_json::Value> {
        read_frame(client).await.unwrap()
    }

    async fn drain_until_message_type(
        client: &mut DuplexStream,
        message_type: &str,
        timeout: Duration,
    ) -> Envelope<serde_json::Value> {
        let deadline = tokio::time::Instant::now() + timeout;
        while tokio::time::Instant::now() < deadline {
            let msg = tokio::time::timeout(Duration::from_millis(250), read_event(client))
                .await
                .unwrap();
            if msg.message_type == message_type {
                return msg;
            }
        }
        panic!("expected message_type={message_type}");
    }

    async fn spawn_test_session() -> (
        DuplexStream,
        tokio::task::JoinHandle<io::Result<()>>,
        watch::Sender<TelemetryFrame>,
        broadcast::Sender<CoreIpcCommand>,
    ) {
        let (server_side, client_side) = tokio::io::duplex(64 * 1024);
        let (mut server_reader, mut server_writer) = split(server_side);
        let (tx, rx) = watch::channel(TelemetryFrame::default());
        let (cmd_tx, cmd_rx) = broadcast::channel(64);
        let snapshot = Arc::new(Mutex::new(None::<RelaySession>));
        let debug_status = new_debug_status();
        let task = tokio::spawn(async move {
            handle_session_io(
                &mut server_reader,
                &mut server_writer,
                rx,
                snapshot,
                cmd_rx,
                debug_status,
            )
            .await
        });
        (client_side, task, tx, cmd_tx)
    }

    #[tokio::test]
    async fn session_sends_hello_ack_and_periodic_status_snapshot() {
        let (mut client, task, _tx, _cmd_tx) = spawn_test_session().await;

        write_frame(&mut client, &hello_envelope()).await.unwrap();

        let ack = read_event(&mut client).await;
        assert_eq!(ack.message_type, "hello_ack");

        let next = tokio::time::timeout(Duration::from_secs(1), read_event(&mut client))
            .await
            .unwrap();
        assert_eq!(next.message_type, "status_snapshot");

        drop(client);
        let _ = task.await;
    }

    #[tokio::test]
    async fn session_emits_timeout_protocol_error_when_heartbeat_missing() {
        let (mut client, task, _tx, _cmd_tx) = spawn_test_session().await;

        write_frame(&mut client, &hello_envelope()).await.unwrap();

        // Drain hello ack and any status snapshots until timeout protocol_error arrives.
        let mut saw_timeout = false;
        let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
        while tokio::time::Instant::now() < deadline {
            let msg = tokio::time::timeout(Duration::from_millis(500), read_event(&mut client))
                .await
                .unwrap();
            if msg.message_type == "protocol_error" {
                let payload: ProtocolErrorPayload = serde_json::from_value(msg.payload).unwrap();
                if matches!(payload.code, ProtocolErrorCode::Timeout) {
                    saw_timeout = true;
                    break;
                }
            }
        }

        assert!(saw_timeout, "expected timeout protocol_error");
        let session_result = task.await.unwrap();
        assert!(session_result.is_ok());
    }

    #[tokio::test]
    async fn session_replies_to_ping_with_matching_pong() {
        let (mut client, task, _tx, _cmd_tx) = spawn_test_session().await;

        write_frame(&mut client, &hello_envelope()).await.unwrap();
        let ack = read_event(&mut client).await;
        assert_eq!(ack.message_type, "hello_ack");

        write_frame(&mut client, &ping_envelope("nonce-abc"))
            .await
            .unwrap();

        let deadline = tokio::time::Instant::now() + Duration::from_secs(1);
        let mut saw_pong = false;
        while tokio::time::Instant::now() < deadline {
            let msg = tokio::time::timeout(Duration::from_millis(250), read_event(&mut client))
                .await
                .unwrap();
            if msg.message_type == "pong" {
                let payload: PongPayload = serde_json::from_value(msg.payload).unwrap();
                assert_eq!(payload.nonce, "nonce-abc");
                saw_pong = true;
                break;
            }
        }

        assert!(saw_pong, "expected pong response");
        drop(client);
        let _ = task.await;
    }

    #[tokio::test]
    async fn session_returns_status_snapshot_on_request_status() {
        let (mut client, task, tx, _cmd_tx) = spawn_test_session().await;

        let _ = tx.send(TelemetryFrame {
            health: 0.8,
            obs: crate::model::ObsFrame {
                connected: true,
                ..Default::default()
            },
            network: crate::model::NetworkFrame {
                latency_ms: 42.0,
                ..Default::default()
            },
            streams: vec![crate::model::StreamOutput {
                bitrate_kbps: 2222,
                ..Default::default()
            }],
            ..Default::default()
        });

        write_frame(&mut client, &hello_envelope()).await.unwrap();
        let ack = read_event(&mut client).await;
        assert_eq!(ack.message_type, "hello_ack");

        write_frame(&mut client, &request_status_envelope())
            .await
            .unwrap();

        let deadline = tokio::time::Instant::now() + Duration::from_secs(1);
        let mut saw_snapshot = false;
        while tokio::time::Instant::now() < deadline {
            let msg = tokio::time::timeout(Duration::from_millis(250), read_event(&mut client))
                .await
                .unwrap();
            if msg.message_type == "status_snapshot" {
                let payload: StatusSnapshotPayload = serde_json::from_value(msg.payload).unwrap();
                assert_eq!(payload.bitrate_kbps, 2222);
                assert_eq!(payload.rtt_ms, 42);
                saw_snapshot = true;
                break;
            }
        }

        assert!(saw_snapshot, "expected status_snapshot response");
        drop(client);
        let _ = task.await;
    }

    #[tokio::test]
    async fn malformed_payload_emits_invalid_payload_protocol_error() {
        let (mut client, task, _tx, _cmd_tx) = spawn_test_session().await;

        write_frame(&mut client, &hello_envelope()).await.unwrap();
        let ack = read_event(&mut client).await;
        assert_eq!(ack.message_type, "hello_ack");

        let bad_ping = make_envelope(
            "ping",
            Priority::Normal,
            serde_json::json!({ "nonce": 123 }),
        );
        write_frame(&mut client, &bad_ping).await.unwrap();

        let deadline = tokio::time::Instant::now() + Duration::from_secs(1);
        let mut saw_invalid_payload = false;
        while tokio::time::Instant::now() < deadline {
            let msg = tokio::time::timeout(Duration::from_millis(250), read_event(&mut client))
                .await
                .unwrap();
            if msg.message_type == "protocol_error" {
                let payload: ProtocolErrorPayload = serde_json::from_value(msg.payload).unwrap();
                if matches!(payload.code, ProtocolErrorCode::InvalidPayload) {
                    saw_invalid_payload = true;
                    break;
                }
            }
        }

        assert!(
            saw_invalid_payload,
            "expected protocol_error with invalid_payload"
        );
        drop(client);
        let _ = task.await;
    }

    #[tokio::test]
    async fn unknown_message_type_emits_unknown_type_protocol_error() {
        let (mut client, task, _tx, _cmd_tx) = spawn_test_session().await;

        write_frame(&mut client, &hello_envelope()).await.unwrap();
        let ack = read_event(&mut client).await;
        assert_eq!(ack.message_type, "hello_ack");

        let unknown = make_envelope(
            "totally_unknown_cmd",
            Priority::Normal,
            serde_json::json!({}),
        );
        write_frame(&mut client, &unknown).await.unwrap();

        let deadline = tokio::time::Instant::now() + Duration::from_secs(1);
        let mut saw_unknown_type = false;
        while tokio::time::Instant::now() < deadline {
            let msg = tokio::time::timeout(Duration::from_millis(250), read_event(&mut client))
                .await
                .unwrap();
            if msg.message_type == "protocol_error" {
                let payload: ProtocolErrorPayload = serde_json::from_value(msg.payload).unwrap();
                if matches!(payload.code, ProtocolErrorCode::UnknownType) {
                    saw_unknown_type = true;
                    break;
                }
            }
        }

        assert!(
            saw_unknown_type,
            "expected protocol_error with unknown_type"
        );
        drop(client);
        let _ = task.await;
    }

    #[tokio::test]
    async fn repeated_protocol_errors_trigger_controlled_session_reset() {
        let (mut client, task, _tx, _cmd_tx) = spawn_test_session().await;

        write_frame(&mut client, &hello_envelope()).await.unwrap();
        let ack = read_event(&mut client).await;
        assert_eq!(ack.message_type, "hello_ack");

        for _ in 0..6 {
            let unknown = make_envelope("bad_cmd", Priority::Normal, serde_json::json!({}));
            write_frame(&mut client, &unknown).await.unwrap();
        }

        let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
        let mut unknown_error_count = 0usize;
        while tokio::time::Instant::now() < deadline {
            match tokio::time::timeout(Duration::from_millis(200), read_frame(&mut client)).await {
                Ok(Ok(msg)) => {
                    if msg.message_type == "protocol_error" {
                        let payload: ProtocolErrorPayload =
                            serde_json::from_value(msg.payload).unwrap();
                        if matches!(payload.code, ProtocolErrorCode::UnknownType) {
                            unknown_error_count += 1;
                        }
                    }
                }
                Ok(Err(err)) if err.kind() == io::ErrorKind::UnexpectedEof => break,
                Ok(Err(err)) => panic!("unexpected read error: {err}"),
                Err(_) => {
                    // Session likely closed; stop waiting.
                    break;
                }
            }
        }

        assert!(
            unknown_error_count >= 5,
            "expected multiple unknown_type protocol errors before reset"
        );

        let session_result = tokio::time::timeout(Duration::from_secs(1), task)
            .await
            .expect("session task should finish after reset")
            .unwrap();
        assert!(session_result.is_ok());
    }

    #[tokio::test]
    async fn core_switch_scene_command_emits_event_and_ack_clears_timeout() {
        let (mut client, task, _tx, cmd_tx) = spawn_test_session().await;

        write_frame(&mut client, &hello_envelope()).await.unwrap();
        let ack = read_event(&mut client).await;
        assert_eq!(ack.message_type, "hello_ack");

        cmd_tx
            .send(CoreIpcCommand::SwitchScene {
                scene_name: "BRB".to_string(),
                reason: "auto_failover".to_string(),
                deadline_ms: 200,
            })
            .unwrap();

        let deadline = tokio::time::Instant::now() + Duration::from_secs(1);
        let mut switch_request_id = None::<String>;
        while tokio::time::Instant::now() < deadline {
            let msg = tokio::time::timeout(Duration::from_millis(250), read_event(&mut client))
                .await
                .unwrap();
            if msg.message_type == "switch_scene" {
                let payload: SwitchScenePayload = serde_json::from_value(msg.payload).unwrap();
                assert_eq!(payload.scene_name, "BRB");
                assert_eq!(payload.reason, "auto_failover");
                switch_request_id = Some(payload.request_id);
                break;
            }
        }
        let request_id = switch_request_id.expect("expected switch_scene event");

        let ack_env = make_envelope(
            "scene_switch_result",
            Priority::High,
            SceneSwitchResultPayload {
                request_id,
                ok: true,
                error: None,
            },
        );
        write_frame(&mut client, &ack_env).await.unwrap();

        // Keep heartbeat alive and ensure timeout notice is not emitted for this request.
        write_frame(&mut client, &ping_envelope("keepalive"))
            .await
            .unwrap();
        let until = tokio::time::Instant::now() + Duration::from_millis(400);
        let mut saw_timeout_notice = false;
        while tokio::time::Instant::now() < until {
            if let Ok(msg) =
                tokio::time::timeout(Duration::from_millis(100), read_event(&mut client)).await
            {
                if msg.message_type == "user_notice" {
                    let payload: UserNoticePayload = serde_json::from_value(msg.payload).unwrap();
                    if payload.message.contains("timed out") {
                        saw_timeout_notice = true;
                        break;
                    }
                }
            } else {
                // no message in this slice; send another ping to avoid heartbeat timeout
                let _ = write_frame(&mut client, &ping_envelope("keepalive-2")).await;
            }
        }
        assert!(
            !saw_timeout_notice,
            "switch_scene ack should clear pending timeout"
        );

        drop(client);
        let _ = task.await;
    }

    #[tokio::test]
    async fn core_switch_scene_command_timeout_emits_user_notice() {
        let (mut client, task, _tx, cmd_tx) = spawn_test_session().await;

        write_frame(&mut client, &hello_envelope()).await.unwrap();
        let ack = read_event(&mut client).await;
        assert_eq!(ack.message_type, "hello_ack");

        cmd_tx
            .send(CoreIpcCommand::SwitchScene {
                scene_name: "BRB".to_string(),
                reason: "auto_failover".to_string(),
                deadline_ms: 80,
            })
            .unwrap();

        let deadline = tokio::time::Instant::now() + Duration::from_secs(1);
        let mut saw_switch_scene = false;
        let mut saw_timeout_notice = false;
        while tokio::time::Instant::now() < deadline {
            // keep heartbeat alive while waiting
            let _ = write_frame(&mut client, &ping_envelope("keepalive")).await;
            let msg =
                tokio::time::timeout(Duration::from_millis(150), read_event(&mut client)).await;
            match msg {
                Ok(msg) => {
                    if msg.message_type == "switch_scene" {
                        saw_switch_scene = true;
                    }
                    if msg.message_type == "user_notice" {
                        let payload: UserNoticePayload =
                            serde_json::from_value(msg.payload).unwrap();
                        if payload.message.contains("timed out") {
                            saw_timeout_notice = true;
                            break;
                        }
                    }
                }
                Err(_) => {}
            }
        }

        assert!(saw_switch_scene, "expected switch_scene event");
        assert!(saw_timeout_notice, "expected timeout user_notice");

        drop(client);
        let _ = task.await;
    }

    #[tokio::test]
    async fn repeated_identical_set_mode_request_is_noop() {
        let (mut client, task, _tx, _cmd_tx) = spawn_test_session().await;

        write_frame(&mut client, &hello_envelope()).await.unwrap();
        let ack = read_event(&mut client).await;
        assert_eq!(ack.message_type, "hello_ack");

        let _ = drain_until_message_type(&mut client, "status_snapshot", Duration::from_secs(1)).await;

        write_frame(&mut client, &set_mode_request_envelope("irl"))
            .await
            .unwrap();
        let notice1 = drain_until_message_type(&mut client, "user_notice", Duration::from_secs(1)).await;
        let payload1: UserNoticePayload = serde_json::from_value(notice1.payload).unwrap();
        assert!(payload1.message.contains("irl"));
        let snapshot1 = drain_until_message_type(&mut client, "status_snapshot", Duration::from_secs(1)).await;
        let snap1: StatusSnapshotPayload = serde_json::from_value(snapshot1.payload).unwrap();
        assert!(matches!(snap1.mode, SnapshotMode::Irl));

        write_frame(&mut client, &set_mode_request_envelope("irl"))
            .await
            .unwrap();

        let until = tokio::time::Instant::now() + Duration::from_millis(350);
        let mut saw_redundant_notice = false;
        let mut saw_redundant_snapshot = false;
        while tokio::time::Instant::now() < until {
            let _ = write_frame(&mut client, &ping_envelope("keepalive-noop-mode")).await;
            match tokio::time::timeout(Duration::from_millis(80), read_event(&mut client)).await {
                Ok(msg) => {
                    if msg.message_type == "user_notice" {
                        let payload: UserNoticePayload = serde_json::from_value(msg.payload).unwrap();
                        if payload.message.contains("Dock mode override set to irl") {
                            saw_redundant_notice = true;
                            break;
                        }
                    } else if msg.message_type == "status_snapshot"
                        && matches!(msg.priority, Priority::High)
                    {
                        saw_redundant_snapshot = true;
                        break;
                    }
                }
                Err(_) => {}
            }
        }
        assert!(!saw_redundant_notice, "unexpected duplicate user_notice for no-op set_mode_request");
        assert!(!saw_redundant_snapshot, "unexpected duplicate status_snapshot for no-op set_mode_request");

        drop(client);
        let _ = task.await;
    }

    #[tokio::test]
    async fn repeated_identical_set_setting_request_is_noop() {
        let (mut client, task, _tx, _cmd_tx) = spawn_test_session().await;

        write_frame(&mut client, &hello_envelope()).await.unwrap();
        let ack = read_event(&mut client).await;
        assert_eq!(ack.message_type, "hello_ack");

        let _ = drain_until_message_type(&mut client, "status_snapshot", Duration::from_secs(1)).await;

        write_frame(
            &mut client,
            &set_setting_request_envelope("auto_scene_switch", true),
        )
        .await
        .unwrap();
        let notice1 = drain_until_message_type(&mut client, "user_notice", Duration::from_secs(1)).await;
        let payload1: UserNoticePayload = serde_json::from_value(notice1.payload).unwrap();
        assert!(payload1.message.contains("auto_scene_switch"));
        let snapshot1 = drain_until_message_type(&mut client, "status_snapshot", Duration::from_secs(1)).await;
        let snap1: StatusSnapshotPayload = serde_json::from_value(snapshot1.payload).unwrap();
        let settings1 = snap1.settings.expect("expected settings payload after set_setting_request");
        assert_eq!(settings1.auto_scene_switch, Some(true));

        write_frame(
            &mut client,
            &set_setting_request_envelope("auto_scene_switch", true),
        )
        .await
        .unwrap();

        let until = tokio::time::Instant::now() + Duration::from_millis(350);
        let mut saw_redundant_notice = false;
        let mut saw_redundant_snapshot = false;
        while tokio::time::Instant::now() < until {
            let _ = write_frame(&mut client, &ping_envelope("keepalive-noop-setting")).await;
            match tokio::time::timeout(Duration::from_millis(80), read_event(&mut client)).await {
                Ok(msg) => {
                    if msg.message_type == "user_notice" {
                        let payload: UserNoticePayload = serde_json::from_value(msg.payload).unwrap();
                        if payload.message.contains("auto_scene_switch") {
                            saw_redundant_notice = true;
                            break;
                        }
                    } else if msg.message_type == "status_snapshot"
                        && matches!(msg.priority, Priority::High)
                    {
                        saw_redundant_snapshot = true;
                        break;
                    }
                }
                Err(_) => {}
            }
        }
        assert!(
            !saw_redundant_notice,
            "unexpected duplicate user_notice for no-op set_setting_request"
        );
        assert!(
            !saw_redundant_snapshot,
            "unexpected duplicate status_snapshot for no-op set_setting_request"
        );

        drop(client);
        let _ = task.await;
    }
}
