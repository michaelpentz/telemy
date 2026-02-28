use reqwest::{
    header::{HeaderMap, HeaderValue, AUTHORIZATION, CONTENT_TYPE},
    Client, Method, Request, StatusCode, Url,
};
use serde::{Deserialize, Serialize};
use std::{fmt, net::IpAddr, str::FromStr, time::Duration};

const DEFAULT_TIMEOUT_SECS: u64 = 15;
pub const DEFAULT_CLIENT_PLATFORM: &str = "windows";
pub const DEFAULT_CLIENT_VERSION: &str = env!("CARGO_PKG_VERSION");

#[derive(Clone, Debug)]
pub struct ControlPlaneClient {
    http: Client,
    base_url: Url,
    access_jwt: String,
    client_version: String,
    client_platform: String,
}

#[derive(Clone, Debug)]
pub struct ControlPlaneClientBuilder {
    base_url: String,
    access_jwt: String,
    client_version: String,
    client_platform: String,
    timeout: Duration,
}

#[allow(dead_code)] // retained for future client/plugin overrides and test tuning
impl ControlPlaneClientBuilder {
    pub fn new(base_url: impl Into<String>, access_jwt: impl Into<String>) -> Self {
        Self {
            base_url: base_url.into(),
            access_jwt: access_jwt.into(),
            client_version: DEFAULT_CLIENT_VERSION.to_string(),
            client_platform: DEFAULT_CLIENT_PLATFORM.to_string(),
            timeout: Duration::from_secs(DEFAULT_TIMEOUT_SECS),
        }
    }

    pub fn client_version(mut self, client_version: impl Into<String>) -> Self {
        self.client_version = client_version.into();
        self
    }

    pub fn client_platform(mut self, client_platform: impl Into<String>) -> Self {
        self.client_platform = client_platform.into();
        self
    }

    pub fn timeout(mut self, timeout: Duration) -> Self {
        self.timeout = timeout;
        self
    }

    pub fn build(self) -> Result<ControlPlaneClient, ControlPlaneError> {
        ControlPlaneClient::from_parts(
            self.base_url,
            self.access_jwt,
            self.client_version,
            self.client_platform,
            self.timeout,
        )
    }
}

impl ControlPlaneClient {
    pub fn builder(
        base_url: impl Into<String>,
        access_jwt: impl Into<String>,
    ) -> ControlPlaneClientBuilder {
        ControlPlaneClientBuilder::new(base_url, access_jwt)
    }

    pub fn new(
        base_url: impl Into<String>,
        access_jwt: impl Into<String>,
    ) -> Result<Self, ControlPlaneError> {
        Self::builder(base_url, access_jwt).build()
    }

    fn from_parts(
        base_url: String,
        access_jwt: String,
        client_version: String,
        client_platform: String,
        timeout: Duration,
    ) -> Result<Self, ControlPlaneError> {
        if access_jwt.trim().is_empty() {
            return Err(ControlPlaneError::Config(
                "control-plane access JWT must not be empty",
            ));
        }
        if client_version.trim().is_empty() {
            return Err(ControlPlaneError::Config(
                "client version header value must not be empty",
            ));
        }
        if client_platform.trim().is_empty() {
            return Err(ControlPlaneError::Config(
                "client platform header value must not be empty",
            ));
        }

        let mut parsed = Url::parse(base_url.trim()).map_err(|err| ControlPlaneError::Url(err.to_string()))?;
        if !parsed.path().ends_with('/') {
            let new_path = format!("{}/", parsed.path().trim_end_matches('/'));
            parsed.set_path(&new_path);
        }

        let http = Client::builder()
            .timeout(timeout)
            .build()
            .map_err(ControlPlaneError::Http)?;

        Ok(Self {
            http,
            base_url: parsed,
            access_jwt,
            client_version,
            client_platform,
        })
    }

    pub async fn relay_active(&self) -> Result<Option<RelaySession>, ControlPlaneError> {
        let req = self.build_request(Method::GET, "relay/active")?;
        let resp = self.http.execute(req).await.map_err(ControlPlaneError::Http)?;
        let status = resp.status();
        let body = resp.text().await.map_err(ControlPlaneError::Http)?;
        parse_relay_active_response(status, &body)
    }

