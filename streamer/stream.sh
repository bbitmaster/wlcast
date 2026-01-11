#!/bin/bash
# wlcast streamer - fastest mode (OpenCL + HW JPEG)
# Usage: ./stream.sh <destination_ip> [quality] [target-fps]
#   quality: 1-100 (default: 80)
#   target-fps: enable adaptive quality to hit target FPS (default: 0=off)

DEST="${1:-}"
QUALITY="${2:-80}"
TARGET_FPS="${3:-0}"

if [ -z "$DEST" ]; then
    echo "Usage: $0 <destination_ip> [quality] [target-fps]"
    echo "  quality: 1-100 (default: 80)"
    echo "  target-fps: adaptive quality target (0=off, try 55)"
    echo ""
    echo "Example: $0 192.168.50.82"
    echo "         $0 192.168.50.82 90"
    echo "         $0 192.168.50.82 80 55   # adaptive quality targeting 55fps"
    exit 1
fi

export XDG_RUNTIME_DIR=/run/0-runtime-dir
export WAYLAND_DISPLAY=wayland-1

ARGS="--dest $DEST --port 7723 --quality $QUALITY --opencl"

if [ "$TARGET_FPS" != "0" ]; then
    ARGS="$ARGS --target-fps $TARGET_FPS"
fi

exec /storage/wlcast/wlcast-stream $ARGS
