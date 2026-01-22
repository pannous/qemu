#!/bin/bash
PORT="${1:-2222}"
DIR="$(cd "$(dirname "$0")" && pwd)"
scp -P "$PORT" -o StrictHostKeyChecking=no \
    "$DIR/test_tri.c" "$DIR/tri.vert" "$DIR/tri.frag" \
    "$DIR/tri.vert.spv" "$DIR/tri.frag.spv" "$DIR/build.sh" \
    root@localhost:/root/
ssh -p "$PORT" -o StrictHostKeyChecking=no root@localhost '/root/build.sh'
echo "Run: ssh -p $PORT root@localhost /root/test_tri"
