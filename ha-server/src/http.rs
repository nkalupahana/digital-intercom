use anyhow::{Context, Result, bail};
use axum::Router;
use axum::extract::State;
use axum::http::header::AUTHORIZATION;
use axum::http::{HeaderMap, StatusCode};
use axum::routing::post;
use tokio::net::TcpListener;

use crate::intercom::IntercomControl;

pub struct Config {
    addr: String,
    port: u16,
    token: String,
}

impl Config {
    pub fn from_env() -> Result<Self> {
        let token = std::env::var("UNLOCK_TOKEN").context("UNLOCK_TOKEN is required")?;
        if token.trim().is_empty() {
            bail!("UNLOCK_TOKEN is empty");
        }
        let addr = std::env::var("HTTP_ADDR").context("HTTP_ADDR is required")?;
        if addr.trim().is_empty() {
            bail!("HTTP_ADDR is empty");
        }
        let port = std::env::var("HTTP_PORT").context("HTTP_PORT is required")?;
        if port.trim().is_empty() {
            bail!("HTTP_PORT is empty");
        }
        let port = port
            .trim()
            .parse()
            .context("HTTP_PORT must be a port number")?;
        Ok(Self { addr, port, token })
    }
}

#[derive(Clone)]
struct AppState {
    intercom: IntercomControl,
    token: String,
}

pub async fn serve(config: Config, intercom: IntercomControl) -> Result<()> {
    let addr = config.addr;
    let port = config.port;
    let app = Router::new()
        .route("/unlock", post(unlock))
        .with_state(AppState {
            intercom,
            token: config.token,
        });
    let listener = TcpListener::bind((addr.as_str(), port))
        .await
        .with_context(|| format!("bind HTTP {addr}:{port}"))?;
    println!("HTTP listening on {addr}:{port}");
    axum::serve(listener, app).await.context("HTTP server")?;
    Ok(())
}

async fn unlock(State(state): State<AppState>, headers: HeaderMap) -> StatusCode {
    if !authorized(&headers, &state.token) {
        return StatusCode::UNAUTHORIZED;
    }
    if !state.intercom.is_connected() {
        return StatusCode::SERVICE_UNAVAILABLE;
    }
    if state.intercom.unlock().await.is_err() {
        return StatusCode::SERVICE_UNAVAILABLE;
    }
    StatusCode::NO_CONTENT
}

fn authorized(headers: &HeaderMap, token: &str) -> bool {
    let Some(value) = headers.get(AUTHORIZATION).and_then(|v| v.to_str().ok()) else {
        return false;
    };
    value
        .strip_prefix("Bearer ")
        .is_some_and(|provided| provided == token)
}
