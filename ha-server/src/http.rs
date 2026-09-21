use anyhow::{Context, Result, bail};
use axum::Router;
use axum::extract::State;
use axum::http::header::AUTHORIZATION;
use axum::http::{HeaderMap, StatusCode};
use axum::routing::post;
use tokio::net::TcpListener;

use crate::intercom::IntercomControl;

const DEFAULT_HTTP_PORT: u16 = 8080;

pub struct Config {
    port: u16,
    token: String,
}

impl Config {
    pub fn from_env() -> Result<Self> {
        let token = std::env::var("UNLOCK_TOKEN").context("UNLOCK_TOKEN is required")?;
        if token.trim().is_empty() {
            bail!("UNLOCK_TOKEN is empty");
        }
        let port = match std::env::var("HTTP_PORT") {
            Ok(value) if !value.trim().is_empty() => value
                .trim()
                .parse()
                .context("HTTP_PORT must be a port number")?,
            _ => DEFAULT_HTTP_PORT,
        };
        Ok(Self { port, token })
    }
}

#[derive(Clone)]
struct AppState {
    intercom: IntercomControl,
    token: String,
}

pub async fn serve(config: Config, intercom: IntercomControl) -> Result<()> {
    let port = config.port;
    let app = Router::new()
        .route("/unlock", post(unlock))
        .with_state(AppState {
            intercom,
            token: config.token,
        });
    let listener = TcpListener::bind(("0.0.0.0", port))
        .await
        .with_context(|| format!("bind HTTP {port}"))?;
    println!("HTTP listening on 0.0.0.0:{port}");
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
