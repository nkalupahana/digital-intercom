use anyhow::{Context, Result, bail};
use futures_util::{SinkExt, StreamExt};
use rtc::interceptor::Registry;
use rtc::media_stream::MediaStreamTrack;
use rtc::peer_connection::configuration::interceptor_registry::register_default_interceptors;
use rtc::peer_connection::configuration::media_engine::{MIME_TYPE_OPUS, MediaEngine};
use rtc::rtp_transceiver::rtp_sender::{
    RTCRtpCodec, RTCRtpCodecParameters, RTCRtpCodingParameters, RTCRtpEncodingParameters,
    RtpCodecKind,
};
use serde::Deserialize;
use std::collections::HashSet;
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::{Mutex, watch};
use tokio_tungstenite::tungstenite::Message;
use uuid::Uuid;
use webrtc::media_stream::track_local::TrackLocal;
use webrtc::media_stream::track_local::static_sample::TrackLocalStaticSample;
use webrtc::media_stream::track_remote::TrackRemote;
use webrtc::peer_connection::{
    PeerConnection, PeerConnectionBuilder, PeerConnectionEventHandler, RTCConfigurationBuilder,
    RTCIceConnectionState, RTCIceGatheringState, RTCIceServer, RTCSessionDescription,
};
use webrtc::rtp_transceiver::{RTCRtpTransceiverDirection, RTCRtpTransceiverInit, RtpSender};
use webrtc::runtime::TokioRuntime;

use crate::audio::{self, SAMPLE_RATE_HZ};
use crate::intercom::{IntercomControl, Mode};

const BASE_URL: &str = "https://prox.nisa.la";
const REMOTE: &str = "github.com/nkalupahana/digital-intercom.git";
const SERVER_PATH: &str = "/intercom";
const LISTEN_PATH: &str = "/intercom/listen";
const TALK_PATH: &str = "/intercom/talk";

pub struct PublishedTrack {
    pub track: Arc<TrackLocalStaticSample>,
    pub ssrc: u32,
    pub payload_type: u8,
    _pc: Arc<dyn PeerConnection>,
}

#[derive(Clone)]
struct Handler {
    gathering: watch::Sender<RTCIceGatheringState>,
    ice: watch::Sender<RTCIceConnectionState>,
    intercom: IntercomControl,
}

#[async_trait::async_trait]
impl PeerConnectionEventHandler for Handler {
    async fn on_ice_gathering_state_change(&self, state: RTCIceGatheringState) {
        println!("ICE gathering state: {state}");
        let _ = self.gathering.send(state);
    }

    async fn on_ice_connection_state_change(&self, state: RTCIceConnectionState) {
        println!("ICE connection state: {state}");
        let _ = self.ice.send(state);
    }

    async fn on_track(&self, track: Arc<dyn TrackRemote>) {
        let intercom = self.intercom.clone();
        tokio::spawn(async move {
            if let Err(err) = audio::forward_talk(track, intercom).await {
                eprintln!("talk forward error: {err}");
            }
        });
    }
}

#[derive(Deserialize)]
struct SessionResponse {
    #[serde(rename = "sessionId")]
    session_id: String,
    #[serde(rename = "sessionDescription")]
    session_description: SdpBlob,
}

#[derive(Deserialize)]
struct SendTrackResponse {
    #[serde(rename = "sessionDescription")]
    session_description: SdpBlob,
}

#[derive(Deserialize)]
struct ReceiveTracksResponse {
    #[serde(rename = "sessionDescription")]
    session_description: SdpBlob,
}

#[derive(Deserialize)]
struct SdpBlob {
    sdp: String,
}

#[derive(Deserialize)]
struct ActiveSessionsMessage {
    command: String,
    #[serde(default)]
    sessions: Vec<ProxSession>,
}

#[derive(Deserialize)]
struct ProxSession {
    id: String,
    #[serde(rename = "trackId")]
    track_id: String,
    path: String,
}

