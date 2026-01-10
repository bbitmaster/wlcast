#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROBE_BIN="$ROOT_DIR/tools/v4l2_probe"

DEVICE_IP="${DEVICE_IP:-192.168.50.101}"
DEVICE_PATH="${DEVICE_PATH:-/storage/wlcast/tools}"

if [[ ! -f "$PROBE_BIN" ]]; then
  echo "Missing probe binary: $PROBE_BIN" >&2
  echo "Build it first" >&2
  exit 1
fi

ssh "root@$DEVICE_IP" "mkdir -p $DEVICE_PATH"
scp "$PROBE_BIN" "root@$DEVICE_IP:$DEVICE_PATH/v4l2_probe"

printf "\nDeployed to root@%s:%s/v4l2_probe\n" "$DEVICE_IP" "$DEVICE_PATH"
