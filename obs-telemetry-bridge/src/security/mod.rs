use base64::{engine::general_purpose, Engine as _};
use serde::{Deserialize, Serialize};
use std::{
    collections::HashMap,
    fs,
    path::{Path, PathBuf},
};
#[cfg(windows)]
use windows::Win32::Foundation::{LocalFree, HLOCAL};
#[cfg(windows)]
use windows::Win32::Security::Cryptography::{
    CryptProtectData, CryptUnprotectData, CRYPTPROTECT_UI_FORBIDDEN, CRYPT_INTEGER_BLOB,
};

#[derive(Debug)]
pub struct Vault {
    path: PathBuf,
    store: VaultStore,
}

#[derive(Debug, Default, Serialize, Deserialize)]
struct VaultStore {
    entries: HashMap<String, String>,
}

impl Vault {
    pub fn new(path: Option<&str>) -> Result<Self, Box<dyn std::error::Error>> {
        let path = match path {
            Some(p) => PathBuf::from(p),
            None => default_vault_path(),
        };

        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }

        let store = if path.exists() {
            let raw = fs::read_to_string(&path)?;
            serde_json::from_str(&raw).unwrap_or_default()
        } else {
            VaultStore::default()
        };

        Ok(Self { path, store })
    }

    pub fn store(&mut self, key: &str, value: &str) -> Result<(), Box<dyn std::error::Error>> {
        let encrypted = protect(value.as_bytes())?;
        let encoded = general_purpose::STANDARD.encode(encrypted);
        self.store.entries.insert(key.to_string(), encoded);
        self.persist()
    }

    pub fn retrieve(&self, key: &str) -> Result<String, Box<dyn std::error::Error>> {
        let encoded = self.store.entries.get(key).ok_or("missing vault key")?;
        let encrypted = general_purpose::STANDARD.decode(encoded)?;
        let decrypted = unprotect(&encrypted)?;
        Ok(String::from_utf8(decrypted)?)
    }

    pub fn list_keys(&self) -> Vec<String> {
        let mut keys: Vec<String> = self.store.entries.keys().cloned().collect();
        keys.sort();
        keys
    }

    fn persist(&self) -> Result<(), Box<dyn std::error::Error>> {
        let data = serde_json::to_string_pretty(&self.store)?;
        fs::write(&self.path, data)?;
        Ok(())
    }
}

fn default_vault_path() -> PathBuf {
    let base = std::env::var("APPDATA").unwrap_or_else(|_| ".".to_string());
    Path::new(&base).join("Telemy").join("vault.json")
}

#[cfg(windows)]
fn protect(data: &[u8]) -> Result<Vec<u8>, Box<dyn std::error::Error>> {
    unsafe {
        let in_blob = CRYPT_INTEGER_BLOB {
            cbData: data.len() as u32,
            pbData: data.as_ptr() as *mut u8,
        };
        let mut out_blob = CRYPT_INTEGER_BLOB::default();

        CryptProtectData(
            &in_blob,
            None,
            None,
            None,
            None,
            CRYPTPROTECT_UI_FORBIDDEN,
            &mut out_blob,
        )?;

        let out = std::slice::from_raw_parts(out_blob.pbData, out_blob.cbData as usize).to_vec();
        let _ = LocalFree(HLOCAL(out_blob.pbData as *mut _));
        Ok(out)
    }
}

#[cfg(windows)]
fn unprotect(data: &[u8]) -> Result<Vec<u8>, Box<dyn std::error::Error>> {
    unsafe {
        let in_blob = CRYPT_INTEGER_BLOB {
            cbData: data.len() as u32,
            pbData: data.as_ptr() as *mut u8,
        };
        let mut out_blob = CRYPT_INTEGER_BLOB::default();

        CryptUnprotectData(
            &in_blob,
            None,
            None,
            None,
            None,
            CRYPTPROTECT_UI_FORBIDDEN,
            &mut out_blob,
        )?;

        let out = std::slice::from_raw_parts(out_blob.pbData, out_blob.cbData as usize).to_vec();
        let _ = LocalFree(HLOCAL(out_blob.pbData as *mut _));
        Ok(out)
    }
}

#[cfg(not(windows))]
fn protect(data: &[u8]) -> Result<Vec<u8>, Box<dyn std::error::Error>> {
    // Linux fallback for server deployments; values are encoded, not encrypted.
    Ok(data.to_vec())
}

#[cfg(not(windows))]
fn unprotect(data: &[u8]) -> Result<Vec<u8>, Box<dyn std::error::Error>> {
    Ok(data.to_vec())
}
