# Alpine VM Boot Performance Analysis

**Date:** 2026-01-25
**Status:** ✅ WORKING but SLOW (~4 min boot time)

## Summary

The Alpine Linux VM boots successfully with Venus/Vulkan working, but boot time is ~4 minutes which seems excessive.

## Testing Results

### WFI Delay Impact
Tested with/without the 1ms g_usleep() in HVF's WFI handler:

| Configuration | First Kernel Msg | SSH Available | Result |
|--------------|------------------|---------------|---------|
| WITH 1ms delay | 52 seconds | ~240 seconds | ✅ Better |
| WITHOUT delay | 84 seconds | ~235 seconds | ❌ Worse |

**Conclusion:** The 1ms delay HELPS boot performance. Without it, HVF's spurious WFI wakeups actually slow down the early boot process.

### Upstream Merge Investigation
- Commit 9d1d592061 merged 165 upstream commits on Jan 25
- No ARM/HVF changes in that merge
- Likely not the cause of slow boot

## Current Boot Characteristics

### Successful Boot Flow
```
T+0s:   QEMU starts
T+52s:  First kernel message appears
T+85s:  OpenRC starting
T+120s: mdev scanning (slowest phase)
T+180s: Network services starting
T+240s: SSH available, login prompt
```

### What Works
- ✅ 16KB page kernel boots with HVF
- ✅ Venus/Vulkan initializes (no vkCreateInstance errors)
- ✅ /dev/dri/renderD128 available
- ✅ vulkaninfo shows Vulkan 1.4.321
- ✅ SSH works after boot

### Known Issues
- ⚠️ swap device fails (swapon: Invalid argument) - likely 16KB vs 4KB page mismatch
- ⚠️ Some sysctl network parameters unavailable
- ⚠️ mdev scanning takes 30-40 seconds

## Potential Causes of Slow Boot

1. **16KB page kernel overhead**
   - Custom kernel might not be fully optimized
   - Page size mismatch with disk structures

2. **Hardware scanning delays**
   - mdev taking 30-40s to scan virtio devices
   - Could be related to Venus GPU initialization

3. **Serial console buffering**
   - Large gap between QEMU start and first kernel message
   - Might indicate kernel is busy but not printing

4. **HVF/macOS specific**
   - Virtualization framework overhead
   - Context switching costs with 4 vCPUs

5. **initramfs loading**
   - Large initramfs might be slow to decompress

## Next Steps for Investigation

1. **Try with fewer CPUs**
   ```bash
   # Edit run-alpine.sh: change -smp 4 to -smp 2
   ```

2. **Enable kernel boot messages**
   ```bash
   # Remove "quiet" from kernel append line in run-alpine.sh
   ```

3. **Check initramfs size**
   ```bash
   ls -lh alpine-installed/initramfs-virt
   ```

4. **Try TCG temporarily** to see if HVF is the issue
   ```bash
   QEMU_ACCEL=tcg ./scripts/run-alpine.sh
   ```
   (Note: Will be slower overall, but might reveal if HVF is causing delays)

5. **Profile with verbose boot**
   - Remove "quiet" from kernel command line
   - Add "loglevel=7" to see all kernel messages
   - Identify which subsystems are slow

6. **Check if original (working) kernel boots faster**
   ```bash
   # Try booting without 16KB kernel to compare
   unset QEMU_KERNEL
   ./scripts/run-alpine.sh
   ```

## Comparison to Other Systems

Typical QEMU/KVM VM boot times:
- Minimal Linux: 5-10 seconds
- Alpine Linux: 15-30 seconds
- Full Ubuntu: 30-60 seconds

Our 240-second boot suggests something is genuinely wrong, not just "macOS overhead."

## Files Involved

- `/opt/other/qemu/scripts/run-alpine.sh` - VM startup script
- `/opt/other/qemu/scripts/alpine-virt-16k.img` - 16KB page kernel (37MB)
- `/opt/other/qemu/alpine-overlay.qcow2` - VM disk overlay
- `/opt/other/qemu/target/arm/hvf/hvf.c` - HVF WFI handler (line 1741)
