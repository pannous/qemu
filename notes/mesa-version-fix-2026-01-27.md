# Mesa Version Compatibility Fix - 2026-01-27

## Problem Identified

Venus rendering stopped working due to Mesa version mismatch between guest and host:

### Initial Error
```
ERROR_OUT_OF_HOST_MEMORY during vkCreateInstance
virtio_gpu errors: 0x1200, 0x1203 (protocol errors)
```

### Root Cause
- **Guest Alpine**: Mesa 25.2.7-r3 (November 2025)
- **Host**: Mesa 26.0.0-devel (January 2026)
- **virglrenderer**: From January 22, 2026

The protocol version mismatch caused Venus communication to fail completely.

## Solution Applied

### Step 1: Match Host Mesa to Guest
Downgraded host Mesa from 26.0.0-devel to 25.2.7:

```bash
cd /opt/other/mesa
git checkout 461196a1c82  # Mesa 25.2.7 (Nov 12, 2025)
```

### Step 2: Rebuild Everything
```bash
./scripts/rebuild-all.sh clean
```

This rebuilt:
- virglrenderer with Mesa 25.2.7 headers
- QEMU with updated virglrenderer

## Results

### ✅ Protocol Errors Fixed
- No more 0x1200 / 0x1203 virtio_gpu errors
- vkCreateInstance now succeeds
- Venus protocol communication working

### ❌ New Issue: Object Handle Translation
```
vkr: failed to look up object 14 of type 19 (VK_OBJECT_TYPE_PIPELINE)
vkr: vkCmdBindPipeline resulted in CS error
```

The protocol versions match, but object handle translation still fails. This suggests:
1. virglrenderer (Jan 22) Venus protocol is still older than Mesa 25.2.7 (Nov 12)
2. Need to upgrade virglrenderer to match Mesa 25.2.7 timeframe

## Next Steps

### Option 1: Upgrade virglrenderer (Blocked)
- Upstream virglrenderer has macOS compatibility issues:
  - `MSG_CMSG_CLOEXEC` not available on macOS
  - `clock_nanosleep` not available on macOS
- Custom venus-stable branch has macOS fixes but older protocol

### Option 2: Downgrade Guest Mesa (Recommended)
Downgrade Alpine guest Mesa to version from January 2026 timeframe:
```bash
# In guest:
apk add mesa-vulkan-layers=<jan-version>
```

### Option 3: Cherry-pick Protocol Updates
Cherry-pick protocol sync commits from upstream to venus-stable:
```bash
cd /opt/other/virglrenderer
git cherry-pick acaf0be7  # sync to latest protocol for v1.4.334
git cherry-pick cd978d97  # sync latest protocol for more shader extensions
```

## Files Modified

- `/opt/other/mesa`: Checked out 461196a1c82 (Mesa 25.2.7)
- `/opt/other/qemu/scripts/rebuild-all.sh`: Created comprehensive rebuild script
- `/opt/other/qemu/scripts/run-alpine.sh`: Fixed render_server path (builddir → build)

## Current Status

- ✅ Venus protocol communication works
- ✅ vkCreateInstance succeeds
- ❌ Pipeline objects fail to translate
- ❌ Triangle demo crashes with "stuck in ring seqno wait"

The fix is partial - we need either older guest Mesa or newer virglrenderer protocol.
