use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};

const CONFIG_FILE: &str = "config.toml";
const ENV_PREFIX: &str = "TELEMY_";

#[derive(Debug, Clone, Deserialize, Serialize, Default)]
#[serde(default)]
pub struct Config {
    pub obs: ObsConfig,
    pub server: ServerConfig,
    pub vault: VaultConfig,
    pub grafana: GrafanaConfig,
    pub aegis: AegisConfig,
    pub network: NetworkConfig,
    pub startup: StartupConfig,
    pub tray: TrayConfig,
    pub theme: ThemeConfig,
    pub output_names: HashMap<String, String>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(default)]
pub struct ObsConfig {
    pub host: String,
    pub port: u16,
    pub password_key: Option<String>,
    pub auto_detect_process: bool,
    pub process_name: String,
}

impl Default for ObsConfig {
    fn default() -> Self {
        Self {
            host: "127.0.0.1".to_string(),
            port: 4455,
            password_key: None,
            auto_detect_process: true,
            process_name: "obs64.exe".to_string(),
        }
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(default)]
pub struct ServerConfig {
    pub port: u16,
    pub token: Option<String>,
}

impl Default for ServerConfig {
    fn default() -> Self {
        Self {
            port: 7070,
            token: None,
        }
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(default)]
pub struct GrafanaConfig {
    pub enabled: bool,
    pub endpoint: Option<String>,
    pub auth_header: String,
    pub auth_value_key: Option<String>,
    pub push_interval_ms: u64,
}

impl Default for GrafanaConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            endpoint: None,
            auth_header: "Authorization".to_string(),
            auth_value_key: None,
            push_interval_ms: 5000,
        }
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(default)]
pub struct AegisConfig {
    pub enabled: bool,
    pub base_url: Option<String>,
    pub access_jwt_key: Option<String>,
}

impl Default for AegisConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            base_url: None,
            access_jwt_key: None,
        }
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(default)]
pub struct NetworkConfig {
    pub latency_target: String,
}

impl Default for NetworkConfig {
    fn default() -> Self {
        Self {
            latency_target: "1.1.1.1:443".to_string(),
        }
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(default)]
pub struct StartupConfig {
    pub enable_autostart: bool,
    pub app_name: String,
}

impl Default for StartupConfig {
    fn default() -> Self {
        Self {
            enable_autostart: false,
            app_name: "Telemy".to_string(),
        }
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(default)]
pub struct TrayConfig {
    pub enable: bool,
}

impl Default for TrayConfig {
    fn default() -> Self {
        Self { enable: true }
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(default)]
pub struct ThemeConfig {
    pub font_family: String,
    pub bg: String,
    pub panel: String,
    pub muted: String,
    pub good: String,
    pub warn: String,
    pub bad: String,
    pub line: String,
}

impl Default for ThemeConfig {
    fn default() -> Self {
        Self {
            font_family: "Arial, sans-serif".to_string(),
            bg: "#0b0e12".to_string(),
            panel: "#111723".to_string(),
            muted: "#8da3c1".to_string(),
            good: "#33d17a".to_string(),
            warn: "#f6d32d".to_string(),
            bad: "#e01b24".to_string(),
            line: "#1f2a3a".to_string(),
        }
    }
}

#[derive(Debug, Clone, Deserialize, Serialize, Default)]
#[serde(default)]
pub struct VaultConfig {
    pub path: Option<String>,
}

impl Config {
    pub fn load() -> Result<Self, Box<dyn std::error::Error>> {
        // Start with default config
        let mut config = Self::default();
        let config_path = active_config_path();

        // Load from file if it exists
        if let Ok(raw) = fs::read_to_string(&config_path) {
            if let Ok(file_config) = toml::from_str::<Config>(&raw) {
                config = file_config;
            }
        }

        // Override with environment variables
        config.apply_env_overrides()?;

        config.validate()?;
        Ok(config)
    }

