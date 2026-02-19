use serde::Serialize;

#[derive(Debug, Clone, Default, Serialize)]
pub struct TelemetryFrame {
    pub timestamp_unix: u64,
    pub health: f32,
    pub obs: ObsFrame,
    pub system: SystemFrame,
    pub streams: Vec<StreamOutput>,
    pub network: NetworkFrame,
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct ObsFrame {
    pub connected: bool,
    pub streaming: bool,
    pub recording: bool,
    pub studio_mode: bool,
    pub total_dropped_frames: u64,
    pub total_frames: u64,
    pub render_missed_frames: u32,
    pub render_total_frames: u32,
    pub output_skipped_frames: u32,
    pub output_total_frames: u32,
    pub active_fps: f32,
    pub available_disk_space_mb: f64,
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct SystemFrame {
    pub cpu_percent: f32,
    pub mem_percent: f32,
    pub gpu_percent: Option<f32>,
    pub gpu_temp_c: Option<f32>,
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct NetworkFrame {
    pub upload_mbps: f32,
    pub download_mbps: f32,
    pub latency_ms: f32,
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct StreamOutput {
    pub name: String,
    pub bitrate_kbps: u32,
    pub drop_pct: f32,
    pub fps: f32,
    pub encoding_lag_ms: f32,
}
