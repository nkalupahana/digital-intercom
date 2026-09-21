use anyhow::{Context, Result};
use std::net::{IpAddr, SocketAddr};
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream, UdpSocket};
use tokio::sync::{mpsc, watch};

use crate::home_assistant::HomeAssistant;

pub const TCP_PORT: u16 = 9998;
pub const LISTEN_UDP_PORT: u16 = 9999;
pub const TALK_UDP_PORT: u16 = 9997;
const HEARTBEAT_INTERVAL: Duration = Duration::from_secs(1);
const UDP_PACKET_SIZE: usize = 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    Idle,
    Listen,
    Talk,
}

impl Mode {
    fn command(self) -> u8 {
        match self {
            Mode::Idle => b'S',
            Mode::Listen => b'L',
            Mode::Talk => b'T',
        }
    }

    fn name(self) -> &'static str {
        match self {
            Mode::Idle => "idle",
            Mode::Listen => "listen",
            Mode::Talk => "talk",
        }
    }
}

#[derive(Clone)]
pub struct IntercomControl {
    desired_mode: watch::Sender<Mode>,
    mode: watch::Receiver<Mode>,
    peer_ip: watch::Receiver<Option<IpAddr>>,
    unlock: mpsc::Sender<()>,
    talk: Arc<UdpSocket>,
    talk_errors: Arc<AtomicU32>,
}

pub struct Intercom {
    pub control: IntercomControl,
    pub listen_rx: mpsc::Receiver<Vec<u8>>,
}

impl IntercomControl {
    pub fn set_mode(&self, mode: Mode) {
        let _ = self.desired_mode.send_if_modified(|current| {
            if *current == mode {
                false
            } else {
                println!("intercom mode {} -> {}", current.name(), mode.name());
                *current = mode;
                true
            }
        });
    }

    pub fn mode(&self) -> Mode {
        *self.mode.borrow()
    }

    pub fn is_connected(&self) -> bool {
        self.peer_ip.borrow().is_some()
    }

    pub async fn unlock(&self) -> Result<()> {
        self.unlock.send(()).await.context("unlock channel closed")
    }

    pub async fn send_talk(&self, data: &[u8]) -> Result<()> {
        if self.mode() != Mode::Talk {
            return Ok(());
        }
        let Some(ip) = *self.peer_ip.borrow() else {
            return Ok(());
        };
        let dest = SocketAddr::new(ip, TALK_UDP_PORT);
        match self.talk.send_to(data, dest).await {
            Ok(_) => {
                self.talk_errors.store(0, Ordering::Relaxed);
                Ok(())
            }
            Err(err) => {
                let n = self.talk_errors.fetch_add(1, Ordering::Relaxed) + 1;
                if n <= 3 || n.is_multiple_of(100) {
                    eprintln!("talk UDP error ({n}): send to {dest}: {err}");
                }
                Err(err).with_context(|| format!("udp talk send to {dest}"))
            }
        }
    }
}

impl Intercom {
    pub async fn start(ha: HomeAssistant) -> Result<Self> {
        let tcp = TcpListener::bind(("0.0.0.0", TCP_PORT))
            .await
            .with_context(|| format!("bind TCP {TCP_PORT}"))?;
        let listen = UdpSocket::bind(("0.0.0.0", LISTEN_UDP_PORT))
            .await
            .with_context(|| format!("bind UDP {LISTEN_UDP_PORT}"))?;
        let talk = Arc::new(
            UdpSocket::bind("0.0.0.0:0")
                .await
                .context("bind UDP talk socket")?,
        );

        let (desired_tx, desired_rx) = watch::channel(Mode::Idle);
        let (peer_ip_tx, peer_ip_rx) = watch::channel(None);
        let (listen_tx, listen_rx) = mpsc::channel::<Vec<u8>>(32);
        let (unlock_tx, unlock_rx) = mpsc::channel(8);

        println!("TCP control listening on 0.0.0.0:{TCP_PORT}");
        println!("UDP listen on 0.0.0.0:{LISTEN_UDP_PORT}");

        tokio::spawn(tcp_loop(tcp, desired_rx, peer_ip_tx, ha, unlock_rx));
        tokio::spawn(udp_listen_loop(listen, listen_tx));

        Ok(Self {
            control: IntercomControl {
                desired_mode: desired_tx.clone(),
                mode: desired_tx.subscribe(),
                peer_ip: peer_ip_rx,
                unlock: unlock_tx,
                talk,
                talk_errors: Arc::new(AtomicU32::new(0)),
            },
            listen_rx,
        })
    }
}