    fn apply_env_overrides(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        // OBS settings
        if let Ok(val) = env::var(format!("{}OBS_HOST", ENV_PREFIX)) {
            self.obs.host = val;
        }
        if let Ok(val) = env::var(format!("{}OBS_PORT", ENV_PREFIX)) {
            if let Ok(port) = val.parse() {
                self.obs.port = port;
            }
        }
        if let Ok(val) = env::var(format!("{}OBS_PASSWORD_KEY", ENV_PREFIX)) {
            self.obs.password_key = Some(val);
        }
        if let Ok(val) = env::var(format!("{}OBS_AUTO_DETECT", ENV_PREFIX)) {
            self.obs.auto_detect_process = val.parse().unwrap_or(true);
        }

        // Server settings
        if let Ok(val) = env::var(format!("{}SERVER_PORT", ENV_PREFIX)) {
            if let Ok(port) = val.parse() {
                self.server.port = port;
            }
        }
        if let Ok(val) = env::var(format!("{}SERVER_TOKEN", ENV_PREFIX)) {
            self.server.token = Some(val);
        }

        // Vault settings
        if let Ok(val) = env::var(format!("{}VAULT_PATH", ENV_PREFIX)) {
            self.vault.path = Some(val);
        }

        // Grafana settings
        if let Ok(val) = env::var(format!("{}GRAFANA_ENABLED", ENV_PREFIX)) {
            self.grafana.enabled = val.parse().unwrap_or(false);
        }
        if let Ok(val) = env::var(format!("{}GRAFANA_ENDPOINT", ENV_PREFIX)) {
            self.grafana.endpoint = Some(val);
        }
        if let Ok(val) = env::var(format!("{}GRAFANA_AUTH_VALUE_KEY", ENV_PREFIX)) {
            self.grafana.auth_value_key = Some(val);
        }
        if let Ok(val) = env::var(format!("{}GRAFANA_PUSH_INTERVAL_MS", ENV_PREFIX)) {
            if let Ok(interval) = val.parse() {
                self.grafana.push_interval_ms = interval;
            }
        }

        // Aegis control-plane settings
        if let Ok(val) = env::var(format!("{}AEGIS_ENABLED", ENV_PREFIX)) {
            self.aegis.enabled = val.parse().unwrap_or(false);
        }
        if let Ok(val) = env::var(format!("{}AEGIS_BASE_URL", ENV_PREFIX)) {
            self.aegis.base_url = Some(val);
        }
        if let Ok(val) = env::var(format!("{}AEGIS_ACCESS_JWT_KEY", ENV_PREFIX)) {
            self.aegis.access_jwt_key = Some(val);
        }

        // Network settings
        if let Ok(val) = env::var(format!("{}LATENCY_TARGET", ENV_PREFIX)) {
            self.network.latency_target = val;
        }

        // Startup settings
        if let Ok(val) = env::var(format!("{}AUTOSTART", ENV_PREFIX)) {
            self.startup.enable_autostart = val.parse().unwrap_or(false);
        }

        // Tray settings
        if let Ok(val) = env::var(format!("{}TRAY_ENABLE", ENV_PREFIX)) {
            self.tray.enable = val.parse().unwrap_or(true);
        }

        Ok(())
    }

    pub fn validate(&self) -> Result<(), Box<dyn std::error::Error>> {
        if self.obs.port == 0 {
            return Err("obs.port must be non-zero".into());
        }
        if self.server.port == 0 {
            return Err("server.port must be non-zero".into());
        }
        if self.grafana.enabled {
            if self.grafana.endpoint.as_deref().unwrap_or("").is_empty() {
                return Err("grafana.endpoint is required when grafana.enabled = true".into());
            }
            if self.grafana.auth_value_key.is_none() {
                return Err(
                    "grafana.auth_value_key is required when grafana.enabled = true".into(),
                );
            }
            if self.grafana.push_interval_ms < 500 {
                return Err("grafana.push_interval_ms must be >= 500".into());
            }
        }
        if self.aegis.enabled {
            if self.aegis.base_url.as_deref().unwrap_or("").trim().is_empty() {
                return Err("aegis.base_url is required when aegis.enabled = true".into());
            }
            if self.aegis.access_jwt_key.as_deref().unwrap_or("").trim().is_empty() {
                return Err("aegis.access_jwt_key is required when aegis.enabled = true".into());
            }
        }
        if self.network.latency_target.trim().is_empty() {
            return Err("network.latency_target must be set".into());
        }
        Ok(())
    }

