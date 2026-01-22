#!/bin/sh
# Copy Rust demos and shaders to Alpine guest
# Run from host: ./install-to-guest.sh

set -e
cd "$(dirname "$0")"

GUEST_PORT=2222

echo "Copying Rust source to guest /root/guest-demos-rs/..."
ssh -p $GUEST_PORT root@localhost "mkdir -p /root/guest-demos-rs"
scp -P $GUEST_PORT -r \
    Cargo.toml \
    build.sh \
    common \
    triangle \
    vkcube \
    root@localhost:/root/guest-demos-rs/

echo "Copying shaders to /root/..."
scp -P $GUEST_PORT \
    ../guest-demos/triangle/tri.vert \
    ../guest-demos/triangle/tri.frag \
    ../guest-demos/vkcube/cube.vert \
    ../guest-demos/vkcube/cube.frag \
    root@localhost:/root/

echo ""
echo "Done! SSH to guest and run:"
echo "  cd /root/guest-demos-rs && ./build.sh"
