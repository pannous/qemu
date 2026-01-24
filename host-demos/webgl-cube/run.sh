#!/bin/bash
# Open WebGL cube demo in default browser

cd "$(dirname "$0")"

echo "Opening WebGL Gradient Cube demo in browser..."
echo "Check console for GPU info"

# macOS
if [[ "$OSTYPE" == "darwin"* ]]; then
    open index.html
# Linux
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    xdg-open index.html
else
    echo "Please open index.html manually in your browser"
fi
