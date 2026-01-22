# Zero-Copy Vulkan Rendering - Status & Next Steps

## Current State (2025-01-22)

### Completed ✅
- **VK_KHR_external_memory_fd** - Exposed via SHM fallback
- **VK_EXT_external_memory_dma_buf** - Exposed via SHM fallback
- **VK_EXT_image_drm_format_modifier** - Exposed (LINEAR modifier)
- **Extension filtering** - Faked extensions filtered from MoltenVK device creation
- **VK_KHR_portability_subset** - Auto-added for MoltenVK
- **GBM allocation path bypass** - Skipped when use_host_pointer_import=true
- **Resource import translation** - VkImportMemoryResourceInfoMESA → VkImportMemoryHostPointerInfoEXT

### Test Results
```
test_minimal: SUCCESS (instance, device, fence creation)
test_tri:     Renders triangle successfully
              ERROR: Cannot copy - render memory not mapped (expected)
```

The "Cannot copy" error is **expected** - it's a CPU readback fallback that doesn't work for DEVICE_LOCAL memory. The actual GPU rendering completes successfully.

## Current Challenge: Display Verification

The triangle renders to a VkImage backed by a GBM buffer, but we need to verify:

1. **Is the rendered content actually visible on QEMU's display?**
   - The DRM framebuffer is set via `drmModeSetCrtc()`
   - But we can't visually verify from SSH

2. **Memory type issue**:
   - Image memory is DEVICE_LOCAL (type 0), not HOST_VISIBLE
   - This is correct for GPU rendering, but means no CPU access
   - The GBM→Vulkan import path may not be sharing the same backing memory

## Next Steps

### 1. Verify Display Output
```bash
# Check if QEMU window shows the triangle
# Look at QEMU's Cocoa window - should show RGB triangle on blue background
```

### 2. Investigate Memory Sharing
The current flow:
```
GBM bo (virtio-gpu blob)
    ↓
gbm_bo_get_fd() → returns fd
    ↓
vkAllocateMemory with VkImportMemoryFdInfoKHR(DMA_BUF)
    ↓
Venus intercepts → but memory type is DEVICE_LOCAL
```

**Question**: Is the VkImage actually backed by the GBM buffer's memory, or is it a separate allocation?

### 3. Debug Memory Import Path
Add tracing to verify:
- Is `VkImportMemoryResourceInfoMESA` being used?
- Is the resource ID mapping to the GBM buffer?
- Is `vkr_context_get_resource()` finding the right resource?

### 4. Alternative: Force HOST_VISIBLE Memory
Modify test_tri.c to use HOST_VISIBLE memory type:
```c
// Change from DEVICE_LOCAL preference to HOST_VISIBLE
uint32_t mem_type = find_memory_type(mem_reqs.memoryTypeBits,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
```

This would allow CPU verification but may not be true zero-copy.

## Architecture Summary

```
Guest (Alpine VM)                    Host (macOS)
─────────────────                    ─────────────
GBM buffer (SCANOUT)
    │
    ├─ gbm_bo_get_fd() ─────────────→ virtio-gpu blob resource
    │                                     │
    ├─ vkAllocateMemory(DMA_BUF) ───→ Venus intercepts
    │                                     │
    │                                     ↓
    │                                 SHM mmap fallback
    │                                     │
    │                                     ↓
    │                                 VkImportMemoryHostPointerInfoEXT
    │                                     │
    │                                     ↓
    ├─ vkCmdDraw() ─────────────────→ MoltenVK renders to VkImage
    │
    └─ drmModeSetCrtc() ────────────→ QEMU displays blob as scanout
```

## Files Modified

| File | Changes |
|------|---------|
| `virglrenderer/src/venus/vkr_physical_device.c` | Advertise fake DMA-BUF extensions |
| `virglrenderer/src/venus/vkr_device.c` | Filter faked extensions, add portability_subset |
| `virglrenderer/src/venus/vkr_device_memory.c` | Skip GBM path, translate resource import to SHM |

## Commits
- `7e0c24bd` feature: Expose DMA-BUF and DRM format modifier extensions on macOS
- `3243a2f8` fix: Filter faked DMA-BUF extensions for MoltenVK/macOS

## Open Questions

1. **Is the blob memory actually shared?** The GBM fd and Vulkan memory may be different allocations if the import path isn't fully connected.

2. **Scanout path**: When `drmModeSetCrtc()` is called with the GBM fb_id, does QEMU's virtio-gpu actually display it?

3. **Memory type mismatch**: Guest allocates DEVICE_LOCAL (for GPU perf), but SHM fallback uses HOST_VISIBLE. Are they compatible?

## Known Issues

### Vulkan Validation Warning
```
vkBindBufferMemory2(): pBindInfos[0].memory was created with handleType
VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT which is not set
in the VkBuffer VkExternalMemoryBufferCreateInfo::handleType
```

This is a spec violation: when binding memory created via host pointer import,
the buffer should have been created with `VkExternalMemoryBufferCreateInfo`
specifying the same handle type. Venus internal buffers don't do this.

**Impact**: This is a warning, not an error. The operation succeeds but is
technically non-compliant. May need to fix in Venus buffer creation path.

## To Investigate
- Check QEMU's virtio-gpu scanout handling for blob resources
- Trace the full import path from GBM fd to VkDeviceMemory
- Verify blob resource lifecycle (create, import, scanout)
- Fix VkExternalMemoryBufferCreateInfo for internal Venus buffers
