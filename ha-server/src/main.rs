mod audio;
mod home_assistant;
mod http;
mod intercom;
mod prox;

use anyhow::Result;
use home_assistant::HomeAssistant;

#[tokio::main]
async fn main() -> Result<()> {
    dotenvy::dotenv().ok();
    let ha = HomeAssistant::from_env()?;
    let http_config = http::Config::from_env()?;
    if let Err(err) = ha.set_doorbell(false).await {
        eprintln!("failed to create doorbell sensor: {err:#}");
    }

    let intercom = intercom::Intercom::start(ha).await?;
    let published = prox::connect(intercom.control.clone()).await?;
    let http_server = http::serve(http_config, intercom.control.clone());

    tokio::select! {
        result = audio::stream_listen(
            published.track,
            published.ssrc,
            published.payload_type,
            intercom.listen_rx,
        ) => result?,
        result = http_server => result?,
        _ = tokio::signal::ctrl_c() => {
            println!("stopping");
        }
    }

    Ok(())
}
