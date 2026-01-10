#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STREAMER_BIN="$ROOT_DIR/streamer/wlcast-stream"

DEVICE_IP="${DEVICE_IP:-192.168.50.101}"
DEVICE_PATH="${DEVICE_PATH:-/storage/wlcast}"

if [[ ! -f "$STREAMER_BIN" ]]; then
  echo "Missing streamer binary: $STREAMER_BIN" >&2
  echo "Build it first with: cd $ROOT_DIR/streamer && ./cross-compile.sh OPENCL=1" >&2
  exit 1
fi

ssh "root@$DEVICE_IP" "mkdir -p $DEVICE_PATH"
scp "$STREAMER_BIN" "root@$DEVICE_IP:$DEVICE_PATH/wlcast-stream"

printf "\nDeployed to root@%s:%s/wlcast-stream\n" "$DEVICE_IP" "$DEVICE_PATH"
