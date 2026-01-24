#!/bin/bash
# Start a simple HTTP server for the WebGPU demo

cd "$(dirname "$0")"

echo "Starting WebGPU Cube Demo..."
echo "Open http://localhost:8000 in your browser"
echo ""
echo "Supported browsers:"
echo "  - Chrome/Edge 113+"
echo "  - Safari 18+"
echo ""
echo "Press Ctrl+C to stop"

python3 -m http.server 8000
