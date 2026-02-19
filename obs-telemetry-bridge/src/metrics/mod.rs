use crate::model::{NetworkFrame, ObsFrame, StreamOutput, SystemFrame, TelemetryFrame};
use nvml_wrapper::Nvml;
use obws::Client as ObsClient;
use std::net::SocketAddr;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use sysinfo::{Networks, System};
use tokio::io::AsyncWriteExt;
use tokio::net::TcpStream;
use tokio::time::timeout;

pub struct MetricsHub {
    obs_host: String,
    obs_port: u16,
    obs_password: Option<String>,
    obs_client: Option<ObsClient>,
    sys: System,
    networks: Networks,
    last_net_at: Option<Instant>,
    last_rx_bytes: u64,
    last_tx_bytes: u64,
    nvml: Option<Nvml>,
    latency_target: String,
    obs_auto_detect: bool,
    obs_process_name: String,
    last_process_check: Instant,
    obs_process_running: bool,
}

impl MetricsHub {
    pub fn new(
        obs_host: String,
        obs_port: u16,
        obs_password: Option<String>,
        latency_target: String,
        obs_auto_detect: bool,
        obs_process_name: String,
    ) -> Self {
        Self {
            obs_host,
            obs_port,
            obs_password,
            obs_client: None,
            sys: System::new(),
            networks: Networks::new_with_refreshed_list(),
            last_net_at: None,
            last_rx_bytes: 0,
            last_tx_bytes: 0,
            nvml: Nvml::init().ok(),
            latency_target,
            obs_auto_detect,
            obs_process_name,
            last_process_check: Instant::now() - Duration::from_secs(5),
            obs_process_running: true,
        }
    }

    pub async fn collect(&mut self) -> Result<TelemetryFrame, Box<dyn std::error::Error>> {
        let ts = SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs();

        if self.obs_auto_detect {
            self.refresh_obs_process();
        }

        if self.obs_client.is_none() && self.obs_process_running {
            self.try_connect_obs().await;
        }

        let mut outputs = Vec::new();
        let mut obs = ObsFrame::default();

        if let Some(client) = &self.obs_client {
            match client.outputs().list().await {
                Ok(list) => {
                    for output in list {
                        let mut stream = StreamOutput {
                            name: output.name,
                            bitrate_kbps: 0,
                            drop_pct: 0.0,
                            fps: 0.0,
                            encoding_lag_ms: 0.0,
                        };

                        if let Ok(status) = client.outputs().status(&stream.name).await {
                            let total_frames = status.total_frames as f32;
                            let skipped_frames = status.skipped_frames as f32;
                            let duration_secs =
                                status.duration.whole_milliseconds() as f32 / 1000.0;
                            let bytes = status.bytes as f32;

                            if total_frames > 0.0 {
                                stream.drop_pct = skipped_frames / total_frames;
                                stream.fps = if duration_secs > 0.0 {
                                    total_frames / duration_secs
                                } else {
                                    0.0
                                };
                            }

                            if duration_secs > 0.0 {
                                let kbps = (bytes * 8.0) / duration_secs / 1000.0;
                                stream.bitrate_kbps = kbps.round() as u32;
                            }
                        }

                        outputs.push(stream);
                    }
                }
                Err(_) => {
                    self.obs_client = None;
                }
            }

            if let Some(client) = &self.obs_client {
                match client.streaming().status().await {
                    Ok(status) => {
                        obs.connected = true;
                        obs.streaming = status.active;
                        obs.total_frames = status.total_frames as u64;
                        obs.total_dropped_frames = status.skipped_frames as u64;

                        let drop_pct = if status.total_frames > 0 {
                            status.skipped_frames as f32 / status.total_frames as f32
                        } else {
                            0.0
                        };

                        if !outputs.is_empty() {
                            for o in outputs.iter_mut() {
                                if o.drop_pct == 0.0 {
                                    o.drop_pct = drop_pct;
                                }
                            }
                        }
                    }
                    Err(_) => {
                        self.obs_client = None;
                    }
                }
            }

            // Collect OBS general stats (encoding lag, render/output frames, disk space)
            if let Some(client) = &self.obs_client {
                if let Ok(stats) = client.general().stats().await {
                    for o in outputs.iter_mut() {
                        o.encoding_lag_ms = stats.average_frame_render_time as f32;
                    }
                    obs.render_missed_frames = stats.render_skipped_frames;
                    obs.render_total_frames = stats.render_total_frames;
                    obs.output_skipped_frames = stats.output_skipped_frames;
                    obs.output_total_frames = stats.output_total_frames;
                    obs.active_fps = stats.active_fps as f32;
                    obs.available_disk_space_mb = stats.available_disk_space;
                }
            }

            // Collect recording status
            if let Some(client) = &self.obs_client {
                if let Ok(rec) = client.recording().status().await {
                    obs.recording = rec.active;
                }
            }

            // Detect OBS studio mode
            if let Some(client) = &self.obs_client {
                obs.studio_mode = client.ui().studio_mode_enabled().await.unwrap_or(false);
            }
        }

        let health = compute_health(&outputs);

        let (cpu_percent, mem_percent) = self.collect_system();
        let (gpu_percent, gpu_temp_c) = self.collect_gpu();
        let (upload_mbps, download_mbps) = self.collect_network();
        let latency_ms = self.collect_latency().await;

        Ok(TelemetryFrame {
            timestamp_unix: ts,
            health,
            obs,
            system: SystemFrame {
                cpu_percent,
                mem_percent,
                gpu_percent,
                gpu_temp_c,
            },
            network: NetworkFrame {
                upload_mbps,
                download_mbps,
                latency_ms,
            },
            streams: outputs,
        })
    }

