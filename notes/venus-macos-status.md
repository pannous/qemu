# Venus on macOS - Current Status (2026-01-22)

## Summary

✅ **MAJOR MILESTONE ACHIEVED**: Venus rendering fully working on macOS with animated 3D graphics!

- ✅ RGB triangle demo working (single frame)
- ✅ **Animated spinning cube working at 273 FPS!**
- ✅ Depth buffering working
- ✅ All vertex transforms + rasterization + fragment shading on host GPU
- ✅ Vulkan → MoltenVK → Metal pipeline fully functional

## What Works

| Feature | Status | Notes |
|---------|--------|-------|
| Venus protocol | ✅ Working | Commands forwarded to MoltenVK |
| Vulkan instance/device | ✅ Working | "Virtio-GPU Venus (Apple M2 Pro)" |
| HOST_VISIBLE memory | ✅ Working | Via VK_EXT_external_memory_host + SHM |
| vkMapMemory | ✅ Working | Fixed SHM validation in virglrenderer |
| Blob resources | ✅ Working | GBM creates blob-backed buffers |
| DRM scanout | ✅ Working | SET_SCANOUT_BLOB triggers display |
| Host Vulkan swapchain | ✅ Working | MoltenVK → CAMetalLayer |

## Key Fixes Made

### 0. GBM Format Fix (CRITICAL - 2026-01-22)
**Problem**: `drmModeSetCrtc` returned -22 (EINVAL) when using `GBM_FORMAT_ARGB8888`
**Root Cause**: virtio-gpu DRM driver doesn't support alpha channel formats for scanout buffers
**Solution**: Changed to `GBM_FORMAT_XRGB8888` (no alpha channel)
**Impact**: Triangle demo now displays successfully!

### 1. VK_EXT_external_memory_host Fallback (virglrenderer)
MoltenVK lacks `VK_KHR_external_memory_fd`. Implemented SHM-backed memory import:
- Create anonymous SHM file
- mmap to host pointer
- Import via `VkImportMemoryHostPointerInfoEXT`
- Export as `VIRGL_RESOURCE_FD_SHM` blob

### 2. SHM Size Validation Fix (virglrenderer)
macOS requires 16KB alignment for host pointer import. Changed validation from `size == expected` to `size >= expected`.

**Commit**: `0b3d075a`

### 3. MoltenVK Portability Extensions (QEMU)
Added required MoltenVK extensions to host swapchain:
- `VK_KHR_portability_enumeration`
- `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`
- `VK_KHR_portability_subset`

## Test Results

### Triangle Demo (HOST_VISIBLE + Copy) - 2026-01-22 ✅
```
Display: 1280x800
GPU: Virtio-GPU Venus (Apple M2 Pro)
GBM blob: stride=5120, prime_fd=6
Vulkan: rendered 1280x800 triangle to LINEAR HOST_VISIBLE image
Copied to GBM buffer (XRGB8888 format)
drmModeDirtyFB succeeded - buffer marked for display
drmModeSetCrtc succeeded!
RGB triangle on blue (5s)
```
**Result**: RGB triangle visible in QEMU window for 5 seconds!

### Memory Mapping Test
```
vkAllocateMemory: 0
vkMapMemory: 0 ptr=0xffffb3fbc000
write OK!
```

### Blob Scanout Test
```
Display: 1280x800
GBM BO: stride=5120, prime_fd=6
Vulkan: Virtio-GPU Venus (Apple M2 Pro)
Vulkan: rendered 1280x800 blue
Copied to GBM buffer
Blue screen for 3s...
Done!
```

## Architecture (Current: HOST_VISIBLE + Copy)

```
┌─────────────────────────────────────────────────────────────┐
│                         GUEST                               │
├─────────────────────────────────────────────────────────────┤
│  Vulkan App                                                 │
│       ↓                                                     │
│  vkCreateImage (LINEAR, HOST_VISIBLE)                       │
│       ↓                                                     │
│  Render triangle to VkImage                                 │
│       ↓                                                     │
│  vkMapMemory + memcpy to GBM buffer (XRGB8888)             │
│       ↓                                                     │
│  drmModeDirtyFB + drmModeSetCrtc                            │
└───────────────────────────┬─────────────────────────────────┘
                            │ RESOURCE_FLUSH
┌───────────────────────────┴─────────────────────────────────┐
│                          HOST                               │
├─────────────────────────────────────────────────────────────┤
│  QEMU virtio-gpu-virgl.c                                    │
│       ↓                                                     │
│  virglrenderer (Venus) → MoltenVK                           │
│       ↓                                                     │
│  Host Vulkan Swapchain → CAMetalLayer → macOS Window        │
│       ↓                                                     │
│  ✅ RGB triangle visible!                                   │
└─────────────────────────────────────────────────────────────┘
```

