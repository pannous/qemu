#!/bin/bash
# Install shadertoy viewer to Alpine guest
# Run from host: ./install-to-guest.sh

set -e

GUEST_USER="root"
GUEST_IP="localhost"
GUEST_PORT="2222"
GUEST_DIR="/root/shadertoy"

echo "Installing shadertoy viewer to guest..."

# Create directory on guest
ssh -p $GUEST_PORT $GUEST_USER@$GUEST_IP "mkdir -p $GUEST_DIR"

# Copy all files
echo "Copying files..."
scp -P $GUEST_PORT -r \
    shadertoy_viewer.cpp \
    shadertoy.vert \
    shadertoy.frag \
    build.sh \
    run.sh \
    switch_shader.sh \
    examples/ \
    shaders/ \
    Makefile \
    README.md \
    $GUEST_USER@$GUEST_IP:$GUEST_DIR/

echo "Building on guest..."
ssh -p $GUEST_PORT $GUEST_USER@$GUEST_IP "cd $GUEST_DIR && chmod +x *.sh && ./build.sh"

echo ""
echo "Installation complete!"
echo "To run: ssh -p $GUEST_PORT $GUEST_USER@$GUEST_IP"
echo "Then: cd $GUEST_DIR && ./shadertoy_viewer"
