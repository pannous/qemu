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
        shaderc \
        build-base \
        glfw-dev \
        g++
fi

cd "$(dirname "$0")"

# Compile shaders
echo "Compiling shaders..."
glslc shadertoy.vert -o vert.spv
glslc shadertoy.frag -o frag.spv

# Compile example shaders if they exist
if [ -d "examples" ]; then
    echo "Compiling example shaders..."
    for shader in examples/*.frag; do
        [ -f "$shader" ] || continue
        glslc "$shader" -o "${shader}.spv"
    done
fi

# Compile the viewer
echo "Compiling shadertoy_viewer..."
g++ -O2 -o shadertoy_viewer shadertoy_viewer.cpp \
    -lvulkan -lglfw -I/usr/include/libdrm -lm

echo "Build complete!"
echo "Run with: ./shadertoy_viewer"
