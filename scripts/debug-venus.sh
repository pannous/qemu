#!/bin/bash
# Debug Venus/Vulkan on macOS - tmux wrapper
# Creates a split layout for monitoring Venus components

SESSION="venus-debug"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU_DIR="$(dirname "$SCRIPT_DIR")"

# Attach to existing session or create new one
if tmux has-session -t "$SESSION" 2>/dev/null; then
    tmux attach -t "$SESSION"
else
    tmux new-session -d -s "$SESSION"
    tmux send-keys -t "$SESSION" "cd $QEMU_DIR && ./scripts/run-alpine.sh" Enter
    tmux attach -t "$SESSION"
fi
