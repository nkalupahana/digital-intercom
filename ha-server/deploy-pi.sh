#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
HOST="${HA_SERVER_HOST:-hassio@homeassistant.local}"
DEST="${HA_SERVER_DEST:-/root/intercom/ha-server}"
IMAGE="${HA_SERVER_IMAGE:-rust:bookworm}"

if ! docker info >/dev/null 2>&1; then
  if [[ "$(uname -s)" == "Darwin" && -d /Applications/Docker.app ]]; then
    echo "starting Docker Desktop..."
    open /Applications/Docker.app
    for _ in $(seq 1 60); do
      docker info >/dev/null 2>&1 && break
      sleep 2
    done
  fi
fi
if ! docker info >/dev/null 2>&1; then
  echo "docker is not running" >&2
  exit 1
fi

DIST="$(mktemp -d)"
trap 'rm -rf "$DIST"' EXIT

echo "building static aarch64 musl binary..."
docker run --rm --platform linux/arm64 \
  -v "$ROOT:/src:ro" \
  -v ha-server-cargo-registry:/usr/local/cargo/registry \
  -v ha-server-cargo-git:/usr/local/cargo/git \
  -v ha-server-linux-target:/out \
  -v "$DIST:/dist" \
  -e CARGO_TARGET_DIR=/out \
  -e CARGO_HOME=/usr/local/cargo \
  -e CC=musl-gcc \
  -e CXX=musl-g++ \
  -e CC_aarch64_unknown_linux_musl=musl-gcc \
  -e CXX_aarch64_unknown_linux_musl=musl-g++ \
  -e CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_LINKER=musl-gcc \
  -e LIBOPUS_STATIC=1 \
  -e OPUS_STATIC=1 \
  -e LIBOPUS_NO_PKG=1 \
  -e OPUS_NO_PKG=1 \
  -e RUSTFLAGS='-C target-feature=+crt-static' \
  -w /src \
  "$IMAGE" \
  bash -c '
    set -euo pipefail
    export PATH="/usr/local/cargo/bin:$PATH"
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y musl-tools cmake
    rustup target add aarch64-unknown-linux-musl
    cargo build --release --target aarch64-unknown-linux-musl
    strip /out/aarch64-unknown-linux-musl/release/ha-server
    file /out/aarch64-unknown-linux-musl/release/ha-server
    cp /out/aarch64-unknown-linux-musl/release/ha-server /dist/ha-server
  '

REMOTE_TMP="/tmp/ha-server.new"
echo "copying to $HOST:$DEST..."
ssh "$HOST" "sudo mkdir -p \"$(dirname "$DEST")\""
scp -O "$DIST/ha-server" "$HOST:$REMOTE_TMP"
ssh "$HOST" "sudo mv \"$REMOTE_TMP\" \"$DEST\" && sudo chmod 755 \"$DEST\" && ls -la \"$DEST\""
echo "done"
