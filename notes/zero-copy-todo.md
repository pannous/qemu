# Zero-Copy Vulkan Rendering - Status & Next Steps

## Summary

**Current Working Path**: HOST_VISIBLE memory + CPU copy to GBM + drmModeDirtyFB

**True Zero-Copy Blocked By**: virtio-gpu/Venus resource ID mismatch

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

## CRITICAL: Test Is Not Zero-Copy!

**The current test_tri.c does NOT implement zero-copy.** It creates separate memory:

```c
// Line 85: Gets GBM fd
int prime_fd = gbm_bo_get_fd(bo);

// Line 245-247: CLOSES the fd without importing!
close(prime_fd);  // <-- fd never used for Vulkan import!
```

The test creates:
1. GBM buffer (for DRM scanout) - one memory region
2. Vulkan image with DEVICE_LOCAL memory - **separate** memory region

These are NOT sharing memory, so the triangle rendered to Vulkan is NOT visible on the DRM scanout.

## Next Steps: Implement Actual Zero-Copy

### Option 1: Import GBM fd as Vulkan Memory (True Zero-Copy)

Modify test_tri.c to import the GBM prime_fd:

```c
// Instead of closing prime_fd, import it as Vulkan memory
VkImportMemoryFdInfoKHR import_info = {
    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    .fd = prime_fd
};

VkMemoryAllocateInfo alloc_info = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .pNext = &import_info,
    .allocationSize = mem_req.size,
    .memoryTypeIndex = mem_type
};

VkDeviceMemory render_mem;
vkAllocateMemory(device, &alloc_info, NULL, &render_mem);
// fd ownership transfers to Vulkan - don't close it!
```

### Option 2: Use HOST_VISIBLE Memory + CPU Copy

Keep current approach but force HOST_VISIBLE memory and copy to GBM:

```c
// Use HOST_VISIBLE memory type
uint32_t mem_type = find_mem(&mem_props, mem_req.memoryTypeBits,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

// After rendering, copy to GBM
void *gbm_map = gbm_bo_map(...);
memcpy(gbm_map, render_ptr, size);
gbm_bo_unmap(...);
```

## Current Challenge: Display Verification

Since test_tri.c is not doing zero-copy, the QEMU display shows the uninitialized
GBM buffer (likely black or garbage), not the rendered triangle

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

## Current Blocker: Resource ID Mismatch

When trying to import GBM fd as Vulkan memory:
```
vkr: failed to import resource: invalid res_id 25
vkr: vkAllocateMemory resulted in CS error
```

**Root Cause**: The GBM buffer is created as a virtio-gpu blob resource, but when
the Mesa Venus driver tries to import it, it generates a resource ID that doesn't
exist in the virglrenderer Venus context.

**Why This Happens**:
1. Guest creates GBM buffer via virtio-gpu → creates blob resource (e.g., ID 25)
2. Guest calls `gbm_bo_get_fd()` → returns fd 6
3. Guest calls `vkAllocateMemory` with `VkImportMemoryFdInfoKHR(fd=6)`
4. Mesa Venus driver intercepts → creates `VkImportMemoryResourceInfoMESA(resourceId=25)`
5. virglrenderer's `vkr_context_get_resource(ctx, 25)` → returns NULL (not found!)

The issue is that virtio-gpu resources and Venus resources are in separate namespaces.
The Venus context doesn't know about virtio-gpu blob resources.

**Possible Fixes**:
1. **Resource sharing**: Make Venus aware of virtio-gpu blob resources
2. **Different import path**: Use `VK_EXT_external_memory_host` directly with mmap'd GBM fd
3. **QEMU integration**: Have QEMU register virtio-gpu blobs with virglrenderer Venus context

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
