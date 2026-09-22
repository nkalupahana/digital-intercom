# Home Assistant Intercom Panel

Custom sidebar panel that ports the Digital Intercom push-to-talk UI into Home Assistant.

Home Assistant loads this as a [custom panel](https://www.home-assistant.io/integrations/panel_custom/) web component ([developer docs](https://developers.home-assistant.io/docs/frontend/custom-ui/creating-custom-panels/)). The browser talks to Prox directly; Home Assistant does not proxy those calls.

## Install

1. Copy `digital-intercom-panel.js` to Home Assistant `<config>/www/`.
   Files in `www` are served at `/local`.
2. Merge the snippet from `configuration.yaml.example` into your Home Assistant `configuration.yaml`.
3. Restart Home Assistant.
4. Open **Intercom** in the sidebar (`/intercom`). Allow microphone access when prompted.

Use HTTPS (or the Home Assistant app over a trusted connection). `getUserMedia` requires a secure context.

Do not set `embed_iframe: true`. Home Assistant’s panel iframe does not grant microphone permission.

## Config

| Key | Default | Purpose |
| --- | --- | --- |
| `base_url` | `https://prox.nisa.la` | Prox HTTP origin |
| `remote` | `github.com/nkalupahana/digital-intercom.git` | Prox room / remote id |
| `listen_path` | `/intercom/listen` | Path while listening |
| `talk_path` | `/intercom/talk` | Path while talking |
