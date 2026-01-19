#!/bin/bash
# Debug Venus/Vulkan on macOS - tmux wrapper
# Creates a split layout for monitoring Venus components

SESSION="venus-debug"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU_DIR="$(dirname "$SCRIPT_DIR")"

# Check for tmux
if ! command -v tmux &>/dev/null; then
    echo "Error: tmux not found. Install with: brew install tmux"
    exit 1
fi

# Kill existing session
tmux kill-session -t "$SESSION" 2>/dev/null

# Create new session
tmux new-session -d -s "$SESSION" -x 200 -y 50

# Pane 0: QEMU with serial console (main)
tmux send-keys -t "$SESSION:0" "cd $QEMU_DIR && ./scripts/run-alpine.sh" Enter

# Split horizontally for monitoring
tmux split-window -h -t "$SESSION:0"

# Pane 1: Process monitor
tmux send-keys -t "$SESSION:0.1" "watch -n 2 'ps aux | grep -E \"qemu|virgl_render\" | grep -v grep'" Enter

# Split pane 1 vertically
tmux split-window -v -t "$SESSION:0.1"

# Pane 2: SSH to VM (after boot)
tmux send-keys -t "$SESSION:0.2" "echo 'Waiting for VM to boot...'; sleep 30; echo 'Try: ssh -p 2222 root@localhost'"

# Split pane 0 vertically for log monitoring
tmux split-window -v -t "$SESSION:0.0"

# Pane 3: Instructions
tmux send-keys -t "$SESSION:0.3" "cat << 'EOF'
╔═══════════════════════════════════════════════════════════════╗
║                  Venus Debug Session                          ║
╠═══════════════════════════════════════════════════════════════╣
║                                                               ║
║  Quick Commands (in guest):                                   ║
║    setup-interfaces -a && ifup eth0                          ║
║    echo 'http://dl-cdn.alpinelinux.org/alpine/edge/main' > /etc/apk/repositories
║    echo 'http://dl-cdn.alpinelinux.org/alpine/edge/community' >> /etc/apk/repositories
║    apk update && apk add mesa-vulkan-virtio vulkan-tools     ║
║    vulkaninfo --summary                                       ║
║                                                               ║
║  Host Debug:                                                  ║
║    VK_ICD_FILENAMES=/opt/homebrew/Cellar/molten-vk/1.4.0/etc/vulkan/icd.d/MoltenVK_icd.json vulkaninfo
║    nm /opt/other/virglrenderer/install/lib/libvirglrenderer.dylib | grep venus
║                                                               ║
║  Tmux:                                                        ║
║    Ctrl-B + arrow keys to switch panes                       ║
║    Ctrl-B + z to zoom pane                                   ║
║    Ctrl-B + d to detach                                      ║
║                                                               ║
╚═══════════════════════════════════════════════════════════════╝
EOF" Enter

# Attach to session
tmux attach -t "$SESSION"
