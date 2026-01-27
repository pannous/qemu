#!/bin/sh
# Build script for ShaderToy viewer in Alpine guest
# Run: ./build.sh

set -e

# Install dependencies if missing
if ! command -v glslc >/dev/null 2>&1; then
    echo "Installing build dependencies..."
    apk add --no-cache \
        vulkan-tools vulkan-loader vulkan-headers \
        mesa-vulkan-virtio mesa-dev \
        libdrm libdrm-dev \
        gbm-dev \
        shaderc \
        build-base \
        gcc
fi

cd "$(dirname "$0")"

# Compile the DRM-based viewer (pure C, no C++!)
echo "Compiling shadertoy_drm..."
gcc -O2 -o shadertoy_drm shadertoy_drm.c \
    -lvulkan -ldrm -lgbm -I/usr/include/libdrm -lm

echo "Build complete!"
echo ""
echo "Usage: ./run_shader.sh <shader.frag> [duration]"
echo "Example: ./run_shader.sh /opt/3d/metalshade/tunnel.frag 30"
