use crate::config::Config;
use crate::exporters::GrafanaExporter;
use crate::metrics::MetricsHub;
use crate::model::TelemetryFrame;
use crate::security::Vault;
use rand::{distributions::Alphanumeric, Rng};
use std::net::SocketAddr;
use std::sync::atomic::AtomicBool;
use std::sync::{Arc, Mutex};
use tokio::sync::watch;
use tokio::time::Duration;

pub async fn run() -> Result<(), Box<dyn std::error::Error>> {
    let config = Config::load()?;

    if let Some(ref command) = std::env::args().nth(1) {
        if command == "vault-set" {
            return handle_vault_set(&config);
        }
        if command == "vault-get" {
            return handle_vault_get(&config);
        }
        if command == "vault-list" {
            return handle_vault_list(&config);
        }
        if command == "config-init" {
            return handle_config_init();
        }
        if command == "autostart-enable" {
            return handle_autostart(true, &config);
        }
        if command == "autostart-disable" {
            return handle_autostart(false, &config);
        }
    }

    let vault = Arc::new(Mutex::new(Vault::new(config.vault.path.as_deref())?));

    let obs_password = {
        let v = vault.lock().unwrap();
        match config.obs.password_key.as_deref() {
            Some(key) => v.retrieve(key).ok().map(|p| p.trim().to_string()),
            None => None,
        }
    };

    let grafana_auth_value = {
        let v = vault.lock().unwrap();
        match config.grafana.auth_value_key.as_deref() {
            Some(key) => v.retrieve(key).ok(),
            None => None,
        }
    };

    let grafana_configured =
        config.grafana.enabled && config.grafana.endpoint.is_some() && grafana_auth_value.is_some();

    let (tx, rx) = watch::channel(TelemetryFrame::default());
    let obs_host = config.obs.host.clone();
    let obs_port = config.obs.port;
    let latency_target = config.network.latency_target.clone();
    let obs_auto_detect = config.obs.auto_detect_process;
    let obs_process_name = config.obs.process_name.clone();

    let metrics_task = tokio::spawn(async move {
        let mut hub = MetricsHub::new(
            obs_host,
            obs_port,
            obs_password,
            latency_target,
            obs_auto_detect,
            obs_process_name,
        );
        let mut ticker = tokio::time::interval(std::time::Duration::from_millis(500));
        loop {
            ticker.tick().await;
            if let Ok(frame) = hub.collect().await {
                let _ = tx.send(frame);
            }
        }
    });

    if config.grafana.enabled {
        if let Some(endpoint) = config.grafana.endpoint.clone() {
            let export_rx = rx.clone();
            let interval_ms = config.grafana.push_interval_ms;
            tokio::spawn(async move {
                let mut backoff_ms = 1000u64;
                loop {
                    let exporter = GrafanaExporter::new(
                        &endpoint,
                        &config.grafana.auth_header,
                        grafana_auth_value.clone(),
                        interval_ms,
                    );

                    match exporter {
                        Ok(exporter) => {
                            let mut ticker =
                                tokio::time::interval(Duration::from_millis(interval_ms));
                            loop {
                                ticker.tick().await;
                                let frame = export_rx.borrow().clone();
                                exporter.record(&frame);
                            }
                        }
                        Err(err) => {
                            eprintln!("grafana exporter init failed: {err}");
                            tokio::time::sleep(Duration::from_millis(backoff_ms)).await;
                            backoff_ms = (backoff_ms * 2).min(30_000);
                        }
                    }
                }
            });
        }
    }

    if config.startup.enable_autostart {
        if let Err(err) = crate::startup::set_autostart(&config.startup.app_name, true) {
            eprintln!("autostart setup failed: {err}");
        }
    }

    let addr: SocketAddr = format!("127.0.0.1:{}", config.server.port).parse()?;

    // Get or generate server token, storing in vault for persistence
    let token = if let Some(token) = config.server.token {
        token
    } else {
        // Try to retrieve existing token from vault
        let vault_lock = vault.lock().unwrap();
        match vault_lock.retrieve("server_token") {
            Ok(existing_token) => existing_token,
            Err(_) => {
                // Generate new token and store in vault
                drop(vault_lock); // Drop lock before re-acquiring
                let new_token = generate_token(32);
                let mut vault_lock = vault.lock().unwrap();
                if let Err(e) = vault_lock.store("server_token", &new_token) {
                    tracing::warn!("Failed to store server token in vault: {}", e);
                }
                new_token
            }
        }
    };

    let dashboard_url = format!(
        "http://127.0.0.1:{}/obs?token={}",
        config.server.port, token
    );
    let settings_url = format!(
        "http://127.0.0.1:{}/settings?token={}",
        config.server.port, token
    );

    println!("OBS dashboard: {}", dashboard_url);

    let shutdown_flag = Arc::new(AtomicBool::new(false));
    let (shutdown_tx, mut shutdown_rx) = watch::channel(false);
    let shutdown_rx_server = shutdown_rx.clone();

    if config.tray.enable {
        let url = dashboard_url.clone();
        let settings = settings_url.clone();
        let flag = shutdown_flag.clone();
        let tx = shutdown_tx.clone();
        std::thread::spawn(move || {
            if let Err(err) = crate::tray::start_tray(url, settings, flag, tx) {
                eprintln!("tray failed: {err}");
            }
        });
    }

    tokio::select! {
        res = crate::server::start(addr, token, rx, shutdown_rx_server, config.theme.clone(), vault.clone(), grafana_configured) => res,
        _ = tokio::signal::ctrl_c() => {
            eprintln!("shutdown: ctrl-c");
            metrics_task.abort();
            let _ = shutdown_tx.send(true);
            Ok(())
        }
        _ = shutdown_rx.changed() => {
            eprintln!("shutdown: tray");
            metrics_task.abort();
            Ok(())
        }
    }
}

fn handle_vault_set(config: &Config) -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(2);
    let key = args.next().ok_or("missing key")?;
    let value = args.next().ok_or("missing value")?;

    let mut vault = Vault::new(config.vault.path.as_deref())?;
    vault.store(&key, &value)?;

    println!("Stored vault key: {}", key);
    Ok(())
}

fn handle_vault_get(config: &Config) -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(2);
    let key = args.next().ok_or("missing key")?;

    let vault = Vault::new(config.vault.path.as_deref())?;
    let value = vault.retrieve(&key)?;

    println!("{}", value);
    Ok(())
}

fn handle_vault_list(config: &Config) -> Result<(), Box<dyn std::error::Error>> {
    let vault = Vault::new(config.vault.path.as_deref())?;
    for key in vault.list_keys() {
        println!("{}", key);
    }
    Ok(())
}

fn handle_config_init() -> Result<(), Box<dyn std::error::Error>> {
    let path = Config::default_path();
    Config::write_default(&path)?;
    println!("Wrote default config to {}", path.display());
    Ok(())
}

fn handle_autostart(enable: bool, config: &Config) -> Result<(), Box<dyn std::error::Error>> {
    crate::startup::set_autostart(&config.startup.app_name, enable)?;
    println!(
        "autostart {} for {}",
        if enable { "enabled" } else { "disabled" },
        config.startup.app_name
    );
    Ok(())
}

fn generate_token(len: usize) -> String {
    rand::thread_rng()
        .sample_iter(&Alphanumeric)
        .take(len)
        .map(char::from)
        .collect()
}
