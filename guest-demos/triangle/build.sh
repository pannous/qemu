#!/bin/sh
# Build script for Venus triangle demo
set -e

if ! command -v glslc >/dev/null 2>&1; then
    echo "Installing dependencies..."
    apk add --no-cache vulkan-tools vulkan-loader vulkan-headers \
        mesa-vulkan-virtio mesa-dev mesa-gbm libdrm libdrm-dev shaderc build-base
fi

cd "$(dirname "$0")"
echo "Compiling shaders..."
glslc tri.vert -o tri.vert.spv
glslc tri.frag -o tri.frag.spv

echo "Compiling test_tri..."
gcc -O2 -o test_tri test_tri.c -lvulkan -lgbm -ldrm -I/usr/include/libdrm

echo "Build complete! Run: ./test_tri"
