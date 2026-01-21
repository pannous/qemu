#!/bin/bash
# Rebuild QEMU for macOS aarch64 with Venus/VirGL support
# Usage: ./scripts/rebuild-qemu.sh [clean|quick]
#   clean - Full clean rebuild (removes build directory)
#   quick - Incremental build (default)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$QEMU_DIR/build"
JOBS=$(sysctl -n hw.ncpu)

MODE="${1:-quick}"

configure_qemu() {
    echo "Configuring QEMU..."
    # CRITICAL: Use macOS native ar/ranlib - GNU ar creates incompatible archives
    AR=/usr/bin/ar RANLIB=/usr/bin/ranlib "$QEMU_DIR/configure" \
        --target-list=aarch64-softmmu \
        --enable-hvf \
        --enable-cocoa \
        --enable-virglrenderer \
        --enable-virtfs \
        --disable-fuse \
        --disable-werror \
        --extra-cflags="-I/opt/homebrew/include -I/opt/other/virglrenderer/install/include" \
        --extra-ldflags="-L/opt/homebrew/lib -L/opt/other/virglrenderer/install/lib"
}

case "$MODE" in
    clean)
        echo "=== Full Clean Rebuild ==="
        rm -rf "$BUILD_DIR"
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
        configure_qemu
        ;;
    quick)
        echo "=== Incremental Build ==="
        if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
            echo "No existing build found, running configure..."
            mkdir -p "$BUILD_DIR"
            cd "$BUILD_DIR"
            configure_qemu
        else
            cd "$BUILD_DIR"
        fi
        ;;
    *)
        echo "Usage: $0 [clean|quick]"
        exit 1
        ;;
esac

echo "Building with $JOBS parallel jobs..."
ninja -j"$JOBS" qemu-system-aarch64

echo ""
echo "=== Build Complete ==="
"$BUILD_DIR/qemu-system-aarch64" --version
echo ""
echo "Binary: $BUILD_DIR/qemu-system-aarch64"
