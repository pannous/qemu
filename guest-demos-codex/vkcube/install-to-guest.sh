#!/bin/bash
# Deploy vkcube demo to guest VM
# Usage: ./install-to-guest.sh [port] (default: 2222)

PORT="${1:-2222}"
DIR="$(cd "$(dirname "$0")" && pwd)"

echo "Deploying to guest on port $PORT..."
scp -P "$PORT" -o StrictHostKeyChecking=no \
    "$DIR/vkcube_anim.c" \
    "$DIR/cube.vert" \
    "$DIR/cube.frag" \
    "$DIR/cube.vert.spv" \
    "$DIR/cube.frag.spv" \
    "$DIR/build.sh" \
    root@localhost:/root/

echo "Running build on guest..."
ssh -p "$PORT" -o StrictHostKeyChecking=no root@localhost '/root/build.sh'

echo "Done! Run with: ssh -p $PORT root@localhost /root/vkcube_anim"
