# Zero-Copy Vulkan Rendering - Status & Next Steps

## Summary

✅ **MILESTONE ACHIEVED** (2026-01-22): Triangle rendering displays successfully!

**Current Working Path**: HOST_VISIBLE memory + CPU copy to GBM + drmModeDirtyFB + **XRGB8888 format**

**True Zero-Copy Blocked By**: virtio-gpu/Venus resource ID mismatch

## Current State (2026-01-22)

### Completed ✅
- **VK_KHR_external_memory_fd** - Exposed via SHM fallback
- **VK_EXT_external_memory_dma_buf** - Exposed via SHM fallback
- **VK_EXT_image_drm_format_modifier** - Exposed (LINEAR modifier)
- **Extension filtering** - Faked extensions filtered from MoltenVK device creation
- **VK_KHR_portability_subset** - Auto-added for MoltenVK
- **GBM allocation path bypass** - Skipped when use_host_pointer_import=true
- **Resource import translation** - VkImportMemoryResourceInfoMESA → VkImportMemoryHostPointerInfoEXT
- **🎯 GBM format fix (2026-01-22)** - Changed ARGB8888 → XRGB8888 for scanout compatibility
  - **Problem**: `drmModeSetCrtc` returned -22 (EINVAL)
  - **Root Cause**: virtio-gpu doesn't support alpha channel formats for DRM scanout
  - **Solution**: Use `GBM_FORMAT_XRGB8888` instead of `GBM_FORMAT_ARGB8888`
  - **Result**: Triangle demo now displays successfully! ✅

### Test Results
test_minimal: SUCCESS (instance, device, fence creation)

/opt/other/qemu/ ssh -p 2222 root@localhost /root/test_tri
Starting...
Opened DRM fd=3
Became DRM master
Got resources res=0x7fff9306ba90
Found connector
Display: 1280x800
Got encoder, crtc_id=36
Creating GBM device...
GBM device=0x7fff9309f0f0
Creating GBM bo...
GBM bo=0x7fff92627510
Getting stride...
stride=5120
Getting fd...
GBM blob: stride=5120, prime_fd=6
Creating DRM framebuffer...
handle=1
Created framebuffer fb_id=42
Creating Vulkan instance...
vkCreateInstance returned 0
Enumerating physical devices...
vkEnumeratePhysicalDevices returned 0, count=1
Getting device properties...
GPU: Virtio-GPU Venus (Apple M2 Pro)
Getting memory properties...
Memory types: 3, heaps: 1
Creating device...
vkCreateDevice returned 0
Getting device queue...
Queue=0x7fff925f1570
Creating VkImage...
vkCreateImage returned 0
Getting image memory requirements...
Image memory: size=4096000, alignment=16, typeBits=0x3
  MemType 0: flags=0x1 heap=0 (compatible)
  MemType 1: flags=0xf heap=0 (compatible)
  MemType 2: flags=0x11 heap=0 
Finding HOST_VISIBLE memory type...
Using memory type: 1 (HOST_VISIBLE)
vkAllocateMemory returned 0
Binding memory...
vkBindImageMemory returned 0
Mapping render memory...
vkMapMemory returned 0
Done with memory setup (HOST_VISIBLE + copy mode)
Creating image view...
vkCreateImageView returned 0
Creating render pass...
vkCreateRenderPass returned 0
Creating framebuffer...
vkCreateFramebuffer returned 0
Loading shaders...
vs_code=0x7fff925f2d20 vs_size=1504, fs_code=0x7fff92614da0 fs_size=572
Creating shader modules...
vkCreateShaderModule (vert) returned 0
vkCreateShaderModule (frag) returned 0
Creating pipeline layout...
vkCreatePipelineLayout returned 0
Creating graphics pipeline...
vkCreateGraphicsPipelines returned 0
Creating command pool...
vkCreateCommandPool returned 0
Allocating command buffer...
vkAllocateCommandBuffers returned 0
Creating fence...
vkCreateFence returned 0
Starting render...
Rendered triangle
Copying rendered content to GBM buffer...
Copying: VK pitch=5120, GBM stride=5120
Copied to GBM buffer successfully!
Setting DRM scanout...
  crtc_id=36, fb_id=42
  connector_id=37
  mode: 1280x800 (1280x800 @ 60Hz)
drmModeDirtyFB succeeded - buffer marked for display
drmModeSetCrtc succeeded!
RGB triangle on blue (5s)

✅ **Display verified**: RGB triangle visible in QEMU window for 5 seconds!


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

## Current Challenge: Zero-Copy Path

✅ **Display Verified**: Triangle successfully renders and displays via HOST_VISIBLE + copy path

❌ **Zero-Copy Blocked**: Direct GBM→VkImage import fails due to resource ID mismatch

## Next Steps

### 1. ✅ Display Working (DONE)
Triangle demo successfully displays via HOST_VISIBLE memory + CPU copy path.
See commit `e5f5f0a880` for details.

### 2. Investigate Memory Sharing (For Zero-Copy)
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

## To Investigate (For Zero-Copy)
- ✅ ~~drmModeSetCrtc -22 error~~ - FIXED: Use XRGB8888 instead of ARGB8888
- Check QEMU's virtio-gpu scanout handling for blob resources
- Trace the full import path from GBM fd to VkDeviceMemory
- Verify blob resource lifecycle (create, import, scanout)
- Fix VkExternalMemoryBufferCreateInfo for internal Venus buffers
- Resolve resource ID mismatch between virtio-gpu blobs and Venus resources

## Working Demos

### test_tri ✅ (HOST_VISIBLE + Copy)
- **File**: `guest-demos/triangle/test_tri.c`
- **Status**: Working - displays RGB triangle on blue background
- **Method**: LINEAR HOST_VISIBLE VkImage → memcpy to GBM XRGB8888 → drmModeSetCrtc
- **Performance**: ~1 frame (5s display), acceptable for proof-of-concept

### vkcube ❌ (Zero-Copy Attempt)
- **File**: `guest-demos/vkcube/vkcube_anim.c`
- **Status**: Fails with VK_ERROR_DEVICE_LOST
- **Method**: Attempts external memory import (GBM prime FD → VkImage)
- **Blocker**: Resource ID mismatch
- **TODO**: Refactor to use HOST_VISIBLE + copy path like test_tri