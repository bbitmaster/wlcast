#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS_DIR="$ROOT_DIR/tools"
SRC="$TOOLS_DIR/v4l2_probe.c"
OUT="$TOOLS_DIR/v4l2_probe"
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

"$CC" --sysroot="$SYSROOT" -O2 -g -Wall -Wextra -Wshadow -Wvla -Wconversion \
  -o "$OUT" "$SRC"

file "$OUT"