    pub fn write_default<P: AsRef<Path>>(path: P) -> Result<(), Box<dyn std::error::Error>> {
        if path.as_ref().exists() {
            return Err("config.toml already exists".into());
        }
        if let Some(parent) = path.as_ref().parent() {
            if !parent.as_os_str().is_empty() {
                fs::create_dir_all(parent)?;
            }
        }
        let data = toml::to_string_pretty(&Config::default())?;
        fs::write(path, data)?;
        Ok(())
    }

    pub fn save(&self) -> Result<(), Box<dyn std::error::Error>> {
        let path = active_config_path();
        self.validate()?;
        let data = toml::to_string_pretty(self)?;
        if let Some(parent) = path.parent() {
            if !parent.as_os_str().is_empty() {
                fs::create_dir_all(parent)?;
            }
        }
        fs::write(path, data)?;
        Ok(())
    }

    pub fn default_path() -> PathBuf {
        managed_config_path()
    }
}

fn managed_config_path() -> PathBuf {
    if let Ok(path) = env::var(format!("{}CONFIG_PATH", ENV_PREFIX)) {
        return PathBuf::from(path);
    }
    let appdata = env::var("APPDATA").unwrap_or_else(|_| ".".to_string());
    Path::new(&appdata).join("Telemy").join(CONFIG_FILE)
}

fn active_config_path() -> PathBuf {
    let local = PathBuf::from(CONFIG_FILE);
    if local.exists() {
        local
    } else {
        managed_config_path()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_config_serializes() {
        let cfg = Config::default();
        let toml = toml::to_string_pretty(&cfg).unwrap();
        let parsed: Config = toml::from_str(&toml).unwrap();
        parsed.validate().unwrap();
    }

    #[test]
    fn validate_rejects_zero_ports() {
        let mut cfg = Config::default();
        cfg.obs.port = 0;
        assert!(cfg.validate().is_err());
        cfg.obs.port = 4455;
        cfg.server.port = 0;
        assert!(cfg.validate().is_err());
    }

    #[test]
    fn validate_requires_grafana_fields_when_enabled() {
        let mut cfg = Config::default();
        cfg.grafana.enabled = true;
        cfg.grafana.endpoint = None;
        assert!(cfg.validate().is_err());

        cfg.grafana.endpoint = Some("https://example.com".to_string());
        cfg.grafana.auth_value_key = None;
        assert!(cfg.validate().is_err());
    }

    #[test]
    fn validate_rejects_too_low_grafana_interval() {
        let mut cfg = Config::default();
        cfg.grafana.enabled = true;
        cfg.grafana.endpoint = Some("https://example.com".to_string());
        cfg.grafana.auth_value_key = Some("grafana_auth".to_string());
        cfg.grafana.push_interval_ms = 100;
        assert!(cfg.validate().is_err());
    }

    #[test]
    fn validate_requires_aegis_fields_when_enabled() {
        let mut cfg = Config::default();
        cfg.aegis.enabled = true;
        assert!(cfg.validate().is_err());

        cfg.aegis.base_url = Some("https://api.example.test".to_string());
        assert!(cfg.validate().is_err());

        cfg.aegis.access_jwt_key = Some("aegis_cp_access_jwt".to_string());
        assert!(cfg.validate().is_ok());
    }
}


