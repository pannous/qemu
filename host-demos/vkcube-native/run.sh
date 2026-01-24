#!/bin/bash
# Run the native MoltenVK cube demo
cd "$(dirname "$0")"

# For GUI apps on macOS, we need special handling to make windows visible
# when launched from terminal
exec ./vkcube_native
