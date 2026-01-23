# Zero-Copy Vulkan Rendering Progress

**Date:** 2026-01-23
**Achievement:** 47 FPS with host swapchain presentation (5-6x improvement)

## Current Architecture

```
Guest VkImage (LINEAR, HOST_VISIBLE)
         ↓ (render on host GPU via Venus)
    Host Memory Pointer
         ↓ (QEMU swapchain blit, NO guest memcpy)
    Metal/Vulkan Swapchain
         ↓
    macOS Display
```

## What Works

1. **Venus hostptr exposure** - `virgl_renderer_get_venus_hostptr()` retrieves host memory pointers
2. **QEMU swapchain** - `virtio-gpu-vk-swapchain.m` presents host memory via VkSwapchainKHR
3. **Zero-copy pipeline** - Guest renders, QEMU blits directly from mapped host memory
4. **47 FPS** - Confirmed working in both triangle and vkcube demos

## Code Changes (Last 5 virglrenderer commits)

- **v2** (0018e310): Additional swapchain refinements
- **milestone zero-copy triangle** (f48b5b19): Core zero-copy implementation
- **codex wip** (19cf9e77): Initial scaffolding (~600 lines)
- Plus format/extension fixes for MoltenVK

Total: ~1,100 lines across Venus/proxy/server layers

## Known Technical Debt (from commit messages)

### 1. Legacy Scanout Override (HIGH PRIORITY)
- **Current:** Hijacking `virgl_cmd_set_scanout` to trigger swapchain present
- **Needed:** Use `SET_SCANOUT_BLOB` or clean Venus-aware scanout path
- **Location:** `hw/display/virtio-gpu-virgl.c`

### 2. Hostptr Selection Heuristic
- **Current:** "Keep largest hostptr" to avoid tiny allocations
- **Needed:** Proper resource_id → hostptr binding
- **Issue:** Not robust for multiple contexts/resources

### 3. Context Tracking
- **Current:** Global "last Venus ctx_id" in QEMU
- **Needed:** Per-resource context association
- **Risk:** Breaks with multiple simultaneous contexts

### 4. Format Hardcoding
- **Current:** XRGB8888 / PIXMAN_x8r8g8b8 only
- **Needed:** Multi-format support (ARGB, BGRA, etc.)
- **Location:** Multiple scanout paths

### 5. Debug Artifacts
- `/tmp/vkr_hostptr.log` - Venus debug logging
- Various printf debugging in swapchain code
- Unused `do_gl` compile warning

### 6. Resource Info Bypass
- **Issue:** `virgl_renderer_resource_get_info` fails in proxy mode
- **Current:** Bypassed with hardcoded assumptions
- **Needed:** Implement proxy query or refactor dependency

## Next Steps (Priority Order)

1. **Implement SET_SCANOUT_BLOB path** - Replace legacy scanout hack
2. **Resource → hostptr binding** - Proper tracking instead of heuristics
3. **Multi-format support** - Generalize beyond XRGB8888
4. **Clean up debug code** - Remove temp logging, fix warnings
5. **Proxy resource queries** - Fix or eliminate dependency on resource_get_info

## Performance Notes

- **Before:** ~8 FPS (HOST_VISIBLE + memcpy)
- **After:** 47 FPS (zero-copy swapchain)
- **Bottleneck removed:** Guest CPU memcpy eliminated
- **Current limit:** Likely vsync (60 FPS cap) or host GPU/swapchain overhead

## Files Modified (QEMU)

- `hw/display/virtio-gpu-virgl.c` - Scanout hooks, hostptr integration
- `hw/display/virtio-gpu-vk-swapchain.m` - Swapchain implementation (NEW)
- `hw/display/virtio-gpu-gl.c` - GL integration points
- `include/hw/virtio/virtio-gpu.h` - Headers
- `guest-demos-codex/vkcube/vkcube_anim.c` - Zero-copy demo

## Files Modified (virglrenderer)

- `src/venus/vkr_device_memory.c` - Hostptr tracking
- `src/venus/vkr_context.c` - Swapchain plumbing
- `src/venus/vkr_renderer.c` - API exposure
- `src/venus/vkr_image.c` - Image handling
- `src/proxy/proxy_context.c` - IPC forwarding
- `server/render_*.{c,h}` - Protocol extensions
- `src/virglrenderer.{c,h}` - Public API

## Test Commands

```bash
# Build virglrenderer
cd /opt/other/virglrenderer
ninja -C builddir install

# Run Alpine VM
cd /opt/other/qemu
./scripts/run-alpine.sh

# In guest (Alpine)
cd /root/vkcube
./vkcube_anim  # Should show 47 FPS
```

## Compatibility

- ✅ macOS (MoltenVK + Metal)
- ✅ Alpine Linux guest (16KB pages)
- ✅ HVF acceleration
- ❌ Linux host (needs IOSurface → dma-buf equivalent)
- ❌ Multiple contexts (context tracking is global)
- ❌ Non-XRGB formats (hardcoded)
