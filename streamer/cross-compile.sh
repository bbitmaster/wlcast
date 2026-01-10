#!/bin/bash
# Cross-compile wlcast-stream for ROCKNIX RK3566 (aarch64)

TOOLCHAIN="/home/ben/claude_code_dir/rocknix_stuff/distribution/build.ROCKNIX-RK3566.aarch64/toolchain"
SYSROOT="$TOOLCHAIN/aarch64-rocknix-linux-gnu/sysroot"

export PATH="$TOOLCHAIN/bin:$PATH"
export CC="aarch64-rocknix-linux-gnu-gcc"
export PKG_CONFIG="$TOOLCHAIN/bin/pkg-config"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export WAYLAND_SCANNER="$TOOLCHAIN/bin/wayland-scanner"

# Clean all object files (in case of architecture mismatch from native build)
rm -f *.o

# Build
make clean 2>/dev/null
make "$@"
