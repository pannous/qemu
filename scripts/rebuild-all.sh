#!/bin/bash
# Rebuild entire Venus stack: virglrenderer + QEMU
# This ensures protocol compatibility between guest Mesa and host virglrenderer
# Usage: ./scripts/rebuild-all.sh [clean|quick]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU_DIR="$(dirname "$SCRIPT_DIR")"
VIRGL_DIR="/opt/other/virglrenderer"
MESA_DIR="/opt/other/mesa"
JOBS=$(sysctl -n hw.ncpu)
MODE="${1:-quick}"

echo "===================================="
echo "Rebuilding Venus Stack for macOS"
echo "===================================="
echo ""
echo "Mesa:          $MESA_DIR"
echo "virglrenderer: $VIRGL_DIR"
echo "QEMU:          $QEMU_DIR"
echo "Mode:          $MODE"
echo "Parallel jobs: $JOBS"
echo ""

# Check Mesa version
if [[ -d "$MESA_DIR" ]]; then
    cd "$MESA_DIR"
    MESA_COMMIT=$(git log --pretty=format:"%h %ad %s" --date=format:"%Y-%m-%d %H:%M" -1)
    echo "Mesa version:  $MESA_COMMIT"
    echo ""
fi

# Step 1: Rebuild virglrenderer
echo "===================================="
echo "Step 1: Building virglrenderer"
echo "===================================="

cd "$VIRGL_DIR"

if [[ "$MODE" == "clean" ]]; then
    echo "Clean build: removing build directory..."
    rm -rf build
    rm -rf install
fi

if [[ ! -d build ]]; then
    echo "Configuring virglrenderer with meson..."
    mkdir -p build
    meson setup build \
        -Dvenus=true \
        -Dtests=false \
        -Dprefix="$VIRGL_DIR/install" \
        -Dbuildtype=release
else
    echo "Reconfiguring existing build..."
    meson setup --reconfigure build
fi

echo "Building virglrenderer..."
meson compile -C build -j"$JOBS"

echo "Installing virglrenderer..."
meson install -C build

echo ""
echo "virglrenderer build complete!"
echo "Installed to: $VIRGL_DIR/install"
echo ""

# Verify virglrenderer installation
if [[ ! -f "$VIRGL_DIR/install/lib/libvirglrenderer.dylib" ]]; then
    echo "ERROR: virglrenderer library not found after installation!"
    exit 1
fi

# Step 2: Rebuild QEMU
echo "===================================="
echo "Step 2: Building QEMU"
echo "===================================="

cd "$QEMU_DIR"
"$SCRIPT_DIR/rebuild-qemu.sh" "$MODE"

echo ""
echo "===================================="
echo "✓ Full rebuild complete!"
echo "===================================="
echo ""
echo "Next steps:"
echo "  1. Test with: ./scripts/debug-venus.sh"
echo "  2. Or run:    ./run-alpine.sh"
echo ""
