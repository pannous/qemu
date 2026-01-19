#!/bin/bash
# Run Alpine Linux aarch64 VM with Venus dual-GPU setup
# Includes SSH port forwarding and serial console
#
# Usage: ./run-alpine.sh [install|run]
#   install - Boot from ISO for installation (first time)
#   run     - Boot from installed disk (default)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU_DIR="$(dirname "$SCRIPT_DIR")"

# Paths
QEMU="${QEMU_DIR}/build/qemu-system-aarch64"
ISO="/tmp/alpine-virt-3.21.0-aarch64.iso"
DISK="/tmp/alpine-disk.qcow2"
EFI_CODE="/opt/homebrew/share/qemu/edk2-aarch64-code.fd"
EFI_VARS="/tmp/alpine-efivars.fd"

# MoltenVK ICD - correct path for Homebrew installation
export VK_ICD_FILENAMES=/opt/homebrew/Cellar/molten-vk/1.4.0/etc/vulkan/icd.d/MoltenVK_icd.json

# Check QEMU
if [[ ! -x "$QEMU" ]]; then
    echo "Error: QEMU not found at $QEMU"
    echo "Run: cd $QEMU_DIR && ./configure && make -j$(sysctl -n hw.ncpu)"
    exit 1
fi

# Create disk if missing
if [[ ! -f "$DISK" ]]; then
    echo "Creating 8GB disk image at $DISK..."
    qemu-img create -f qcow2 "$DISK" 8G
fi

# Create EFI vars if missing
if [[ ! -f "$EFI_VARS" ]]; then
    cp "$EFI_CODE" "$EFI_VARS" 2>/dev/null || {
        echo "Warning: EFI code not found, will boot without EFI"
    }
fi

MODE="${1:-run}"

# Common options for Venus dual-GPU
# IMPORTANT: Venus device FIRST so it becomes renderD128 (Mesa opens renderD128 first)
COMMON_OPTS=(
    -M virt -accel hvf -cpu host -m 2G -smp 4
    -device virtio-gpu-gl-pci,venus=on,blob=on,hostmem=256M
    -device virtio-gpu-pci
    -display cocoa
    -device qemu-xhci -device usb-kbd -device usb-tablet
    -device virtio-net-pci,netdev=net0
    -netdev user,id=net0,hostfwd=tcp::2222-:22
    -serial mon:stdio
)

echo "Starting Alpine Linux aarch64 VM (mode: $MODE)..."
echo "  SSH: ssh -p 2222 root@localhost"
echo "  Serial console: Ctrl-A X to quit"
echo ""

case "$MODE" in
    install)
        # Install mode: boot from ISO with disk attached
        KERNEL="/tmp/alpine-boot/boot/vmlinuz-virt"
        INITRD="/tmp/alpine-boot/boot/initramfs-virt"

        if [[ ! -f "$KERNEL" ]]; then
            echo "Extracting kernel from ISO..."
            mkdir -p /tmp/alpine-boot
            (cd /tmp/alpine-boot && bsdtar -xf "$ISO" boot 2>/dev/null)
        fi

        echo "Install mode - run 'setup-alpine' in guest to install to disk"
        echo "After install, restart with: $0 run"
        echo ""

        exec "$QEMU" "${COMMON_OPTS[@]}" \
            -kernel "$KERNEL" \
            -initrd "$INITRD" \
            -append "console=ttyAMA0 modules=loop,squashfs,sd-mod,usb-storage quiet" \
            -cdrom "$ISO" \
            -drive if=virtio,file="$DISK",format=qcow2
        ;;

    run)
        # Run mode: boot from installed disk with EFI
        if [[ ! -s "$DISK" ]] || [[ $(qemu-img info "$DISK" 2>/dev/null | grep 'virtual size' | grep -o '[0-9.]*') == "0" ]]; then
            echo "Disk appears empty. Run with 'install' first:"
            echo "  $0 install"
            exit 1
        fi

        # Check if disk has been installed (has any data beyond initial qcow2)
        ACTUAL_SIZE=$(qemu-img info "$DISK" 2>/dev/null | grep 'disk size' | awk '{print $3}')
        if [[ "$ACTUAL_SIZE" == "196" ]]; then  # Empty qcow2 is ~196K
            echo "Disk appears empty. Run with 'install' first:"
            echo "  $0 install"
            exit 1
        fi

        exec "$QEMU" "${COMMON_OPTS[@]}" \
            -drive if=pflash,format=raw,readonly=on,file="$EFI_CODE" \
            -drive if=pflash,format=raw,file="$EFI_VARS" \
            -drive if=virtio,file="$DISK",format=qcow2
        ;;

    *)
        echo "Usage: $0 [install|run]"
        echo "  install - Boot from ISO for installation"
        echo "  run     - Boot from installed disk (default)"
        exit 1
        ;;
esac
