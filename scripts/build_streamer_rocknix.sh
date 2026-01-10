#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STREAMER_DIR="$ROOT_DIR/streamer"
TOOLCHAIN_ROOT_DEFAULT="$ROOT_DIR/../distribution/build.ROCKNIX-RK3566.aarch64/toolchain"

TOOLCHAIN_ROOT="${TOOLCHAIN_ROOT:-$TOOLCHAIN_ROOT_DEFAULT}"
SYSROOT="${SYSROOT:-$TOOLCHAIN_ROOT/aarch64-rocknix-linux-gnu/sysroot}"
CC="${CC:-$TOOLCHAIN_ROOT/bin/aarch64-rocknix-linux-gnu-gcc}"

if [[ ! -d "$TOOLCHAIN_ROOT" ]]; then
  echo "Toolchain not found: $TOOLCHAIN_ROOT" >&2
  exit 1
fi

if [[ ! -d "$SYSROOT" ]]; then
  echo "Sysroot not found: $SYSROOT" >&2
  exit 1
fi

if [[ ! -x "$CC" ]]; then
  echo "Compiler not found: $CC" >&2
  exit 1
fi

if ! command -v wayland-scanner >/dev/null 2>&1; then
  echo "Missing wayland-scanner on host. Install: sudo pacman -S wayland wayland-protocols" >&2
  exit 1
fi

export PKG_CONFIG_DIR=
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export CCACHE_DISABLE=1
export CCACHE_TEMPDIR=/tmp

make -C "$STREAMER_DIR" clean
make -C "$STREAMER_DIR" CC="$CC" PKG_CONFIG=pkg-config \
  WAYLAND_SCANNER=wayland-scanner

file "$STREAMER_DIR/screen_streamer"
