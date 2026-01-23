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
QEMU_SIGNED="${QEMU_DIR}/build/qemu-system-aarch64"
QEMU_UNSIGNED="${QEMU_DIR}/build/qemu-system-aarch64-unsigned"
QEMU="$QEMU_SIGNED"
DISK_BACKING="${QEMU_DIR}/alpine-venus.img"
DISK="${QEMU_DIR}/alpine-overlay.qcow2"

# Create overlay if missing
if [[ ! -f "$DISK" ]]; then
    qemu-img create -f qcow2 -b "$DISK_BACKING" -F raw "$DISK"
fi
EFI_CODE="/opt/other/redox/tools/firmware/edk2-aarch64-code.fd"
EFI_VARS="${QEMU_DIR}/alpine-efivars.fd"

# MoltenVK ICD - correct path for Homebrew installation
export VK_ICD_FILENAMES=/opt/homebrew/Cellar/molten-vk/1.4.0/etc/vulkan/icd.d/MoltenVK_icd.json

# Vulkan loader library path for virglrenderer Venus backend
# Put custom virglrenderer FIRST so it's found before homebrew version
export DYLD_LIBRARY_PATH=/opt/other/virglrenderer/install/lib:/opt/homebrew/lib:${DYLD_LIBRARY_PATH:-}

# Use custom virglrenderer render_server from builddir (not installed)
export RENDER_SERVER_EXEC_PATH=/opt/other/virglrenderer/builddir/server/virgl_render_server

# Venus/virgl debug (uncomment for troubleshooting)
export VKR_DEBUG=all
export MVK_CONFIG_LOG_LEVEL=2

# Present from host-visible allocations via host Vulkan swapchain (no guest CPU copy)
: "${VKR_PRESENT_HOSTPTR:=1}"
export VKR_PRESENT_HOSTPTR
# Drive host-side present loop (uncapped by default for max FPS test)
: "${VKR_PRESENT_TIMER:=1}"
export VKR_PRESENT_TIMER
# Force host pointer import even if fd export is available (needed for host-present path)
: "${VKR_FORCE_HOSTPTR_IMPORT:=1}"
export VKR_FORCE_HOSTPTR_IMPORT
# Debug host-present path (prints first pixel/stride)
: "${VKR_PRESENT_DEBUG:=1}"
export VKR_PRESENT_DEBUG
# Ensure IOSurface path doesn't steal presentation from the host swapchain path
unset VKR_USE_IOSURFACE

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
#
# HVF acceleration: requires 16KB page kernel!
# Default 4KB kernel causes HVF to fail (16KB alignment requirement for memory regions)
# Use QEMU_ACCEL=hvf with QEMU_KERNEL=/path/to/16k-kernel for HVF
ACCEL="${QEMU_ACCEL:-hvf}"
if [[ "$ACCEL" == "hvf" ]]; then
    ACCEL_OPTS="-accel hvf -cpu host"
    # Warn if not using 16KB kernel
    if [[ -z "${QEMU_KERNEL:-}" ]]; then
        echo "WARNING: HVF requires 16KB page kernel. Set QEMU_KERNEL to use custom kernel."
        echo "         e.g. QEMU_KERNEL=$SCRIPT_DIR/alpine-virt-16k.img"
    fi
else
    ACCEL_OPTS="-accel tcg -cpu max"
    echo "Note: Don NOT use TCG acceleration, use HVF!"
    echo "If you encounter any issues, fix the issues, but do not switch back to TCG"
    echo "exiting"
    exit
fi

# Custom kernel override (for 16KB page kernel)
# Auto-detect 16KB kernel if not specified
if [[ -z "${QEMU_KERNEL:-}" && -f "$SCRIPT_DIR/alpine-virt-16k.img" ]]; then
    CUSTOM_KERNEL="$SCRIPT_DIR/alpine-virt-16k.img"
else
    CUSTOM_KERNEL="${QEMU_KERNEL:-}"
fi