    pub async fn relay_start(
        &self,
        idempotency_key: &str,
        request: &RelayStartRequest,
    ) -> Result<RelaySession, ControlPlaneError> {
        let req = self.build_relay_start_request(idempotency_key, request)?;
        let resp = self.http.execute(req).await.map_err(ControlPlaneError::Http)?;
        let status = resp.status();
        let body = resp.text().await.map_err(ControlPlaneError::Http)?;
        parse_relay_start_response(status, &body)
    }

    pub async fn relay_stop(
        &self,
        request: &RelayStopRequest,
    ) -> Result<RelayStopResponse, ControlPlaneError> {
        let req = self.build_relay_stop_request(request)?;
        let resp = self.http.execute(req).await.map_err(ControlPlaneError::Http)?;
        let status = resp.status();
        let body = resp.text().await.map_err(ControlPlaneError::Http)?;
        parse_relay_stop_response(status, &body)
    }

    pub fn build_relay_active_request(&self) -> Result<Request, ControlPlaneError> {
        self.build_request(Method::GET, "relay/active")
    }

    pub fn build_relay_start_request(
        &self,
        idempotency_key: &str,
        request: &RelayStartRequest,
    ) -> Result<Request, ControlPlaneError> {
        if idempotency_key.trim().is_empty() {
            return Err(ControlPlaneError::Config(
                "Idempotency-Key must not be empty for relay/start",
            ));
        }

        let body = serde_json::to_vec(request).map_err(ControlPlaneError::Json)?;
        let mut builder = self.build_request_builder(Method::POST, "relay/start")?;
        builder = builder.header("Idempotency-Key", idempotency_key.trim());
        builder = builder.header(CONTENT_TYPE, "application/json");
        builder.body(body).build().map_err(ControlPlaneError::Http)
    }

    pub fn build_relay_stop_request(
        &self,
        request: &RelayStopRequest,
    ) -> Result<Request, ControlPlaneError> {
        let body = serde_json::to_vec(request).map_err(ControlPlaneError::Json)?;
        let builder = self
            .build_request_builder(Method::POST, "relay/stop")?
            .header(CONTENT_TYPE, "application/json")
            .body(body);
        builder.build().map_err(ControlPlaneError::Http)
    }

    fn build_request(&self, method: Method, path: &str) -> Result<Request, ControlPlaneError> {
        self.build_request_builder(method, path)?
            .build()
            .map_err(ControlPlaneError::Http)
    }

    fn build_request_builder(
        &self,
        method: Method,
        path: &str,
    ) -> Result<reqwest::RequestBuilder, ControlPlaneError> {
        let url = self.base_url.join(&format!("api/v1/{}", path)).map_err(|err| ControlPlaneError::Url(err.to_string()))?;
        let headers = self.common_headers()?;
        Ok(self.http.request(method, url).headers(headers))
    }

    fn common_headers(&self) -> Result<HeaderMap, ControlPlaneError> {
        let mut headers = HeaderMap::new();
        headers.insert(
            AUTHORIZATION,
            HeaderValue::from_str(&format!("Bearer {}", self.access_jwt))
                .map_err(ControlPlaneError::InvalidHeaderValue)?,
        );
        headers.insert(
            "X-Aegis-Client-Version",
            HeaderValue::from_str(self.client_version.trim())
                .map_err(ControlPlaneError::InvalidHeaderValue)?,
        );
        headers.insert(
            "X-Aegis-Client-Platform",
            HeaderValue::from_str(self.client_platform.trim())
                .map_err(ControlPlaneError::InvalidHeaderValue)?,
        );
        Ok(headers)
    }
}

#[derive(Debug)]
pub enum ControlPlaneError {
    Config(&'static str),
    Url(String),
    Http(reqwest::Error),
    Json(serde_json::Error),
    InvalidHeaderValue(reqwest::header::InvalidHeaderValue),
    Api { status: StatusCode, body: String },
}

impl fmt::Display for ControlPlaneError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Config(msg) => write!(f, "config error: {msg}"),
            Self::Url(err) => write!(f, "url error: {err}"),
            Self::Http(err) => write!(f, "http error: {err}"),
            Self::Json(err) => write!(f, "json error: {err}"),
            Self::InvalidHeaderValue(err) => write!(f, "invalid header value: {err}"),
            Self::Api { status, body } => write!(f, "api error {}: {}", status.as_u16(), body),
        }
    }
}

