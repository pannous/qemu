# Zero-Copy Vulkan Rendering - Required Modifications

## Current State
- GBM→scanout works (test_gbm shows red via gbm_bo_map + drmModeSetCrtc)
- Vulkan rendering works (via HOST_VISIBLE + CPU memcpy to GBM)
- **Missing**: Direct GBM→Vulkan import (zero-copy)

## Problem
Venus/macOS doesn't expose Linux DMA-BUF extensions:
- ❌ `VK_KHR_external_memory_fd` - not available
- ❌ `VK_EXT_external_memory_dma_buf` - not available
- ❌ `VK_EXT_image_drm_format_modifier` - not available

macOS has no DMA-BUF concept - uses IOSurface instead.

## Required Modifications

### Option A: Fake DMA-BUF Extensions in Venus (virglrenderer)

1. **Expose VK_KHR_external_memory_fd in Venus**
   - File: `/opt/other/virglrenderer/src/venus/vkr_physical_device.c`
   - Already partially done for HOST_VISIBLE memory via SHM
   - Need to extend to handle OPAQUE_FD / DMA_BUF handle types

2. **Expose VK_EXT_external_memory_dma_buf**
   - File: `/opt/other/virglrenderer/src/venus/vkr_physical_device.c`
   - Map DMA_BUF imports to SHM-backed memory internally
   - Guest thinks it's DMA-BUF, host uses VK_EXT_external_memory_host

3. **Expose VK_EXT_image_drm_format_modifier**
   - File: `/opt/other/virglrenderer/src/venus/vkr_image.c`
   - Handle DRM format modifier queries
   - Map LINEAR modifier (0) to standard tiling

4. **Handle vkGetMemoryFdKHR / vkGetMemoryFdPropertiesKHR**
   - File: `/opt/other/virglrenderer/src/venus/vkr_device_memory.c`
   - Return SHM fd that can be imported by GBM

### Option B: Use Existing SHM Path + GBM Import

1. **Modify GBM to accept SHM-backed buffers**
   - Mesa's GBM implementation needs to import from SHM fd
   - File: `/opt/other/mesa/src/gbm/backends/dri/gbm_dri.c`

2. **Create VkImage with HOST_VISIBLE, get SHM fd, import to GBM**
   - Reverse direction: Vulkan→GBM instead of GBM→Vulkan
   - Render to HOST_VISIBLE VkImage
   - Export fd via virglrenderer's SHM mechanism
   - Import into GBM for scanout

### Option C: Host-Side Compositing (Current Approach Enhanced)

1. **Keep current memcpy path but optimize**
   - Use async copy with double-buffering
   - Minimize stalls with proper synchronization

2. **Implement proper swapchain in QEMU**
   - File: `/opt/other/qemu/hw/display/virtio-gpu-vk-swapchain.m`
   - Host receives SET_SCANOUT_BLOB
   - Host copies blob to MTLTexture
   - Host presents via CAMetalLayer

## Recommended Path: Option A

Fake the DMA-BUF extensions in virglrenderer Venus backend:

```
Guest App
    ↓
gbm_bo_create(SCANOUT|RENDERING)
    ↓
gbm_bo_get_fd() → returns virtio-gpu blob fd
    ↓
vkImportMemoryFdKHR(DMA_BUF) → Venus intercepts
    ↓ (virglrenderer)
Map to SHM-backed VkDeviceMemory via VK_EXT_external_memory_host
    ↓
Render to VkImage (backed by same SHM)
    ↓
drmModeSetCrtc() → scanout same memory
```

## Files to Modify

| Component | File | Change |
|-----------|------|--------|
| virglrenderer | `src/venus/vkr_physical_device.c` | Advertise DMA-BUF extensions |
| virglrenderer | `src/venus/vkr_device_memory.c` | Handle DMA-BUF import via SHM |
| virglrenderer | `src/venus/vkr_image.c` | Support DRM format modifiers |
| Mesa (optional) | `src/virtio/vulkan/vn_physical_device.c` | Client-side extension handling |

## Testing

After modifications:
```bash
# In guest
cd /root/triangle
./build.sh
./test_tri  # Should show triangle with zero copy
```

## References
- Venus external memory: `vkr_device_memory.c:vkr_dispatch_vkAllocateMemory`
- SHM fallback: Already implemented for HOST_VISIBLE
- Extension filtering: `vkr_physical_device.c:vkr_physical_device_init`
