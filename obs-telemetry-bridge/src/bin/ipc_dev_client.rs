use serde::{Deserialize, Serialize};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::sync::mpsc;
use uuid::Uuid;

const IPC_PROTOCOL_VERSION: u8 = 1;
const MAX_FRAME_SIZE: usize = 64 * 1024;
const CMD_PIPE_NAME: &str = r"\\.\pipe\aegis_cmd_v1";
const EVT_PIPE_NAME: &str = r"\\.\pipe\aegis_evt_v1";

#[derive(Debug, Clone, Serialize, Deserialize)]
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
    capabilities: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct PingPayload {
    nonce: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
struct RequestStatusPayload {}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct SceneSwitchResultPayload {
    request_id: String,
    ok: bool,
    error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct SwitchScenePayload {
    request_id: String,
    scene_name: String,
    reason: String,
    deadline_ms: u64,
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

async fn read_frame<R>(reader: &mut R) -> std::io::Result<Envelope<serde_json::Value>>
where
    R: AsyncRead + Unpin,
{
    let len = reader.read_u32_le().await? as usize;
    if len > MAX_FRAME_SIZE {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            format!("frame too large: {len}"),
        ));
    }
    let mut buf = vec![0u8; len];
    reader.read_exact(&mut buf).await?;
    rmp_serde::from_slice(&buf)
        .map_err(|err| std::io::Error::new(std::io::ErrorKind::InvalidData, err.to_string()))
}

async fn write_frame<W, T>(writer: &mut W, msg: &Envelope<T>) -> std::io::Result<()>
where
    W: AsyncWrite + Unpin,
    T: Serialize,
{
    let buf = rmp_serde::to_vec_named(msg)
        .map_err(|err| std::io::Error::new(std::io::ErrorKind::InvalidData, err.to_string()))?;
    if buf.len() > MAX_FRAME_SIZE {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            format!("encoded frame too large: {}", buf.len()),
        ));
    }
    writer.write_u32_le(buf.len() as u32).await?;
    writer.write_all(&buf).await?;
    writer.flush().await
}

#[cfg(windows)]
async fn run() -> Result<(), Box<dyn std::error::Error>> {
    use tokio::net::windows::named_pipe::ClientOptions;

    let auto_ack = !std::env::args().any(|a| a == "--no-auto-ack");
    let request_status = !std::env::args().any(|a| a == "--no-request-status");

    println!("ipc-dev-client: connecting");
    let mut cmd_pipe;
    let evt_pipe;

    loop {
        match (
            ClientOptions::new().open(CMD_PIPE_NAME),
            ClientOptions::new().open(EVT_PIPE_NAME),
        ) {
            (Ok(cmd), Ok(evt)) => {
                cmd_pipe = cmd;
                evt_pipe = evt;
                break;
            }
            _ => {
                tokio::time::sleep(Duration::from_millis(250)).await;
            }
        }
    }

    println!(
        "ipc-dev-client: connected to cmd={} evt={}",
        CMD_PIPE_NAME, EVT_PIPE_NAME
    );

    let hello = make_envelope(
        "hello",
        Priority::High,
        HelloPayload {
            plugin_version: "ipc-dev-client".to_string(),
            protocol_version: IPC_PROTOCOL_VERSION,
            obs_pid: std::process::id(),
            capabilities: vec![
                "scene_switch".to_string(),
                "dock".to_string(),
                "restart_hint".to_string(),
            ],
        },
    );
    let (mut evt_read, _evt_write) = tokio::io::split(evt_pipe);
    let (out_tx, mut out_rx) = mpsc::unbounded_channel::<Envelope<serde_json::Value>>();

    tokio::spawn(async move {
        loop {
            let Some(msg) = out_rx.recv().await else {
                break;
            };
            if write_frame(&mut cmd_pipe, &msg).await.is_err() {
                break;
            }
        }
    });

    let heartbeat_tx = out_tx.clone();
    tokio::spawn(async move {
        let mut ticker = tokio::time::interval(Duration::from_millis(1000));
        loop {
            ticker.tick().await;
            let nonce = Uuid::new_v4().to_string();
            let ping = make_envelope("ping", Priority::Normal, PingPayload { nonce });
            let ping_value = serde_json::to_value(ping.payload).unwrap_or_default();
            let env = Envelope {
                v: ping.v,
                id: ping.id,
                ts_unix_ms: ping.ts_unix_ms,
                message_type: ping.message_type,
                priority: ping.priority,
                payload: ping_value,
            };
            if heartbeat_tx.send(env).is_err() {
                break;
            }
        }
    });

    let hello_value = Envelope {
        v: hello.v,
        id: hello.id,
        ts_unix_ms: hello.ts_unix_ms,
        message_type: hello.message_type,
        priority: hello.priority,
        payload: serde_json::to_value(hello.payload)?,
    };
    out_tx.send(hello_value)?;
    println!("-> hello");

    if request_status {
        let req = make_envelope("request_status", Priority::High, RequestStatusPayload {});
        let req_value = Envelope {
            v: req.v,
            id: req.id,
            ts_unix_ms: req.ts_unix_ms,
            message_type: req.message_type,
            priority: req.priority,
            payload: serde_json::to_value(req.payload)?,
        };
        out_tx.send(req_value)?;
        println!("-> request_status");
    }

    loop {
        let msg = read_frame(&mut evt_read).await?;
        println!(
            "<- {} {}",
            msg.message_type,
            serde_json::to_string(&msg.payload)?
        );

        if msg.message_type == "switch_scene" {
            let payload: SwitchScenePayload = serde_json::from_value(msg.payload.clone())?;
            if auto_ack {
                let ack = make_envelope(
                    "scene_switch_result",
                    Priority::High,
                    SceneSwitchResultPayload {
                        request_id: payload.request_id.clone(),
                        ok: true,
                        error: None,
                    },
                );
                let ack_value = Envelope {
                    v: ack.v,
                    id: ack.id,
                    ts_unix_ms: ack.ts_unix_ms,
                    message_type: ack.message_type,
                    priority: ack.priority,
                    payload: serde_json::to_value(ack.payload)?,
                };
                if out_tx.send(ack_value).is_ok() {
                    println!("-> scene_switch_result ok {}", payload.request_id);
                }
            }
        }
    }
}

#[cfg(not(windows))]
async fn run() -> Result<(), Box<dyn std::error::Error>> {
    eprintln!("ipc-dev-client is Windows-only (named pipes)");
    Ok(())
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    run().await
}