## Next Steps

### 1. ✅ DONE: Working Demos
Both triangle and vkcube demos now work via HOST_VISIBLE + copy path!

### 2. Performance Analysis

**Current**: 273 FPS (Venus GPU rendering + GBM blob + drmModeDirtyFB)
**Previous**: 939 FPS (Venus GPU rendering + DRM dumb buffer, no dirty FB call)

#### Why is the previous version 3.4x faster?

The old version used simpler DRM infrastructure:
```c
// Old: DRM dumb buffer (simple linear memory)
struct drm_mode_create_dumb create = {...};
void *fbPtr = mmap(...);  // One-time mapping
// Per-frame:
memcpy(fbPtr, rtPtr, ...);           // Copy to dumb buffer
drmModeSetCrtc(...);                 // Display (no drmModeDirtyFB!)
```

New version uses GBM blobs with extra steps:
```c
// New: GBM blob (virtio-gpu resource)
struct gbm_bo *bo = gbm_bo_create(...);
// Per-frame:
void *ptr = gbm_bo_map(...);         // Map per frame
memcpy(ptr, rtPtr, ...);             // Copy to GBM buffer
gbm_bo_unmap(...);                   // Unmap per frame
drmModeDirtyFB(...);                 // Mark dirty (extra syscall!)
drmModeSetCrtc(...);                 // Display
```

**Overhead sources:**
1. **`drmModeDirtyFB` call**: Extra syscall not present in old version
2. **GBM map/unmap per frame**: More overhead than one-time `mmap()`
3. **GBM blob infrastructure**: More complex than simple dumb buffers
4. **Venus protocol overhead**: All Vulkan calls cross virtio boundary

**Conclusion**: For future optimization, consider reverting to DRM dumb buffers (skip `drmModeDirtyFB`) or implement true zero-copy with resource ID fix

### 3. Zero-Copy Rendering (Future Optimization)
**Goal**: Import GBM prime FD directly as VkImage, eliminate CPU copy
**Blocker**: Resource ID mismatch - Venus blob resource IDs don't match GBM prime FDs
**Status**: See `notes/zero-copy-todo.md` for investigation details
**Priority**: Low - current performance is good enough for now

### 4. Real-World Applications
With working Venus + display, next targets:
- Test Mesa demos (glxgears equivalent for Vulkan)
- Test vkcube-wayland with wlroots
- Integration with Redox OS
- IOSurface optimization for macOS (if needed)

## Files Modified

### QEMU
- `hw/display/virtio-gpu-vk-swapchain.m` - Host Vulkan swapchain
- `hw/display/virtio-gpu-virgl.c` - Blob scanout integration
- `ui/cocoa.m` - CAMetalLayer support

### virglrenderer
- `src/venus/vkr_device_memory.c` - VK_EXT_external_memory_host path
- `src/venus/vkr_physical_device.c` - Extension detection
- `src/proxy/proxy_context.c` - SHM size validation fix
- `src/proxy/proxy_socket.c` - macOS socket framing
- `server/render_socket.c` - macOS socket framing

## Running

```bash
# Start VM with Venus
./scripts/run-alpine.sh

# In guest, verify Venus works
vulkaninfo | grep "Virtio-GPU Venus"

# Run triangle demo (HOST_VISIBLE + copy path)
ssh -p 2222 root@localhost
cd /root
./test_tri
# Should display RGB triangle on blue background for 5 seconds!
```

## Demo Programs

### test_tri (✅ Working)
- **Path**: `guest-demos/triangle/test_tri.c`
- **Method**: HOST_VISIBLE memory + CPU copy to GBM
- **Format**: GBM_FORMAT_XRGB8888 (no alpha)
- **Result**: RGB triangle on blue background
- **Display**: 5 seconds (single frame)

### vkcube (✅ Working!)
- **Path**: `guest-demos/vkcube/vkcube_anim.c`
- **Method**: HOST_VISIBLE memory + CPU copy to GBM
- **Format**: GBM_FORMAT_XRGB8888 (no alpha)
- **Result**: Spinning rainbow cube with depth buffer
- **Display**: 10 seconds animation
- **Performance**: **273.4 FPS** (2734 frames in 10s)
- **Rendering**: All vertex transforms + rasterization + fragment shading on host GPU
- **Details**: MVP matrix computed on guest CPU, uploaded to uniform buffer, vertex shader does transforms on host

### vkcube_zerocopy (❌ Not Working)
- **Path**: `guest-demos/vkcube/vkcube_anim_zerocopy_attempt.c`
- **Method**: External memory import (zero-copy attempt)
- **Issue**: VK_ERROR_DEVICE_LOST due to resource ID mismatch
- **Status**: Preserved for future reference when zero-copy is fixed