pub async fn connect(intercom: IntercomControl) -> Result<PublishedTrack> {
    let http = reqwest::Client::new();

    let mut media_engine = MediaEngine::default();
    let audio_codec = RTCRtpCodecParameters {
        rtp_codec: RTCRtpCodec {
            mime_type: MIME_TYPE_OPUS.to_owned(),
            clock_rate: SAMPLE_RATE_HZ,
            channels: 1,
            sdp_fmtp_line: "minptime=10;useinbandfec=1".to_owned(),
            rtcp_feedback: vec![],
        },
        payload_type: 111,
    };
    media_engine.register_codec(audio_codec.clone(), RtpCodecKind::Audio)?;
    let registry = register_default_interceptors(Registry::new(), &mut media_engine)?;

    let config = RTCConfigurationBuilder::default()
        .with_ice_servers(vec![RTCIceServer {
            urls: vec!["stun:stun.cloudflare.com:3478".to_owned()],
            ..Default::default()
        }])
        .build();

    let (gathering_tx, gathering_rx) = watch::channel(RTCIceGatheringState::New);
    let (ice_tx, ice_rx) = watch::channel(RTCIceConnectionState::New);
    let handler = Arc::new(Handler {
        gathering: gathering_tx,
        ice: ice_tx,
        intercom: intercom.clone(),
    });

    let pc = PeerConnectionBuilder::new()
        .with_configuration(config)
        .with_media_engine(media_engine)
        .with_interceptor_registry(registry)
        .with_runtime(Arc::new(TokioRuntime))
        .with_handler(handler)
        .with_udp_addrs(vec!["0.0.0.0:0"])
        .build()
        .await?;
    let pc: Arc<dyn PeerConnection> = Arc::new(pc);

    let track_name = Uuid::new_v4().to_string();
    let ssrc = {
        let bytes = Uuid::new_v4().into_bytes();
        u32::from_le_bytes(bytes[0..4].try_into().unwrap()).max(1)
    };

    let audio_track = Arc::new(TrackLocalStaticSample::new(MediaStreamTrack::new(
        "ha-server".to_owned(),
        track_name.clone(),
        "ha-server-listen".to_owned(),
        RtpCodecKind::Audio,
        vec![RTCRtpEncodingParameters {
            rtp_coding_parameters: RTCRtpCodingParameters {
                ssrc: Some(ssrc),
                ..Default::default()
            },
            codec: audio_codec.rtp_codec.clone(),
            ..Default::default()
        }],
    ))?);

    let transceiver = pc
        .add_transceiver_from_track(
            Arc::clone(&audio_track) as Arc<dyn TrackLocal>,
            Some(RTCRtpTransceiverInit {
                direction: RTCRtpTransceiverDirection::Sendonly,
                ..Default::default()
            }),
        )
        .await?;

    let offer = pc.create_offer(None).await?;
    pc.set_local_description(offer).await?;
    wait_gathering_complete(gathering_rx.clone()).await?;
    let local = pc
        .local_description()
        .await
        .context("missing local description after gathering")?;

    let session: SessionResponse = post_json(
        &http,
        &format!("{BASE_URL}/session"),
        serde_json::json!({ "sdp": local.sdp }),
    )
    .await
    .context("POST /session")?;
    println!("session {}", session.session_id);

    pc.set_remote_description(RTCSessionDescription::answer(
        session.session_description.sdp,
    )?)
    .await?;
    wait_ice_connected(ice_rx).await?;
    println!("ICE connected");

    let offer = pc.create_offer(None).await?;
    pc.set_local_description(offer).await?;
    wait_gathering_complete(gathering_rx).await?;
    let local = pc
        .local_description()
        .await
        .context("missing local description for tracks/send")?;
    let mid = transceiver.mid().await?.context("transceiver has no mid")?;

    let send: SendTrackResponse = post_json(
        &http,
        &format!("{BASE_URL}/tracks/send"),
        serde_json::json!({
            "sessionId": session.session_id,
            "sdp": local.sdp,
            "track": {
                "location": "local",
                "mid": mid,
                "trackName": track_name,
            }
        }),
    )
    .await
    .context("POST /tracks/send")?;

    pc.set_remote_description(RTCSessionDescription::answer(send.session_description.sdp)?)
        .await?;

    let sender = transceiver
        .sender()
        .await?
        .context("transceiver has no sender")?;
    let payload_type = negotiated_payload_type(&sender).await?;
    println!("publishing track {track_name} mid={mid} pt={payload_type} ssrc={ssrc}");

    let sdp_lock = Arc::new(Mutex::new(()));
    connect_prox_websocket(
        http,
        Arc::clone(&pc),
        sdp_lock,
        session.session_id,
        track_name,
        intercom,
    )
    .await?;

    Ok(PublishedTrack {
        track: audio_track,
        ssrc,
        payload_type,
        _pc: pc,
    })
}

