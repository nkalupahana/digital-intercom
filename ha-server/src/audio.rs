use anyhow::Result;
use bytes::Bytes;
use rtc::media::Sample;
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::mpsc;
use webrtc::media_stream::track_local::static_sample::TrackLocalStaticSample;
use webrtc::media_stream::track_remote::{TrackRemote, TrackRemoteEvent};

use crate::intercom::IntercomControl;

pub const SAMPLE_RATE_HZ: u32 = 48_000;
pub const FRAME_DURATION: Duration = Duration::from_millis(20);
pub const FRAME_SAMPLES_48K: usize = 960; // 20ms at 48 kHz
pub const FRAME_SAMPLES_32K: usize = 640; // 20ms at 32 kHz
const FRAME_BYTES_32K: usize = FRAME_SAMPLES_32K * 2;
const TALK_UDP_PACKET_SIZE: usize = 1024;
const LISTEN_BUFFER_LIMIT: usize = FRAME_BYTES_32K * 10;

pub async fn stream_listen(
    track: Arc<TrackLocalStaticSample>,
    ssrc: u32,
    payload_type: u8,
    mut listen_rx: mpsc::Receiver<Vec<u8>>,
) -> Result<()> {
    let mut encoder = opus::Encoder::new(
        SAMPLE_RATE_HZ,
        opus::Channels::Mono,
        opus::Application::Voip,
    )?;
    let mut encoded = vec![0u8; 4000];
    let mut pcm_buf = Vec::<u8>::new();
    let mut ticker = tokio::time::interval(FRAME_DURATION);
    ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    println!("streaming intercom listen audio (ctrl-c to stop)");

    loop {
        ticker.tick().await;
        while let Ok(pkt) = listen_rx.try_recv() {
            pcm_buf.extend_from_slice(&pkt);
        }
        if pcm_buf.len() > LISTEN_BUFFER_LIMIT {
            let keep = FRAME_BYTES_32K * 2;
            pcm_buf.drain(..pcm_buf.len() - keep);
        }

        let mut frame_32k = [0i16; FRAME_SAMPLES_32K];
        if pcm_buf.len() >= FRAME_BYTES_32K {
            for (i, sample) in frame_32k.iter_mut().enumerate() {
                let off = i * 2;
                *sample = i16::from_le_bytes([pcm_buf[off], pcm_buf[off + 1]]);
            }
            pcm_buf.drain(..FRAME_BYTES_32K);
        }

        let frame_48k = upsample_32k_to_48k(&frame_32k);
        let n = encoder.encode(&frame_48k, &mut encoded)?;
        track
            .sample_writer(ssrc, payload_type)
            .write_sample(&Sample {
                data: Bytes::copy_from_slice(&encoded[..n]),
                duration: FRAME_DURATION,
                ..Default::default()
            })
            .await?;
    }
}

pub async fn forward_talk(track: Arc<dyn TrackRemote>, intercom: IntercomControl) -> Result<()> {
    let mut decoder = opus::Decoder::new(SAMPLE_RATE_HZ, opus::Channels::Mono)?;
    let mut pcm = vec![0i16; FRAME_SAMPLES_48K * 6];
    let mut udp_buf = Vec::<u8>::new();
    println!("receiving remote talk track");

    while let Some(event) = track.poll().await {
        match event {
            TrackRemoteEvent::OnRtpPacket(pkt) => {
                let n = match decoder.decode(&pkt.payload, &mut pcm, false) {
                    Ok(n) => n,
                    Err(err) => {
                        eprintln!("opus decode error: {err}");
                        continue;
                    }
                };
                if n == 0 {
                    continue;
                }
                let down = downsample_48k_to_16k(&pcm[..n]);
                for sample in down {
                    udp_buf.extend_from_slice(&sample.to_le_bytes());
                }
                while udp_buf.len() >= TALK_UDP_PACKET_SIZE {
                    let _ = intercom.send_talk(&udp_buf[..TALK_UDP_PACKET_SIZE]).await;
                    udp_buf.drain(..TALK_UDP_PACKET_SIZE);
                }
            }
            TrackRemoteEvent::OnEnded | TrackRemoteEvent::OnEnding => {
                println!("remote talk track ended");
                break;
            }
            _ => {}
        }
    }
    Ok(())
}

fn upsample_32k_to_48k(input: &[i16; FRAME_SAMPLES_32K]) -> [i16; FRAME_SAMPLES_48K] {
    let mut out = [0i16; FRAME_SAMPLES_48K];
    for (i, sample) in out.iter_mut().enumerate() {
        let src_x3 = i * 2;
        let idx = src_x3 / 3;
        let rem = src_x3 % 3;
        let a = i32::from(input[idx.min(FRAME_SAMPLES_32K - 1)]);
        let b = i32::from(input[(idx + 1).min(FRAME_SAMPLES_32K - 1)]);
        *sample = (a + (b - a) * rem as i32 / 3) as i16;
    }
    out
}

fn downsample_48k_to_16k(input: &[i16]) -> Vec<i16> {
    input
        .chunks_exact(3)
        .map(|chunk| ((i32::from(chunk[0]) + i32::from(chunk[1]) + i32::from(chunk[2])) / 3) as i16)
        .collect()
}
