#!/bin/sh
# Build script for Venus Vulkan cube demo
# Run: ./build.sh

set -e

# Install dependencies if missing
if ! command -v glslc >/dev/null 2>&1; then
    echo "Installing build dependencies..."
    apk add --no-cache \
        vulkan-tools vulkan-loader vulkan-headers \
        mesa-vulkan-virtio mesa-dev \
        libdrm libdrm-dev \
        shaderc \
        build-base
fi

cd "$(dirname "$0")"

# Compile shaders
echo "Compiling shaders..."
glslc cube.vert -o cube.vert.spv
glslc cube.frag -o cube.frag.spv

# Compile the demo
echo "Compiling vkcube_anim..."
gcc -O2 -o vkcube_anim vkcube_anim.c -lvulkan -ldrm -I/usr/include/libdrm -lm -lgbm

echo "Build complete!"
echo "Run with: ./vkcube_anim"