    async fn try_connect_obs(&mut self) {
        let password = self.obs_password.as_deref();

        // Log connection attempt details (without exposing the actual password)
        let password_status = if password.is_some() {
            "with password"
        } else {
            "without password"
        };
        tracing::debug!(
            "Attempting to connect to OBS at {}:{} {}",
            self.obs_host,
            self.obs_port,
            password_status
        );

        match ObsClient::connect(&self.obs_host, self.obs_port, password).await {
            Ok(client) => {
                tracing::info!(
                    "Successfully connected to OBS at {}:{}",
                    self.obs_host,
                    self.obs_port
                );
                self.obs_client = Some(client);
            }
            Err(e) => {
                // Provide more specific error messages for common failures
                let error_msg = e.to_string();
                if error_msg.contains("handshake") {
                    tracing::warn!(
                        "Failed to connect to OBS at {}:{}: Authentication handshake failed. \
                        This usually means the password is incorrect or OBS WebSocket server requires authentication. \
                        Error: {}",
                        self.obs_host, self.obs_port, e
                    );
                } else {
                    tracing::warn!(
                        "Failed to connect to OBS at {}:{}: {}",
                        self.obs_host,
                        self.obs_port,
                        e
                    );
                }

                // Add a small delay before next connection attempt to avoid hammering
                tokio::time::sleep(Duration::from_secs(2)).await;
            }
        }
    }

    fn collect_system(&mut self) -> (f32, f32) {
        self.sys.refresh_cpu();
        self.sys.refresh_memory();

        let cpu = self.sys.global_cpu_info().cpu_usage();

        let mem_total = self.sys.total_memory() as f32;
        let mem_used = self.sys.used_memory() as f32;
        let mem_percent = if mem_total > 0.0 {
            (mem_used / mem_total) * 100.0
        } else {
            0.0
        };

        (cpu, mem_percent)
    }

    fn collect_network(&mut self) -> (f32, f32) {
        self.networks.refresh();

        let mut rx_bytes = 0u64;
        let mut tx_bytes = 0u64;
        for (_name, data) in &self.networks {
            rx_bytes = rx_bytes.saturating_add(data.received());
            tx_bytes = tx_bytes.saturating_add(data.transmitted());
        }

        let now = Instant::now();
        let mut upload_mbps = 0.0;
        let mut download_mbps = 0.0;

        if let Some(prev) = self.last_net_at {
            let dt = now.duration_since(prev).as_secs_f32();
            if dt > 0.0 {
                let delta_tx = tx_bytes.saturating_sub(self.last_tx_bytes);
                let delta_rx = rx_bytes.saturating_sub(self.last_rx_bytes);
                upload_mbps = (delta_tx as f32 * 8.0) / dt / 1_000_000.0;
                download_mbps = (delta_rx as f32 * 8.0) / dt / 1_000_000.0;
            }
        }

        self.last_net_at = Some(now);
        self.last_rx_bytes = rx_bytes;
        self.last_tx_bytes = tx_bytes;

        (upload_mbps, download_mbps)
    }

    fn collect_gpu(&mut self) -> (Option<f32>, Option<f32>) {
        let nvml = match &self.nvml {
            Some(nvml) => nvml,
            None => return (None, None),
        };

        let device = match nvml.device_by_index(0) {
            Ok(device) => device,
            Err(_) => return (None, None),
        };

        let util = device.utilization_rates().ok().map(|u| u.gpu as f32);
        let temp = device
            .temperature(nvml_wrapper::enum_wrappers::device::TemperatureSensor::Gpu)
            .ok()
            .map(|t| t as f32);

        (util, temp)
    }

    async fn collect_latency(&self) -> f32 {
        let addr: SocketAddr = match self.latency_target.parse() {
            Ok(addr) => addr,
            Err(_) => return 0.0,
        };

        let start = Instant::now();
        let connect = timeout(Duration::from_millis(250), TcpStream::connect(addr)).await;
        match connect {
            Ok(Ok(mut stream)) => {
                let _ = stream.shutdown().await;
                start.elapsed().as_millis() as f32
            }
            _ => 0.0,
        }
    }

    fn refresh_obs_process(&mut self) {
        if self.last_process_check.elapsed() < Duration::from_secs(2) {
            return;
        }
        self.last_process_check = Instant::now();
        self.sys.refresh_processes();
        let target = self.obs_process_name.to_lowercase();
        self.obs_process_running = self
            .sys
            .processes()
            .values()
            .any(|p| p.name().to_lowercase() == target);
    }
}

fn compute_health(outputs: &[StreamOutput]) -> f32 {
    if outputs.is_empty() {
        return 0.0;
    }
    let avg_drop = outputs.iter().map(|o| o.drop_pct).sum::<f32>() / outputs.len() as f32;
    let health = 1.0 - avg_drop;
    health.clamp(0.0, 1.0)
}