impl std::error::Error for ControlPlaneError {}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct RelayStartRequest {
    #[serde(default)]
    pub region_preference: Option<String>,
    #[serde(default)]
    pub client_context: Option<RelayStartClientContext>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct RelayStartClientContext {
    #[serde(default)]
    pub obs_connected: Option<bool>,
    #[serde(default)]
    pub mode: Option<String>,
    #[serde(default)]
    pub requested_by: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RelayStopRequest {
    pub session_id: String,
    pub reason: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RelayStopResponse {
    pub session_id: String,
    pub status: String,
    #[serde(default)]
    pub stopped_at: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RelaySessionEnvelope {
    pub session: RelaySession,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct RelaySession {
    pub session_id: String,
    pub status: String,
    #[serde(default)]
    pub user_id: Option<String>,
    #[serde(default)]
    pub region: Option<String>,
    #[serde(default)]
    pub relay: Option<RelayEndpoint>,
    #[serde(default)]
    pub credentials: Option<RelayCredentials>,
    #[serde(default)]
    pub timers: Option<RelayTimers>,
    #[serde(default)]
    pub usage: Option<RelayUsage>,
}

impl RelaySession {
    fn normalize(mut self) -> Self {
        if let Some(relay) = self.relay.as_mut() {
            relay.normalize();
        }
        self
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct RelayEndpoint {
    #[serde(default)]
    pub instance_id: Option<String>,
    #[serde(default)]
    pub public_ip: Option<String>,
    #[serde(default)]
    pub srt_port: Option<u16>,
    #[serde(default)]
    pub ws_url: Option<String>,
}

impl RelayEndpoint {
    fn normalize(&mut self) {
        if let Some(ip) = self.public_ip.as_mut() {
            *ip = normalize_ip_string(ip);
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct RelayCredentials {
    #[serde(default)]
    pub pair_token: Option<String>,
    #[serde(default)]
    pub relay_ws_token: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct RelayTimers {
    #[serde(default)]
    pub grace_window_seconds: Option<u64>,
    #[serde(default)]
    pub grace_remaining_seconds: Option<u64>,
    #[serde(default)]
    pub max_session_seconds: Option<u64>,
    #[serde(default)]
    pub max_session_remaining_seconds: Option<u64>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct RelayUsage {
    #[serde(default)]
    pub started_at: Option<String>,
    #[serde(default)]
    pub ended_at: Option<String>,
    #[serde(default)]
    pub duration_seconds: Option<u64>,
}

pub fn parse_relay_active_response(
    status: StatusCode,
    body: &str,
) -> Result<Option<RelaySession>, ControlPlaneError> {
    if status == StatusCode::NO_CONTENT {
        return Ok(None);
    }
    if !status.is_success() {
        return Err(ControlPlaneError::Api {
            status,
            body: body.to_string(),
        });
    }

    let envelope: RelaySessionEnvelope = serde_json::from_str(body).map_err(ControlPlaneError::Json)?;
    Ok(Some(envelope.session.normalize()))
}

pub fn parse_relay_start_response(
    status: StatusCode,
    body: &str,
) -> Result<RelaySession, ControlPlaneError> {
    if !(status == StatusCode::OK || status == StatusCode::CREATED) {
        return Err(ControlPlaneError::Api {
            status,
            body: body.to_string(),
        });
    }
    let envelope: RelaySessionEnvelope = serde_json::from_str(body).map_err(ControlPlaneError::Json)?;
    Ok(envelope.session.normalize())
}

pub fn parse_relay_stop_response(
    status: StatusCode,
    body: &str,
) -> Result<RelayStopResponse, ControlPlaneError> {
    if !status.is_success() {
        return Err(ControlPlaneError::Api {
            status,
            body: body.to_string(),
        });
    }
    serde_json::from_str(body).map_err(ControlPlaneError::Json)
}

fn normalize_ip_string(raw: &str) -> String {
    let trimmed = raw.trim();
    if let Some((candidate_ip, suffix)) = trimmed.rsplit_once('/') {
        if !suffix.is_empty()
            && suffix.chars().all(|c| c.is_ascii_digit())
            && IpAddr::from_str(candidate_ip).is_ok()
        {
            return candidate_ip.to_string();
        }
    }
    trimmed.to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;

    fn client() -> ControlPlaneClient {
        ControlPlaneClient::builder("https://api.example.test/", "jwt-123")
            .client_version("0.0.3")
            .client_platform("windows")
            .build()
            .unwrap()
    }

    #[test]
    fn active_request_includes_required_common_headers() {
        let req = client().build_relay_active_request().unwrap();
        assert_eq!(req.method(), Method::GET);
        assert_eq!(req.url().as_str(), "https://api.example.test/api/v1/relay/active");
        assert_eq!(
            req.headers().get(AUTHORIZATION).unwrap(),
            &HeaderValue::from_static("Bearer jwt-123")
        );
        assert_eq!(
            req.headers().get("X-Aegis-Client-Version").unwrap(),
            &HeaderValue::from_static("0.0.3")
        );
        assert_eq!(
            req.headers().get("X-Aegis-Client-Platform").unwrap(),
            &HeaderValue::from_static("windows")
        );
    }

    #[test]
    fn start_request_includes_idempotency_key_and_json_body() {
        let req = client()
            .build_relay_start_request(
                "idem-123",
                &RelayStartRequest {
                    region_preference: Some("auto".to_string()),
                    client_context: Some(RelayStartClientContext {
                        obs_connected: Some(true),
                        mode: Some("studio".to_string()),
                        requested_by: Some("dashboard".to_string()),
                    }),
                },
            )
            .unwrap();

        assert_eq!(req.method(), Method::POST);
        assert_eq!(req.url().as_str(), "https://api.example.test/api/v1/relay/start");
        assert_eq!(
            req.headers().get("Idempotency-Key").unwrap(),
            &HeaderValue::from_static("idem-123")
        );
        assert_eq!(
            req.headers().get(CONTENT_TYPE).unwrap(),
            &HeaderValue::from_static("application/json")
        );

        let body = req.body().unwrap().as_bytes().unwrap();
        let json: Value = serde_json::from_slice(body).unwrap();
        assert_eq!(json["region_preference"], "auto");
        assert_eq!(json["client_context"]["mode"], "studio");
    }

    #[test]
    fn parse_active_returns_none_on_204() {
        let out = parse_relay_active_response(StatusCode::NO_CONTENT, "").unwrap();
        assert!(out.is_none());
    }

    #[test]
    fn parse_start_normalizes_public_ip_cidr_suffix() {
        let session = parse_relay_start_response(
            StatusCode::CREATED,
            r#"{
                "session": {
                    "session_id": "ses_1",
                    "status": "active",
                    "relay": {
                        "public_ip": "203.0.113.10/32",
                        "srt_port": 9000,
                        "ws_url": "wss://203.0.113.10:7443/telemetry"
                    }
                }
            }"#,
        )
        .unwrap();

        assert_eq!(
            session.relay.unwrap().public_ip.as_deref(),
            Some("203.0.113.10")
        );
    }

    #[test]
    fn parse_start_keeps_plain_public_ip_unchanged() {
        let session = parse_relay_start_response(
            StatusCode::OK,
            r#"{
                "session": {
                    "session_id": "ses_2",
                    "status": "active",
                    "relay": { "public_ip": "203.0.113.10" }
                }
            }"#,
        )
        .unwrap();

        assert_eq!(
            session.relay.unwrap().public_ip.as_deref(),
            Some("203.0.113.10")
        );
    }

    #[test]
    fn normalize_ip_string_strips_ipv6_host_prefix_suffix_too() {
        assert_eq!(normalize_ip_string("2001:db8::1/128"), "2001:db8::1");
    }

    #[test]
    fn start_request_rejects_empty_idempotency_key() {
        let err = client()
            .build_relay_start_request("   ", &RelayStartRequest::default())
            .unwrap_err();
        assert!(format!("{err}").contains("Idempotency-Key"));
    }
}

