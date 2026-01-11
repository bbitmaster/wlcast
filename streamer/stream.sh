#!/bin/bash
# wlcast streamer - fastest mode (OpenCL + HW JPEG)
# Usage: ./stream.sh <destination_ip> [quality] [target-fps] [--audio]
#   quality: 1-100 (default: 80)
#   target-fps: enable adaptive quality to hit target FPS (default: 0=off)
#   --audio: enable audio streaming

DEST="${1:-}"
QUALITY="${2:-80}"
TARGET_FPS="${3:-0}"
AUDIO_FLAG=""

# Check for --audio anywhere in args
for arg in "$@"; do
    if [ "$arg" = "--audio" ]; then
        AUDIO_FLAG="--audio"
    fi
done

if [ -z "$DEST" ]; then
    echo "Usage: $0 <destination_ip> [quality] [target-fps] [--audio]"
    echo "  quality: 1-100 (default: 80)"
    echo "  target-fps: adaptive quality target (0=off, try 55)"
    echo "  --audio: enable audio streaming"
    echo ""
    echo "Example: $0 192.168.50.82"
    echo "         $0 192.168.50.82 90"
    echo "         $0 192.168.50.82 80 55   # adaptive quality targeting 55fps"
    echo "         $0 192.168.50.82 80 55 --audio   # with audio"
    exit 1
fi

export XDG_RUNTIME_DIR=/run/0-runtime-dir
export WAYLAND_DISPLAY=wayland-1

ARGS="--dest $DEST --port 7723 --quality $QUALITY --opencl"

if [ "$TARGET_FPS" != "0" ]; then
    ARGS="$ARGS --target-fps $TARGET_FPS"
fi

if [ -n "$AUDIO_FLAG" ]; then
    ARGS="$ARGS --audio"
fi

exec /storage/wlcast/wlcast-stream $ARGS