async fn post_json<T: for<'de> Deserialize<'de>>(
    http: &reqwest::Client,
    url: &str,
    body: serde_json::Value,
) -> Result<T> {
    let text = post_text(http, url, body).await?;
    serde_json::from_str(&text).with_context(|| format!("parsing {url} response: {text}"))
}

async fn post_text(http: &reqwest::Client, url: &str, body: serde_json::Value) -> Result<String> {
    let response = http
        .post(url)
        .header("content-type", "application/json")
        .json(&body)
        .send()
        .await?;
    let status = response.status();
    let text = response.text().await?;
    if !status.is_success() {
        bail!("{url} failed ({status}): {text}");
    }
    Ok(text)
}

async fn wait_gathering_complete(mut rx: watch::Receiver<RTCIceGatheringState>) -> Result<()> {
    if *rx.borrow() == RTCIceGatheringState::Complete {
        return Ok(());
    }
    tokio::time::timeout(
        Duration::from_secs(10),
        rx.wait_for(|state| *state == RTCIceGatheringState::Complete),
    )
    .await
    .context("ICE gathering timed out")?
    .context("ICE gathering watch closed")?;
    Ok(())
}

async fn wait_ice_connected(mut rx: watch::Receiver<RTCIceConnectionState>) -> Result<()> {
    if matches!(
        *rx.borrow(),
        RTCIceConnectionState::Connected | RTCIceConnectionState::Completed
    ) {
        return Ok(());
    }
    tokio::time::timeout(
        Duration::from_secs(5),
        rx.wait_for(|state| {
            matches!(
                state,
                RTCIceConnectionState::Connected | RTCIceConnectionState::Completed
            )
        }),
    )
    .await
    .context("ICE connect timeout")?
    .context("ICE connection watch closed")?;
    Ok(())
}

async fn negotiated_payload_type(sender: &Arc<dyn RtpSender>) -> Result<u8> {
    sender
        .get_parameters()
        .await?
        .rtp_parameters
        .codecs
        .first()
        .map(|codec| codec.payload_type)
        .context("sender has no negotiated codec")
}

async fn connect_prox_websocket(
    http: reqwest::Client,
    pc: Arc<dyn PeerConnection>,
    sdp_lock: Arc<Mutex<()>>,
    session_id: String,
    track_id: String,
    intercom: IntercomControl,
) -> Result<()> {
    let mut url = reqwest::Url::parse(&format!("{BASE_URL}/websocket"))?;
    url.set_scheme("wss").ok();
    url.query_pairs_mut()
        .append_pair("sessionId", &session_id)
        .append_pair("trackId", &track_id)
        .append_pair("remote", REMOTE);

    let (ws, _) = tokio_tungstenite::connect_async(url.as_str())
        .await
        .context("websocket connect")?;
    let (mut sink, mut stream) = ws.split();

    sink.send(Message::Text(
        serde_json::json!({
            "command": "set_path",
            "path": SERVER_PATH,
            "prettyPath": SERVER_PATH
        })
        .to_string()
        .into(),
    ))
    .await?;
    sink.send(Message::Text(
        serde_json::json!({
            "command": "set_name",
            "name": "ha-server"
        })
        .to_string()
        .into(),
    ))
    .await?;
    println!("websocket joined remote {REMOTE} at {SERVER_PATH}");

    tokio::spawn(async move {
        let mut subscribed = HashSet::<String>::new();
        while let Some(msg) = stream.next().await {
            match msg {
                Ok(Message::Text(text)) => {
                    if let Err(err) = handle_ws_text(
                        &text,
                        &http,
                        &pc,
                        &sdp_lock,
                        &session_id,
                        &intercom,
                        &mut subscribed,
                    )
                    .await
                    {
                        eprintln!("websocket handler error: {err:#}");
                    }
                }
                Ok(Message::Close(frame)) => fatal(format!("websocket closed: {frame:?}")),
                Err(err) => fatal(format!("websocket error: {err}")),
                _ => {}
            }
        }
        fatal("websocket disconnected".into());
    });

    Ok(())
}