COMMON_OPTS=(
    -M virt $ACCEL_OPTS -m 2G -smp 4
    -device virtio-gpu-gl-pci,venus=on,blob=on,hostmem=256M
    -display cocoa
    -device qemu-xhci -device usb-kbd -device usb-tablet
    -device virtio-net-pci,netdev=net0
    -netdev user,id=net0,hostfwd=tcp::2222-:22
    -fsdev local,id=mesa_dev,path=/opt/other/mesa,security_model=mapped-xattr
    -device virtio-9p-pci,fsdev=mesa_dev,mount_tag=mesa_share
    -serial mon:stdio
)

echo "Starting Alpine Linux aarch64 VM (mode: $MODE)..."
echo "  SSH: ssh -p 2222 root@localhost"
echo "  Serial console: Ctrl-A X to quit"
echo ""

case "$MODE" in
    install)
        # Install mode: boot from ISO with disk attached
        KERNEL="${QEMU_DIR}/alpine-boot/boot/vmlinuz-virt"
        INITRD="${QEMU_DIR}/alpine-boot/boot/initramfs-virt"

        if [[ ! -f "$KERNEL" ]]; then
            echo "Extracting kernel from ISO..."
            mkdir -p ${QEMU_DIR}/alpine-boot
            (cd ${QEMU_DIR}/alpine-boot && bsdtar -xf "$ISO" boot 2>/dev/null)
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
        # Run mode: boot from installed disk using direct kernel boot
        # (Alpine installs without EFI by default when using direct kernel boot)

        # Check disk exists and has data
        if [[ ! -s "$DISK" ]]; then
            echo "Disk not found. Run with 'install' first:"
            echo "  $0 install"
            exit 1
        fi

        ACTUAL_SIZE=$(qemu-img info "$DISK" 2>/dev/null | grep 'disk size' | awk '{print $3}')
        if [[ "$ACTUAL_SIZE" == "196" ]]; then  # Empty qcow2 is ~196K
            echo "Disk appears empty. Run with 'install' first:"
            echo "  $0 install"
            exit 1
        fi

        # Extract kernel from installed disk if not already done
        INSTALLED_KERNEL="${QEMU_DIR}/alpine-installed/vmlinuz-virt"
        INSTALLED_INITRD="${QEMU_DIR}/alpine-installed/initramfs-virt"

        if [[ ! -f "$INSTALLED_KERNEL" ]]; then
            echo "Extracting kernel from installed disk..."
            mkdir -p ${QEMU_DIR}/alpine-installed
            # Mount qcow2 using guestfish/libguestfs or nbd
            if command -v guestfish &>/dev/null; then
                guestfish --ro -a "$DISK" -m /dev/sda3 \
                    copy-out /boot/vmlinuz-virt ${QEMU_DIR}/alpine-installed/ : \
                    copy-out /boot/initramfs-virt ${QEMU_DIR}/alpine-installed/ 2>/dev/null
            fi
        fi

        # Fall back to ISO kernel if extraction failed
        if [[ ! -f "$INSTALLED_KERNEL" ]]; then
            echo "Note: Using ISO kernel (guestfish not available for extraction)"
            INSTALLED_KERNEL="${QEMU_DIR}/alpine-boot/boot/vmlinuz-virt"
            INSTALLED_INITRD="${QEMU_DIR}/alpine-boot/boot/initramfs-virt"
        fi

        # Use custom kernel if specified (for 16KB page kernel with HVF)
        if [[ -n "$CUSTOM_KERNEL" ]]; then
            echo "Using custom kernel: $CUSTOM_KERNEL"
            INSTALLED_KERNEL="$CUSTOM_KERNEL"
            # Note: 16KB kernel may need compatible initrd or modules
        fi

        # Boot with root on vda3 (standard Alpine sys install layout: vda1=boot, vda2=swap, vda3=root)
        exec "$QEMU" "${COMMON_OPTS[@]}" \
            -kernel "$INSTALLED_KERNEL" \
            -initrd "$INSTALLED_INITRD" \
            -append "console=ttyAMA0 root=/dev/vda3 modules=ext4 rootfstype=ext4 quiet" \
            -drive if=virtio,file="$DISK",format=qcow2
        ;;

    *)
        echo "Usage: $0 [install|run]"
        echo "  install - Boot from ISO for installation"
        echo "  run     - Boot from installed disk (default)"
        exit 1
        ;;
esac
