# Venus Swapchain Support on macOS Progress

## Summary

Successfully patched Mesa to expose VK_KHR_swapchain extension on macOS with MoltenVK where sync_fd is not available.

## Changes Made

### 1. vn_device.c (line 304-308)

**Original:**
```c
/* see vn_queue_submission_count_batch_semaphores */
if (!app_exts->KHR_external_semaphore_fd && has_wsi) {
   assert(physical_dev->renderer_sync_fd.semaphore_importable);
   extra_exts[extra_count++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
}
```

**Patched:**
```c
/* see vn_queue_submission_count_batch_semaphores
 * Only add external semaphore fd when the renderer supports sync_fd import.
 * On macOS with MoltenVK, sync_fd isn't available but WSI can still work
 * via the fallback fence wait mechanism in vn_wsi_fence_wait().
 */
if (!app_exts->KHR_external_semaphore_fd && has_wsi &&
    physical_dev->renderer_sync_fd.semaphore_importable) {
   extra_exts[extra_count++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
}
```

### 2. vn_physical_device.c (lines 1172-1181)

**Original:**
```c
#ifdef VN_USE_WSI_PLATFORM
   if (physical_dev->renderer_sync_fd.semaphore_importable) {
      exts->KHR_incremental_present = true;
      exts->KHR_swapchain = true;
      exts->KHR_swapchain_mutable_format = true;
      exts->EXT_hdr_metadata = true;
      exts->EXT_swapchain_maintenance1 = true;
   }
```

**Patched:**
```c
#ifdef VN_USE_WSI_PLATFORM
   /* Enable swapchain unconditionally - WSI has fallback for no sync_fd */
   exts->KHR_swapchain = true;
   if (physical_dev->renderer_sync_fd.semaphore_importable) {
      exts->KHR_incremental_present = true;
      exts->KHR_swapchain_mutable_format = true;
      exts->EXT_hdr_metadata = true;
      exts->EXT_swapchain_maintenance1 = true;
   }
```

## Verified Results

1. **vulkaninfo now shows VK_KHR_swapchain** - extension revision 70
2. **VkDevice creation works** - No more assertion failures
3. **vkcube starts with WSI** - Selects GPU, creates swapchain

## Current Issue

vkcube gets stuck during rendering:
```
MESA-VIRTIO: debug: stuck in fence wait with iter at 1024
MESA-VIRTIO: debug: aborting on expired ring alive status at iter 1024
```

This is a Venus protocol ring timeout, not a swapchain extension issue. The rendering commands are being submitted but the fence wait never completes.

## Potential Causes for Fence Timeout

1. Host-side virglrenderer may not be responding to ring commands
2. MoltenVK may have issues with the operations Venus is requesting
3. The blob memory/scanout path may need additional host-side work

## Next Steps

1. Check virglrenderer debug output on host
2. Investigate if the issue is in Venus ring handling or MoltenVK
3. May need to debug host-side render_server process