async fn tcp_loop(
    listener: TcpListener,
    mut desired: watch::Receiver<Mode>,
    peer_ip: watch::Sender<Option<IpAddr>>,
    ha: HomeAssistant,
    mut unlock: mpsc::Receiver<()>,
) {
    let mut writer: Option<tokio::net::tcp::OwnedWriteHalf> = None;
    let mut reader: Option<tokio::net::tcp::OwnedReadHalf> = None;
    let mut last_sent: Option<Mode> = None;
    let mut heartbeat = tokio::time::interval(HEARTBEAT_INTERVAL);
    heartbeat.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    let mut buf = [0u8; 256];

    loop {
        tokio::select! {
            accept = listener.accept() => {
                match accept {
                    Ok((stream, addr)) => {
                        if writer.is_some() {
                            println!("replacing existing intercom TCP connection");
                        }
                        let desired_mode = *desired.borrow();
                        attach_client(
                            stream,
                            addr,
                            &mut writer,
                            &mut reader,
                            &peer_ip,
                            desired_mode,
                            &mut last_sent,
                        )
                        .await;
                    }
                    Err(err) => eprintln!("TCP accept error: {err}"),
                }
            }
            _ = heartbeat.tick() => {
                if let Some(w) = writer.as_mut()
                    && w.write_all(b"H").await.is_err()
                {
                    drop_client(&mut writer, &mut reader, &peer_ip);
                }
            }
            changed = desired.changed() => {
                if changed.is_err() {
                    break;
                }
                let mode = *desired.borrow();
                send_mode(writer.as_mut(), mode, &mut last_sent).await;
                if writer.is_none() {
                    eprintln!("cannot send {} — no intercom TCP client", mode.name());
                }
            }
            msg = unlock.recv() => {
                if msg.is_none() {
                    break;
                }
                if let Some(w) = writer.as_mut() {
                    if w.write_all(b"D").await.is_err() {
                        drop_client(&mut writer, &mut reader, &peer_ip);
                    } else {
                        println!("sent command D (unlock)");
                    }
                } else {
                    eprintln!("cannot send unlock — no intercom TCP client");
                }
            }
            n = read_client(&mut reader, &mut buf), if reader.is_some() => {
                match n {
                    Ok(0) => {
                        println!("intercom TCP disconnected");
                        drop_client(&mut writer, &mut reader, &peer_ip);
                    }
                    Ok(n) => handle_intercom_bytes(&buf[..n], &ha),
                    Err(err) => {
                        eprintln!("intercom TCP read error: {err}");
                        drop_client(&mut writer, &mut reader, &peer_ip);
                    }
                }
            }
        }
    }
}

async fn attach_client(
    stream: TcpStream,
    addr: SocketAddr,
    writer: &mut Option<tokio::net::tcp::OwnedWriteHalf>,
    reader: &mut Option<tokio::net::tcp::OwnedReadHalf>,
    peer_ip: &watch::Sender<Option<IpAddr>>,
    desired: Mode,
    last_sent: &mut Option<Mode>,
) {
    let _ = stream.set_nodelay(true);
    let (r, w) = stream.into_split();
    *reader = Some(r);
    *writer = Some(w);
    let ip = canonical_ip(addr);
    let _ = peer_ip.send(Some(ip));
    println!("intercom connected from {ip}");
    *last_sent = None;
    send_mode(writer.as_mut(), desired, last_sent).await;
}

fn drop_client(
    writer: &mut Option<tokio::net::tcp::OwnedWriteHalf>,
    reader: &mut Option<tokio::net::tcp::OwnedReadHalf>,
    peer_ip: &watch::Sender<Option<IpAddr>>,
) {
    *writer = None;
    *reader = None;
    let _ = peer_ip.send(None);
}

async fn send_mode(
    writer: Option<&mut tokio::net::tcp::OwnedWriteHalf>,
    mode: Mode,
    last_sent: &mut Option<Mode>,
) {
    if *last_sent == Some(mode) {
        return;
    }
    let Some(w) = writer else {
        return;
    };
    if w.write_all(&[mode.command()]).await.is_err() {
        return;
    }
    *last_sent = Some(mode);
    println!("sent command {} ({})", mode.command() as char, mode.name());
}

async fn read_client(
    reader: &mut Option<tokio::net::tcp::OwnedReadHalf>,
    buf: &mut [u8],
) -> std::io::Result<usize> {
    match reader.as_mut() {
        Some(r) => r.read(buf).await,
        None => std::future::pending().await,
    }
}

fn handle_intercom_bytes(data: &[u8], ha: &HomeAssistant) {
    if data.is_empty() {
        return;
    }
    let kind = data[0] as char;
    match kind {
        'B' => {
            println!("intercom event: buzzer");
            let ha = ha.clone();
            tokio::spawn(async move {
                ha.pulse_doorbell().await;
            });
        }
        'C' => println!("intercom event: credit card ({} bytes)", data.len()),
        'D' => println!("intercom event: digital id"),
        other => println!("intercom event: {other:?} ({} bytes)", data.len()),
    }
}

fn canonical_ip(addr: SocketAddr) -> IpAddr {
    match addr.ip() {
        IpAddr::V6(v6) => v6
            .to_ipv4_mapped()
            .map(IpAddr::V4)
            .unwrap_or(IpAddr::V6(v6)),
        ip => ip,
    }
}

async fn udp_listen_loop(sock: UdpSocket, tx: mpsc::Sender<Vec<u8>>) {
    let mut buf = vec![0u8; UDP_PACKET_SIZE * 2];
    loop {
        match sock.recv_from(&mut buf).await {
            Ok((n, _)) => {
                let _ = tx.try_send(buf[..n].to_vec());
            }
            Err(err) => eprintln!("UDP listen error: {err}"),
        }
    }
}
