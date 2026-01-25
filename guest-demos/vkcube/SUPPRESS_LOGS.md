# Virglrenderer Debug Logs

## Status: ✅ FIXED

All debug spam has been permanently removed from both QEMU and virglrenderer.

## What Was Fixed

### 1. Swapchain Debug Spam (QEMU)
**File:** `hw/display/virtio-gpu-vk-swapchain.m` (lines 507-517)

**Removed:**
- `swapchain debug: fmt=%u stride=%u w=%u h=%u first_pixel=0x%08x` stderr spam
- `/tmp/vkr_hostptr.log` file logging
- Faulty `getenv("VKR_PRESENT_DEBUG")` check that triggered even when set to "0"

### 2. Render Server Debug Spam (virglrenderer)
**File:** `server/render_common.c` (line 19+)

**Fixed:**
- Added `VIRGL_SERVER_DEBUG` environment variable guard
- Disabled `render_log()` by default to prevent:
  - `Jan 25 09:26:53 virgl_render_server[10643] <Debug>: render_receive_request...`
  - `[virgl_render_server] ...` stderr messages
- Only logs when `VIRGL_SERVER_DEBUG=1` is explicitly set

## Current Behavior

**Default:** vkcube and all Venus applications run with **clean, silent output** ✨

**Debug mode:** Set `VIRGL_SERVER_DEBUG=1` to re-enable verbose logging if needed

## Re-enabling Debug Logs (optional)

If you need to debug virglrenderer issues:

```bash
export VIRGL_SERVER_DEBUG=1
./scripts/run-alpine.sh
```

This will restore all the debug output.

## Rebuilding (if you modify the code)

**QEMU:**
```bash
cd /opt/other/qemu
make -j$(sysctl -n hw.ncpu)
```

**virglrenderer:**
```bash
cd /opt/other/virglrenderer
ninja -C builddir
ninja -C builddir install
```

## Legacy Workarounds (no longer needed)

These options are obsolete now that the fix is permanent:

~~`export VKR_PRESENT_DEBUG=0`~~ - Variable no longer exists
~~`./vkcube_anim 2>/dev/null`~~ - No spam to redirect
~~`grep -v "virgl_render_server"`~~ - No spam to filter
