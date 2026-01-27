#!/bin/sh
# Wrapper script to compile and run ShaderToy shaders
# Usage: ./run_shader.sh <shader.frag> [duration]

set -e

if [ $# -lt 1 ]; then
    echo "Usage: $0 <shader.frag> [duration]"
    echo "Example: $0 /opt/3d/metalshade/tunnel.frag 30"
    exit 1
fi

SHADER_PATH="$1"
DURATION="${2:-30}"

# Get shader base name
SHADER_NAME=$(basename "$SHADER_PATH" .frag)
SHADER_DIR=$(dirname "$SHADER_PATH")

echo "Compiling shader: $SHADER_PATH"

# Convert to Vulkan GLSL if needed
if [ ! -f "${SHADER_DIR}/${SHADER_NAME}.glsl" ]; then
    if grep -q "#version 450" "$SHADER_PATH" 2>/dev/null; then
        # Already Vulkan format
        cp "$SHADER_PATH" "${SHADER_DIR}/${SHADER_NAME}.glsl"
    else
        # Convert from ShaderToy format
        python3 /opt/3d/metalshade/convert_book_of_shaders.py \
            "$SHADER_PATH" "${SHADER_DIR}/${SHADER_NAME}.glsl"
    fi
fi

# Compile to SPIR-V
glslc "${SHADER_DIR}/${SHADER_NAME}.glsl" -o "${SHADER_DIR}/${SHADER_NAME}.frag.spv" || {
    echo "✗ Shader compilation failed"
    exit 1
}

# Copy vertex shader
cp /opt/3d/metalshade/vert.spv "${SHADER_DIR}/${SHADER_NAME}.vert.spv"

echo "✓ Compiled: ${SHADER_DIR}/${SHADER_NAME}.frag.spv"
echo "✓ Vertex: ${SHADER_DIR}/${SHADER_NAME}.vert.spv"
echo ""

# Run the viewer
exec ./shadertoy_drm \
    "${SHADER_DIR}/${SHADER_NAME}.vert.spv" \
    "${SHADER_DIR}/${SHADER_NAME}.frag.spv" \
    "$DURATION"
