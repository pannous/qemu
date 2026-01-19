# Alpine Linux Setup for Venus/Vulkan Testing

## Quick Start

```bash
# 1. First-time install (boots from ISO with disk attached)
./scripts/run-alpine.sh install

# In the guest, run:
#   setup-alpine   # Follow prompts, install to /dev/vda, use 'sys' mode
#   poweroff       # Shutdown after install

# 2. Run installed system
./scripts/run-alpine.sh run
```

## Guest Setup Commands (after install)

```bash
# Login as root (no password initially)

# Set up networking
setup-interfaces -a
ifup eth0

# Enable SSH
rc-update add sshd
service sshd start
passwd root  # Set password for SSH login

# Add repositories
cat > /etc/apk/repositories << 'EOF'
http://dl-cdn.alpinelinux.org/alpine/edge/main
http://dl-cdn.alpinelinux.org/alpine/edge/community
EOF
apk update

# Install Vulkan packages
apk add mesa-vulkan-virtio vulkan-loader vulkan-tools

# Test Vulkan
vulkaninfo --summary
```

## SSH Access

From host:
```bash
ssh -p 2222 root@localhost
```

## Files Created

- `/tmp/alpine-disk.qcow2` - 8GB persistent disk image
- `/tmp/alpine-efivars.fd` - EFI variables (for EFI boot)
- `/tmp/alpine-boot/` - Extracted kernel/initrd from ISO

## Device Order (Critical!)

The script places `virtio-gpu-gl-pci` (Venus) BEFORE `virtio-gpu-pci` so that:
- Venus device becomes `/dev/dri/renderD128`
- Mesa's virtio Vulkan driver opens renderD128 first and finds Venus

If device order is wrong, `vulkaninfo` will fail with "Failed to detect any valid GPUs".
