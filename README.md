# wlcast

Low-latency UDP screen streaming for Linux devices running Wayland (wlroots-based compositors).

Captures the screen via `wlr-export-dmabuf`, encodes to JPEG using hardware acceleration, and streams over UDP to a desktop viewer.

## Features

- **Hardware JPEG encoding** via hantro-vpu (Rockchip)
- **GPU color conversion** via OpenCL on Mali GPU (30+ fps)
- **Zero-copy screen capture** via wlr-export-dmabuf protocol
- **Audio streaming** via PulseAudio capture + Opus encoding
- **NEON SIMD fallback** for CPU color conversion
- **Software JPEG fallback** via libturbojpeg
- **Simple UDP protocol** with automatic frame reassembly

## Performance

Tested on Anbernic RG353PS (RK3566, 640x480 display):

| Mode | FPS | CPU Usage |
|------|-----|-----------|
| OpenCL + HW JPEG | 30-31 | ~13% |
| NEON + HW JPEG | 19-20 | ~50% |
| NEON + SW JPEG | 15-16 | ~60% |

## Quick Start

### On Device (Streamer)

```bash
# Easiest way - use the helper script
/storage/stream.sh 192.168.1.100

# With audio streaming
/storage/stream.sh 192.168.1.100 80 55 --audio

# Or manually with all options
XDG_RUNTIME_DIR=/run/0-runtime-dir WAYLAND_DISPLAY=wayland-1 \
  /storage/wlcast/wlcast-stream --dest 192.168.1.100 --opencl --audio
```

### On Desktop (Viewer)

```bash
# Open firewall port first
sudo ufw allow 7723/udp

# Run viewer
./viewer/wlcast-view --port 7723
```

## Building

### Requirements

**Streamer (cross-compile for ARM64):**
- ROCKNIX/similar toolchain with:
  - wayland-client
  - libturbojpeg
  - wayland-scanner
  - libpulse (for audio)
  - libopus (for audio)

**Viewer (native build):**
- SDL2
- libturbojpeg
- libopus (for audio)

### Cross-Compile Streamer (Recommended)

The streamer must be cross-compiled as the target device typically lacks development headers.

```bash
cd streamer

# Set up toolchain path in cross-compile.sh, then:
./cross-compile.sh OPENCL=1

# With audio support:
./cross-compile.sh OPENCL=1 AUDIO=1
```

### Build Viewer

```bash
# Install dependencies (Arch/CachyOS)
sudo pacman -S sdl2 libjpeg-turbo

# Build
cd viewer
make

# With audio support:
make AUDIO=1
```

### Deploy to Device

```bash
rsync -avz streamer/wlcast-stream root@<device-ip>:/storage/wlcast/
rsync -avz streamer/stream.sh root@<device-ip>:/storage/
```

## Command Line Options

### Streamer

```
Usage: wlcast-stream --dest <ip> [options]

Required:
  --dest <ip>        Destination IP address for UDP stream

Optional:
  --port <port>      UDP port (default: 7723)
  --quality <1-100>  JPEG quality (default: 80)
  --fps <limit>      Frame rate limit (default: unlimited)
  --region x y w h   Capture region (default: full screen)
  --hw-jpeg          Use hardware JPEG encoder
  --dmabuf           Use wlr-export-dmabuf for zero-copy capture
  --opencl           Use OpenCL GPU conversion (auto-enables --dmabuf --hw-jpeg)
  --audio            Stream audio (requires AUDIO=1 build)
  --no-cursor        Don't overlay cursor in capture
```

### Viewer

```
Usage: wlcast-view [--port <port>]

  --port <port>      UDP port to listen on (default: 7723)
```

## Project Structure

```
wlcast/
├── streamer/           # Device-side capture and encoding
│   ├── main.c
│   ├── capture.c       # wlr-screencopy capture
│   ├── capture_dmabuf.c # wlr-export-dmabuf capture (zero-copy)
│   ├── opencl_convert.c # GPU color conversion
│   ├── v4l2_jpeg.c     # Hardware JPEG encoder
│   ├── compress.c      # Software JPEG (turbojpeg)
│   ├── audio.c         # PulseAudio capture + Opus encoding
│   ├── udp.c           # UDP fragmentation/sending
│   ├── CL/             # OpenCL headers
│   └── cross-compile.sh
├── viewer/             # Desktop-side receiver
│   ├── main.c
│   ├── network.c       # UDP receive/reassembly
│   ├── decode.c        # JPEG decoding
│   └── audio.c         # Opus decoding + SDL playback
├── common/
│   └── protocol.h      # Shared UDP protocol definition
├── protocol/           # Wayland protocol XML files
│   ├── wlr-screencopy-unstable-v1.xml
│   └── wlr-export-dmabuf-unstable-v1.xml
├── tools/
│   └── v4l2_probe.c    # V4L2 capability scanner
└── scripts/            # Build/deploy helper scripts
```

## Compatibility

### Tested On
- Anbernic RG353PS (RK3566) running ROCKNIX with sway

### Should Work On
- Any RK3566/RK3568 device (same VPU)
- RK3588 devices (may need minor tweaks)
- Any device running a wlroots compositor (sway, wayfire, hyprland)

### Requirements
- wlroots-based Wayland compositor (for wlr-export-dmabuf)
- For OpenCL: Mali GPU with libmali driver
- For HW JPEG: Rockchip hantro-vpu

### Won't Work Without Changes
- GNOME/KDE (use PipeWire for screen capture)
- X11 (needs different capture method)
- Non-Rockchip devices (need different V4L2 encoder)

## Protocol

UDP packets use a simple header for frame reassembly:

```c
struct wlcast_udp_header {
  uint32_t magic;       // "WLCP"
  uint32_t frame_id;    // Monotonically increasing
  uint32_t total_size;  // Full JPEG size
  uint16_t chunk_index; // 0..chunk_count-1
  uint16_t chunk_count; // Total chunks
  uint16_t payload_size;
  uint16_t reserved;
};
```

Chunk size: 1200 bytes (avoids IP fragmentation)

## Troubleshooting

### No frames received
- Check firewall: `sudo ufw allow 7723/udp`
- Verify IP address in `--dest`
- Check viewer is listening: `ss -uln | grep 7723`

### Low FPS
- Use `--opencl` for best performance (requires libmali)
- Reduce quality: `--quality 60`
- Check CPU usage with `top`

### Encoder errors
- Reload kernel module: `rmmod hantro_vpu && modprobe hantro_vpu`
- Check `WLCAST_DEBUG=1` output for format issues

### Colors look wrong
- Make sure viewer uses `SDL_PIXELFORMAT_XRGB8888`
- See troubleshooting section in docs for details

## License

GPL v3 - See [LICENSE](LICENSE) for details.