async fn handle_ws_text(
    text: &str,
    http: &reqwest::Client,
    pc: &Arc<dyn PeerConnection>,
    sdp_lock: &Mutex<()>,
    session_id: &str,
    intercom: &IntercomControl,
    subscribed: &mut HashSet<String>,
) -> Result<()> {
    let message: ActiveSessionsMessage = match serde_json::from_str(text) {
        Ok(message) => message,
        Err(_) => return Ok(()),
    };
    if message.command != "active_sessions" {
        return Ok(());
    }

    intercom.set_mode(mode_from_sessions(session_id, &message.sessions));

    let mut tracks = Vec::new();
    let mut new_ids = Vec::new();
    for session in &message.sessions {
        if session.id == session_id {
            continue;
        }
        if !is_intercom_client_path(&session.path) {
            continue;
        }
        if subscribed.contains(&session.track_id) {
            continue;
        }
        println!("subscribing to {} path={}", session.track_id, session.path);
        new_ids.push(session.track_id.clone());
        tracks.push(serde_json::json!({
            "location": "remote",
            "sessionId": session.id,
            "trackName": session.track_id,
        }));
    }

    if !tracks.is_empty() {
        receive_tracks(http, pc, sdp_lock, session_id, tracks).await?;
        subscribed.extend(new_ids);
    }
    Ok(())
}

fn fatal(message: String) -> ! {
    eprintln!("{message}");
    std::process::exit(1);
}

fn is_intercom_client_path(path: &str) -> bool {
    path == LISTEN_PATH || path == TALK_PATH
}

fn mode_from_sessions(self_id: &str, sessions: &[ProxSession]) -> Mode {
    let mut listen = false;
    let mut talk = false;
    for session in sessions {
        if session.id == self_id {
            continue;
        }
        if session.path == TALK_PATH {
            talk = true;
        } else if session.path == LISTEN_PATH {
            listen = true;
        }
    }
    if talk {
        Mode::Talk
    } else if listen {
        Mode::Listen
    } else {
        Mode::Idle
    }
}

async fn receive_tracks(
    http: &reqwest::Client,
    pc: &Arc<dyn PeerConnection>,
    sdp_lock: &Mutex<()>,
    session_id: &str,
    tracks: Vec<serde_json::Value>,
) -> Result<()> {
    let _guard = sdp_lock.lock().await;
    let receive: ReceiveTracksResponse = post_json(
        http,
        &format!("{BASE_URL}/tracks/receive"),
        serde_json::json!({
            "sessionId": session_id,
            "tracks": tracks,
        }),
    )
    .await
    .context("POST /tracks/receive")?;

    pc.set_remote_description(RTCSessionDescription::offer(
        receive.session_description.sdp,
    )?)
    .await?;
    let answer = pc.create_answer(None).await?;
    pc.set_local_description(answer).await?;
    let local = pc
        .local_description()
        .await
        .context("missing local description after receive answer")?;

    post_text(
        http,
        &format!("{BASE_URL}/renegotiate"),
        serde_json::json!({
            "sessionId": session_id,
            "sdp": local.sdp,
        }),
    )
    .await
    .context("POST /renegotiate")?;
    Ok(())
}
