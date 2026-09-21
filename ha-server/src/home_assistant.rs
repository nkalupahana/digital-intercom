use anyhow::{Context, Result, bail};
use std::time::Duration;

const DOORBELL_ENTITY: &str = "binary_sensor.doorbell";
const DOORBELL_NAME: &str = "doorbell";
const PULSE_DURATION: Duration = Duration::from_secs(1);

#[derive(Clone)]
pub struct HomeAssistant {
    http: reqwest::Client,
    base_url: String,
    token: String,
}

impl HomeAssistant {
    pub fn from_env() -> Result<Self> {
        let url = std::env::var("HOME_ASSISTANT_URL").context("HOME_ASSISTANT_URL is required")?;
        let token =
            std::env::var("HOME_ASSISTANT_TOKEN").context("HOME_ASSISTANT_TOKEN is required")?;
        Self::new(url, token)
    }

    fn new(url: String, token: String) -> Result<Self> {
        let base_url = url.trim().trim_end_matches('/').to_string();
        if base_url.is_empty() {
            bail!("HOME_ASSISTANT_URL is empty");
        }
        if token.trim().is_empty() {
            bail!("HOME_ASSISTANT_TOKEN is empty");
        }
        Ok(Self {
            http: reqwest::Client::new(),
            base_url,
            token,
        })
    }

    pub async fn set_doorbell(&self, on: bool) -> Result<()> {
        let state = if on { "on" } else { "off" };
        let url = format!("{}/api/states/{DOORBELL_ENTITY}", self.base_url);
        let response = self
            .http
            .post(&url)
            .bearer_auth(&self.token)
            .json(&serde_json::json!({
                "state": state,
                "attributes": { "friendly_name": DOORBELL_NAME },
            }))
            .send()
            .await
            .with_context(|| format!("POST {url}"))?;
        let status = response.status();
        if !status.is_success() {
            let text = response.text().await.unwrap_or_default();
            bail!("{url} failed ({status}): {text}");
        }
        Ok(())
    }

    pub async fn pulse_doorbell(&self) {
        if let Err(err) = self.set_doorbell(true).await {
            eprintln!("doorbell on: {err}");
            return;
        }
        tokio::time::sleep(PULSE_DURATION).await;
        if let Err(err) = self.set_doorbell(false).await {
            eprintln!("doorbell off: {err}");
        }
    }
}
