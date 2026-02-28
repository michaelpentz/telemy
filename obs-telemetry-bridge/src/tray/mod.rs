#[cfg(windows)]
use std::io::Write;
#[cfg(windows)]
use std::process::{Command, Stdio};
use std::sync::atomic::AtomicBool;
#[cfg(windows)]
use std::sync::atomic::Ordering;
use std::sync::Arc;
#[cfg(windows)]
use tray_item::{IconSource, TrayItem};

#[cfg(windows)]
pub fn start_tray(
    dashboard_url: String,
    settings_url: String,
    shutdown_flag: Arc<AtomicBool>,
    shutdown_tx: tokio::sync::watch::Sender<bool>,
) -> Result<(), Box<dyn std::error::Error>> {
    let mut tray = TrayItem::new("Telemy", IconSource::Resource("tray_default"))?;

    let open_url = dashboard_url.clone();
    tray.add_menu_item("Open Dashboard", move || {
        let _ = Command::new("cmd")
            .args(["/C", "start", "", &open_url])
            .spawn();
    })?;

    let settings = settings_url.clone();
    tray.add_menu_item("Settings", move || {
        let _ = Command::new("cmd")
            .args(["/C", "start", "", &settings])
            .spawn();
    })?;

    let copy_url = dashboard_url.clone();
    tray.add_menu_item("Copy Dashboard URL", move || {
        if let Ok(mut child) = Command::new("clip").stdin(Stdio::piped()).spawn() {
            if let Some(mut stdin) = child.stdin.take() {
                let _ = stdin.write_all(copy_url.as_bytes());
            }
        }
    })?;

    let quit_flag = shutdown_flag.clone();
    let quit_tx = shutdown_tx.clone();
    tray.add_menu_item("Quit", move || {
        quit_flag.store(true, Ordering::SeqCst);
        let _ = quit_tx.send(true);
    })?;

    loop {
        if shutdown_flag.load(Ordering::SeqCst) {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(500));
    }

    Ok(())
}

#[cfg(not(windows))]
pub fn start_tray(
    _dashboard_url: String,
    _settings_url: String,
    _shutdown_flag: Arc<AtomicBool>,
    _shutdown_tx: tokio::sync::watch::Sender<bool>,
) -> Result<(), Box<dyn std::error::Error>> {
    Err("tray is only supported on Windows".into())
}
