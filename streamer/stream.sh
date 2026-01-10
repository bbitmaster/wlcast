#!/bin/bash
# wlcast streamer - fastest mode (OpenCL + HW JPEG)
# Usage: ./stream.sh <destination_ip> [quality]
#   quality: 1-100 (default: 80)

DEST="${1:-}"
QUALITY="${2:-80}"

if [ -z "$DEST" ]; then
    echo "Usage: $0 <destination_ip> [quality]"
    echo "  quality: 1-100 (default: 80)"
    echo ""
    echo "Example: $0 192.168.50.82"
    echo "         $0 192.168.50.82 90"
    exit 1
fi

export XDG_RUNTIME_DIR=/run/0-runtime-dir
export WAYLAND_DISPLAY=wayland-1

exec /storage/wlcast/wlcast-stream \
    --dest "$DEST" \
    --port 7723 \
    --quality "$QUALITY" \
    --opencl
