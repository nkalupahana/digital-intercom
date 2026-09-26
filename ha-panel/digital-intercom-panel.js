const DEFAULTS = {
  base_url: "https://prox.nisa.la",
  remote: "github.com/nkalupahana/digital-intercom.git",
  listen_path: "/intercom/listen",
  talk_path: "/intercom/talk",
};

class DigitalIntercomPanel extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: "open" });
    this._hass = null;
    this._narrow = false;
    this._route = null;
    this._panel = null;
    this._generation = 0;
    this._talking = false;
    this._ready = false;
    this._ws = null;
    this._pc = null;
    this._micTrack = null;
    this._sessionId = null;
    this._path = DEFAULTS.listen_path;
    this._pendingStreamIdToTrackId = {};
    this._activeTracks = {};
    this._receiveQueue = Promise.resolve();
    this._statusEl = null;
    this._ptt = null;
    this._audios = null;
    this._onPointerDown = (event) => {
      event.preventDefault();
      this._ptt.setPointerCapture(event.pointerId);
      this._setTalking(true);
    };
    this._onPointerUp = () => this._setTalking(false);
    this._preventSelect = (event) => event.preventDefault();
    this._onKeyDown = (event) => {
      if (event.code === "Space" && !event.repeat) {
        event.preventDefault();
        this._setTalking(true);
      }
    };
    this._onKeyUp = (event) => {
      if (event.code === "Space") {
        event.preventDefault();
        this._setTalking(false);
      }
    };
    this._onBlur = () => this._setTalking(false);
  }

  set hass(hass) {
    this._hass = hass;
  }

  get hass() {
    return this._hass;
  }

  set narrow(narrow) {
    this._narrow = narrow;
  }

  get narrow() {
    return this._narrow;
  }

  set route(route) {
    this._route = route;
  }

  get route() {
    return this._route;
  }

  set panel(panel) {
    this._panel = panel;
  }

  get panel() {
    return this._panel;
  }

  _config() {
    const config = this._panel?.config || {};
    return {
      base_url: config.base_url || DEFAULTS.base_url,
      remote: config.remote || DEFAULTS.remote,
      listen_path: config.listen_path || DEFAULTS.listen_path,
      talk_path: config.talk_path || DEFAULTS.talk_path,
    };
  }

  connectedCallback() {
    const generation = ++this._generation;
    this._render();
    this._bindUi();
    this._connect(generation).catch((err) => {
      if (generation !== this._generation) return;
      console.error(err);
      this._setStatus(err.message || String(err), "error");
    });
  }

  disconnectedCallback() {
    this._generation += 1;
    this._teardown();
  }

  _render() {
    const { listen_path, talk_path } = this._config();
    this.shadowRoot.innerHTML = `
      <style>
        :host {
          color-scheme: dark;
          --text: #f2f4f8;
          --muted: #9aa3b2;
          --listen: #3d8bfd;
          --talk: #e35d5d;
          --ok: #3ecf8e;
          box-sizing: border-box;
          display: block;
          height: 100%;
          min-height: 100%;
          font-family: ui-sans-serif, system-ui, sans-serif;
          background: transparent;
          color: var(--text);
        }
        * { box-sizing: border-box; }
        .wrap {
          min-height: 100%;
          display: grid;
          place-items: center;
          padding: 1rem;
        }
        main {
          width: min(28rem, calc(100vw - 2rem));
          padding: 1.75rem;
        }
        h1 { margin: 0 0 0.35rem; font-size: 1.4rem; }
        p { margin: 0; color: var(--muted); line-height: 1.45; }
        #status {
          margin: 1.25rem 0 1.5rem;
          font-weight: 600;
          color: var(--listen);
        }
        #status.error { color: var(--talk); }
        #status.talk { color: var(--talk); }
        #status.ok { color: var(--ok); }
        button {
          width: 100%;
          border: 0;
          border-radius: 999px;
          padding: 1.15rem 1rem;
          font: inherit;
          font-size: 1.1rem;
          font-weight: 700;
          color: white;
          background: var(--listen);
          cursor: pointer;
          -webkit-user-select: none;
          user-select: none;
          -webkit-touch-callout: none;
          -webkit-tap-highlight-color: transparent;
          touch-action: none;
        }
        button:disabled {
          opacity: 0.45;
          cursor: not-allowed;
        }
        button.talking { background: var(--talk); }
        #audios { display: none; }
      </style>
      <div class="wrap">
        <main>
          <h1>Digital Intercom</h1>
          <div id="status">Connecting…</div>
          <button id="ptt" type="button" disabled>Push to talk</button>
          <div id="audios"></div>
        </main>
      </div>
    `;
  }

  _bindUi() {
    this._statusEl = this.shadowRoot.getElementById("status");
    this._ptt = this.shadowRoot.getElementById("ptt");
    this._audios = this.shadowRoot.getElementById("audios");
    this._ptt.addEventListener("pointerdown", this._onPointerDown);
    this._ptt.addEventListener("pointerup", this._onPointerUp);
    this._ptt.addEventListener("pointercancel", this._onPointerUp);
    this._ptt.addEventListener("selectstart", this._preventSelect);
    this._ptt.addEventListener("contextmenu", this._preventSelect);
    window.addEventListener("keydown", this._onKeyDown);
    window.addEventListener("keyup", this._onKeyUp);
    window.addEventListener("blur", this._onBlur);
  }

  _setStatus(text, kind = "") {
    if (!this._statusEl) return;
    this._statusEl.textContent = text;
    this._statusEl.className = kind;
  }

  _getPathDistance(path1, path2) {
    const segments1 = path1.split("/").filter(Boolean);
    const segments2 = path2.split("/").filter(Boolean);
    let commonDepth = 0;
    while (
      commonDepth < segments1.length &&
      commonDepth < segments2.length &&
      segments1[commonDepth] === segments2[commonDepth]
    ) {
      commonDepth++;
    }
    const stepsUp = segments1.length - commonDepth;
    const stepsDown = segments2.length - commonDepth;
    return Math.max(stepsUp + stepsDown - 1, 0);
  }

  _getVolume(path1, path2) {
    const distance = this._getPathDistance(path1, path2);
    return { 0: 1, 1: 0.5, 2: 0.1 }[distance] ?? 0;
  }

  _setPath(nextPath) {
    this._path = nextPath;
    if (!this._ws || this._ws.readyState !== WebSocket.OPEN) return;
    this._ws.send(
      JSON.stringify({
        command: "set_path",
        path: nextPath,
        prettyPath: nextPath,
      }),
    );
  }

  _setTalking(next) {
    if (!this._ready || this._talking === next) return;
    this._talking = next;
    if (this._micTrack) this._micTrack.enabled = next;
    const { listen_path, talk_path } = this._config();
    this._setPath(next ? talk_path : listen_path);
    this._ptt.classList.toggle("talking", next);
    this._ptt.textContent = next ? "Talking…" : "Push to talk";
    this._setStatus(next ? "Talking" : "Listening", next ? "talk" : "ok");
    for (const audio of Object.values(this._activeTracks)) {
      audio.muted = next;
    }
  }

  _waitIceConnected(pc) {
    if (
      pc.iceConnectionState === "connected" ||
      pc.iceConnectionState === "completed"
    ) {
      return Promise.resolve();
    }
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(
        () => reject(new Error("ICE connect timeout")),
        5000,
      );
      const onChange = () => {
        if (
          pc.iceConnectionState === "connected" ||
          pc.iceConnectionState === "completed"
        ) {
          clearTimeout(timeout);
          pc.removeEventListener("iceconnectionstatechange", onChange);
          resolve();
        } else if (pc.iceConnectionState === "failed") {
          clearTimeout(timeout);
          pc.removeEventListener("iceconnectionstatechange", onChange);
          reject(new Error("ICE failed"));
        }
      };
      pc.addEventListener("iceconnectionstatechange", onChange);
    });
  }

  async _postJson(path, body) {
    const { base_url } = this._config();
    const response = await fetch(`${base_url}${path}`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
    });
    const text = await response.text();
    if (!response.ok) {
      throw new Error(`${path} failed (${response.status}): ${text}`);
    }
    return text ? JSON.parse(text) : {};
  }

  _enqueueReceive(work) {
    this._receiveQueue = this._receiveQueue.then(work).catch((err) => {
      console.error(err);
      this._setStatus(err.message, "error");
    });
    return this._receiveQueue;
  }

  _stale(generation) {
    return generation !== this._generation;
  }

  async _connect(generation) {
    const { base_url, remote, listen_path } = this._config();
    this._path = listen_path;

    const pc = new RTCPeerConnection({
      iceServers: [{ urls: "stun:stun.cloudflare.com:3478" }],
      bundlePolicy: "max-bundle",
    });
    this._pc = pc;

    const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
    if (this._stale(generation)) {
      for (const track of stream.getTracks()) track.stop();
      pc.close();
      return;
    }

    this._micTrack = stream.getAudioTracks()[0];
    this._micTrack.enabled = false;
    const transceiver = pc.addTransceiver(this._micTrack, {
      direction: "sendonly",
    });

    await pc.setLocalDescription(await pc.createOffer());
    if (this._stale(generation)) return;
    const session = await this._postJson("/session", {
      sdp: pc.localDescription.sdp,
    });
    if (this._stale(generation)) return;
    this._sessionId = session.sessionId;
    await pc.setRemoteDescription(session.sessionDescription);
    await this._waitIceConnected(pc);
    if (this._stale(generation)) return;

    await pc.setLocalDescription(await pc.createOffer());
    if (this._stale(generation)) return;
    const trackName = this._micTrack.id;
    const send = await this._postJson("/tracks/send", {
      sessionId: this._sessionId,
      sdp: pc.localDescription.sdp,
      track: {
        location: "local",
        mid: transceiver.mid,
        trackName,
      },
    });
    if (this._stale(generation)) return;
    await pc.setRemoteDescription(send.sessionDescription);

    pc.ontrack = (event) => {
      if (this._stale(generation)) return;
      const remoteStream = event.streams[0];
      const trackId = this._pendingStreamIdToTrackId[remoteStream.id];
      if (!trackId || this._activeTracks[trackId]) return;
      const audio = new Audio();
      audio.srcObject = remoteStream;
      audio.autoplay = true;
      audio.muted = this._talking;
      this._activeTracks[trackId] = audio;
      this._audios.appendChild(audio);
    };

    const wsParams = new URLSearchParams({
      sessionId: this._sessionId,
      trackId: trackName,
      remote,
    });
    const wsBase = base_url.replace(/^https:/, "wss:").replace(/^http:/, "ws:");
    const ws = new WebSocket(`${wsBase}/websocket?${wsParams}`);
    this._ws = ws;
    ws.onopen = () => {
      if (this._stale(generation) || this._ws !== ws) return;
      ws.send(
        JSON.stringify({
          command: "set_path",
          path: listen_path,
          prettyPath: listen_path,
        }),
      );
      ws.send(JSON.stringify({ command: "set_name", name: "intercom-web" }));
      this._ready = true;
      this._ptt.disabled = false;
      this._setStatus("Listening", "ok");
    };
    ws.onerror = () => {
      if (this._stale(generation) || this._ws !== ws) return;
      this._setStatus("Lost connection with Prox", "error");
    };
    ws.onclose = () => {
      if (this._stale(generation) || this._ws !== ws) return;
      this._ready = false;
      if (this._ptt) this._ptt.disabled = true;
      this._setStatus("Disconnected", "error");
    };
    ws.onmessage = (ev) => {
      if (this._stale(generation) || this._ws !== ws) return;
      let message;
      try {
        message = JSON.parse(ev.data);
      } catch {
        return;
      }
      if (message.command !== "active_sessions") return;
      this._enqueueReceive(() =>
        this._subscribeSessions(pc, message.sessions || [], generation),
      );
    };
  }

  async _subscribeSessions(pc, sessions, generation) {
    if (this._stale(generation)) return;
    const tracksToConnect = [];
    for (const session of sessions) {
      if (session.id === this._sessionId) continue;
      if (session.trackId in this._activeTracks) continue;
      if (this._getVolume(session.path, this._path) === 0) continue;
      tracksToConnect.push({
        location: "remote",
        sessionId: session.id,
        trackName: session.trackId,
      });
    }
    if (!tracksToConnect.length) return;

    const receive = await this._postJson("/tracks/receive", {
      sessionId: this._sessionId,
      tracks: tracksToConnect,
    });
    if (this._stale(generation)) return;
    Object.assign(
      this._pendingStreamIdToTrackId,
      receive.streamIdToTrackId || {},
    );
    await pc.setRemoteDescription(receive.sessionDescription);
    await pc.setLocalDescription(await pc.createAnswer());
    if (this._stale(generation)) return;
    await this._postJson("/renegotiate", {
      sessionId: this._sessionId,
      sdp: pc.localDescription.sdp,
    });
  }

  _teardown() {
    this._ready = false;
    this._talking = false;
    window.removeEventListener("keydown", this._onKeyDown);
    window.removeEventListener("keyup", this._onKeyUp);
    window.removeEventListener("blur", this._onBlur);
    if (this._ptt) {
      this._ptt.removeEventListener("pointerdown", this._onPointerDown);
      this._ptt.removeEventListener("pointerup", this._onPointerUp);
      this._ptt.removeEventListener("pointercancel", this._onPointerUp);
      this._ptt.removeEventListener("selectstart", this._preventSelect);
      this._ptt.removeEventListener("contextmenu", this._preventSelect);
    }
    if (this._ws) {
      this._ws.onopen = null;
      this._ws.onerror = null;
      this._ws.onclose = null;
      this._ws.onmessage = null;
      if (
        this._ws.readyState === WebSocket.OPEN ||
        this._ws.readyState === WebSocket.CONNECTING
      ) {
        this._ws.close();
      }
      this._ws = null;
    }
    if (this._pc) {
      this._pc.ontrack = null;
      this._pc.close();
      this._pc = null;
    }
    if (this._micTrack) {
      this._micTrack.stop();
      this._micTrack = null;
    }
    for (const audio of Object.values(this._activeTracks)) {
      audio.pause();
      audio.srcObject = null;
      audio.remove();
    }
    this._activeTracks = {};
    this._pendingStreamIdToTrackId = {};
    this._sessionId = null;
    this._statusEl = null;
    this._ptt = null;
    this._audios = null;
    this._receiveQueue = Promise.resolve();
  }
}

customElements.define("digital-intercom-panel", DigitalIntercomPanel);
