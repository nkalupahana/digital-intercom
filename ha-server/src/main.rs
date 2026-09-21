mod audio;
mod intercom;
mod prox;

use anyhow::Result;

#[tokio::main]
async fn main() -> Result<()> {
    let intercom = intercom::Intercom::start().await?;
    let published = prox::connect(intercom.control.clone()).await?;

    tokio::select! {
        result = audio::stream_listen(
            published.track,
            published.ssrc,
            published.payload_type,
            intercom.listen_rx,
        ) => result?,
        _ = tokio::signal::ctrl_c() => {
            println!("stopping");
        }
    }

    Ok(())
}
