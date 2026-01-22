#!/bin/sh
# Build script for Rust Vulkan demos on Alpine guest
# Run: ./build.sh

set -e

# Install Rust toolchain if missing
if ! command -v rustc >/dev/null 2>&1; then
    echo "Installing Rust..."
    apk add --no-cache rust cargo
fi

# Install Vulkan/DRM dependencies
if ! pkg-config --exists vulkan 2>/dev/null; then
    echo "Installing Vulkan dependencies..."
    apk add --no-cache \
        vulkan-loader vulkan-headers \
        mesa-vulkan-virtio \
        libdrm libdrm-dev \
        pkgconf
fi

# Install shader compiler if missing
if ! command -v glslc >/dev/null 2>&1; then
    echo "Installing shaderc..."
    apk add --no-cache shaderc
fi

cd "$(dirname "$0")"

# Compile shaders (reuse from C demos)
echo "Compiling shaders..."
glslc ../guest-demos/triangle/tri.vert -o tri.vert.spv
glslc ../guest-demos/triangle/tri.frag -o tri.frag.spv
glslc ../guest-demos/vkcube/cube.vert -o cube.vert.spv
glslc ../guest-demos/vkcube/cube.frag -o cube.frag.spv

# Build Rust demos
echo "Building Rust demos..."
cargo build --release

echo ""
echo "Build complete!"
echo "Binaries in: target/release/"
echo "  - test_tri     (triangle demo)"
echo "  - vkcube_anim  (spinning cube demo)"
