# Venus Instance Creation Failure - Analysis

**Date:** 2026-01-25
**Status:** ❌ BROKEN - All Vulkan demos fail with vkCreateInstance error

## Symptom

All demos (copyback and zero-copy) fail with:
```
VK err -1 @ vkCreateInstance
virtio_gpu_virgl_process_cmd: ctrl 0x208, error 0x1200
```

- Error -1 = VK_ERROR_OUT_OF_HOST_MEMORY
- ctrl 0x208 = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB
- error 0x1200 = VIRTIO_GPU_RESP_ERR_UNSPEC

## What Works

✅ Host Vulkan swapchain initializes: "Venus: Host Vulkan swapchain initialized (1280x800)"
✅ Venus context creates successfully (VKR_RING_MONITOR starts)
✅ MoltenVK is accessible and working on host

## What Fails

❌ Guest vkCreateInstance always returns -1
❌ Blob resource creation fails in virglrenderer
❌ Warning: "Failed to register blob resource X with Venus context Y"

## Root Cause

In `hw/display/virtio-gpu-virgl.c:1588`:
```c
if (!virgl_try_register_venus_resource(cblob.hdr.ctx_id, cblob.resource_id)) {
    warn_report_once("Failed to register blob resource %d with Venus context %u",
                     cblob.resource_id, cblob.hdr.ctx_id);
}
```

This calls `virgl_renderer_resource_register_venus` which calls:
`vkr_renderer_get_or_import_resource(ctx_id, res_id)`

Which calls: `vkr_context_get_resource_or_import(ctx, res_id)`

At `/opt/other/virglrenderer/src/venus/vkr_context.c:478-511`, this function:
1. Looks up existing resource - fails
2. Tries to import from virgl_resource - succeeds
3. Calls `virgl_resource_export_fd` - **returns VIRGL_RESOURCE_FD_INVALID**
4. Returns NULL, causing registration to fail

## Investigation Attempts

### Tried
1. ✅ Reverted virglrenderer to f48b5b19 (milestone zero-copy) - still broken
2. ✅ Reverted QEMU to pre-merge 714a6b6dbf - still broken
3. ✅ Tried older virglrenderer commits - build failures (threads.h missing)
4. ✅ Confirmed custom virglrenderer loads correctly (2.9MB vs 2.9MB homebrew)
5. ✅ Tested all demo variants (test_tri, vkcube_anim, vkcube_zerocopy) - all fail identically

### Not Environmental
- MoltenVK version: unchanged
- Vulkan loader: working (host swapchain works)
- System libraries: no updates
- Disk: not corrupted (VM boots fine)

## Code Path

```
Guest: vkCreateInstance()
  ↓
Venus Protocol: Send command to host
  ↓
virglrenderer: Process Vulkan command (needs blob resource for internal structures)
  ↓
QEMU: virgl_cmd_resource_create_blob (ctrl 0x208)
  ↓
virglrenderer: virgl_renderer_resource_create_blob
  ↓
Venus: vkr_context_create_resource_from_device_memory
  ↓
Tries to find VkDeviceMemory with blob_id
  ↓
**FAILS** - object doesn't exist or can't be exported
  ↓
Returns error to QEMU (0x1200 = VIRTIO_GPU_RESP_ERR_UNSPEC)
  ↓
Guest: vkCreateInstance returns -1
```

## Hypothesis

The blob resource export mechanism in virglrenderer is broken. Specifically:
- `virgl_resource_export_fd()` returns `VIRGL_RESOURCE_FD_INVALID`
- This prevents `vkr_context_get_resource_or_import()` from succeeding
- Without successful import, Venus can't create Vulkan objects

## Next Steps

1. Add debug logging to `virgl_resource_export_fd` to see why it returns INVALID
2. Check if blob resources are created with correct fd_type
3. Verify SHM allocation is working correctly
4. Consider if macOS-specific fd export is broken

## Timeline

- **Jan 22, 2026**: Demos working (273 FPS vkcube_anim confirmed)
- **Jan 23-24, 2026**: Multiple commits to virglrenderer (zero-copy work)
- **Jan 25, 2026**: All demos broken, even after reverting commits

**This suggests the issue isn't in recent commits but something environmental that changed between Jan 22-25.**
