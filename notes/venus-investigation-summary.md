# Venus Queue Submission Investigation Summary

## Goal
Make vkcube work with Venus on macOS by implementing VK_KHR_swapchain support.

## Current Status: BLOCKED

Before implementing swapchain, discovered that **vkQueueSubmit + fence wait doesn't work**. Fences never get signaled, causing VK_TIMEOUT.

## What Works (Confirmed)
- Venus protocol - commands forwarded to MoltenVK
- Vulkan instance/device creation - "Virtio-GPU Venus (Apple M2 Pro)"
- HOST_VISIBLE memory via VK_EXT_external_memory_host + SHM
- vkMapMemory - SHM validation fix applied
- Blob resources - GBM creates blob-backed buffers
- DRM scanout - SET_SCANOUT_BLOB triggers display
- Host Vulkan swapchain - MoltenVK → CAMetalLayer

## What's Broken

### Issue 1: vkGetDeviceQueue sets context fatal
**Location**: `/opt/other/virglrenderer/src/venus/vkr_queue.c:359-366`
```c
static void vkr_dispatch_vkGetDeviceQueue(...) {
   /* Must use vkGetDeviceQueue2 for proper device queue initialization. */
   vkr_context_set_fatal(ctx);  // FATAL!
   return;
}
```
Venus **requires** `vkGetDeviceQueue2` because it needs `VkDeviceQueueTimelineInfoMESA` for ring_idx assignment.

### Issue 2: Queue Submission Fences Never Signal
```c
vkQueueSubmit(queue, 1, &si, fence);  // Returns VK_SUCCESS
vkWaitForFences(device, 1, &fence, VK_TRUE, 1000000000);  // Returns VK_TIMEOUT
```
Even an empty submit with fence times out.

## Code Flow Analysis

### Mesa Venus Driver (Guest)
**File**: `/opt/other/mesa/src/virtio/vulkan/vn_device.c`

The Mesa driver correctly uses `vkGetDeviceQueue2`:
```c
// Line 83-104 in vn_queue_init()
const int ring_idx = vn_instance_acquire_ring_idx(dev->instance);
const VkDeviceQueueTimelineInfoMESA timeline_info = {
   .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_TIMELINE_INFO_MESA,
   .ringIdx = queue->ring_idx,
};
vn_async_vkGetDeviceQueue2(dev->primary_ring, ...);
```

### virglrenderer (Host)
**File**: `/opt/other/virglrenderer/src/venus/vkr_device.c`

During `vkCreateDevice`, queues are created with `vkr_device_create_queues()`:
1. Creates host VkQueue via MoltenVK's `GetDeviceQueue`/`GetDeviceQueue2`
2. Creates `vkr_queue` object (ID not set yet)
3. Queues tracked in `dev->queues` list

**File**: `/opt/other/virglrenderer/src/venus/vkr_queue.c`

When guest calls `vkGetDeviceQueue2`:
1. `vkr_device_lookup_queue()` - finds queue by (flags, family, index)
2. `vkr_queue_assign_ring_idx()` - sets ring_idx from `VkDeviceQueueTimelineInfoMESA`
3. `vkr_queue_assign_object_id()` - assigns guest handle ID

### Handle Translation
**File**: `/opt/other/virglrenderer/src/venus/venus-protocol/vn_protocol_renderer_handles.h`

```c
vn_decode_VkFence_lookup()  // Guest ID → vkr_fence* pointer
vn_replace_VkFence_handle() // vkr_fence* → Host VkFence handle
```

Queue submit and fence wait correctly translate handles before calling MoltenVK.

### Sync Thread Mechanism
**File**: `/opt/other/virglrenderer/src/venus/vkr_queue.c:151-196`

Each queue has a sync thread (`vkr_queue_thread`) that:
1. Waits for sync entries on `queue->sync_thread.syncs`
2. Calls `vk->WaitForFences()` with 3-second timeout
3. On signal, calls `vkr_queue_sync_retire()` to notify guest

## Key Investigation Files

| File | Purpose |
|------|---------|
| `/opt/other/virglrenderer/src/venus/vkr_queue.c` | Queue submission, fence handling |
| `/opt/other/virglrenderer/src/venus/vkr_device.c` | Device and queue creation |
| `/opt/other/virglrenderer/src/venus/vkr_device_memory.c` | Memory allocation with host pointer fallback |
| `/opt/other/mesa/src/virtio/vulkan/vn_device.c` | Guest-side queue initialization |
| `/opt/other/mesa/src/virtio/vulkan/vn_queue.c` | Guest-side queue submission |

## Debug Configuration

**Run script**: `/opt/other/qemu/scripts/run-alpine.sh`

Enable debug output:
```bash
export VKR_DEBUG=all        # Venus debug
export MVK_CONFIG_LOG_LEVEL=2  # MoltenVK debug
```

**render_server** is forked at: `/opt/other/virglrenderer/src/proxy/proxy_server.c:77`
- Uses `RENDER_SERVER_EXEC_PATH` env var
- Inherits QEMU's stdout/stderr
- Path: `/opt/other/virglrenderer/build/server/virgl_render_server`

## Root Cause Theories

1. **Context becomes fatal early** - If vkGetDeviceQueue is called before vkGetDeviceQueue2, context is poisoned
2. **Ring idx / timeline sync issue** - Guest ring_idx may not match host expectations
3. **Fence handle translation** - Guest fence ID might not map to correct host fence
4. **Async fence signaling** - Venus expects async fence processing that may not work on macOS
5. **render_server communication** - Commands may not reach render_server correctly

## Immediate Next Steps

1. **Capture render_server output**
   ```bash
   # In run-alpine.sh, uncomment:
   export VKR_DEBUG=all
   ```

2. **Add debug logging to vkr_dispatch_vkQueueSubmit**
   ```c
   // vkr_queue.c:369
   fprintf(stderr, "[VKR] vkQueueSubmit: fence=%p (guest_id=%llu)\n",
           args->fence, (unsigned long long)fence_id);
   ```

3. **Check if guest calls vkGetDeviceQueue or vkGetDeviceQueue2**
   - If vkGetDeviceQueue is called, context is fatal and subsequent commands fail silently

4. **Test fence status polling**
   - In guest test, poll `vkGetFenceStatus()` to see if fence ever signals

## Alternative Approaches

If queue sync can't be fixed:

1. **Software rendering fallback** - Use HOST_VISIBLE memory directly (works), CPU-copy to GBM buffers (works), scanout via DRM (works). Loses GPU parallelism but avoids fence sync.

2. **WSI Shim Layer** (Medium complexity) - Intercept swapchain calls in guest, map to DRM framebuffers, use existing blob scanout path. Guest unaware.

## For Swapchain Implementation (after fixing queue submit)

**File**: `/opt/other/qemu/notes/venus-swapchain-implementation.md`

Need to:
1. Expose VK_KHR_swapchain extension in virglrenderer
2. Create vkr_swapchain object with IOSurface backing
3. Intercept swapchain commands and proxy to host CAMetalLayer

## Test Commands (in guest)

```bash
# Verify Venus works
vulkaninfo | grep "Virtio-GPU Venus"

# Test memory mapping
/tmp/test_mem

# Test blob scanout (displays blue screen)
/tmp/test_blob
```

## Environment

```bash
# MoltenVK ICD
export VK_ICD_FILENAMES=/opt/homebrew/Cellar/molten-vk/1.4.0/etc/vulkan/icd.d/MoltenVK_icd.json

# Custom virglrenderer
export DYLD_LIBRARY_PATH=/opt/other/virglrenderer/install/lib:/opt/homebrew/lib

# Custom render_server
export RENDER_SERVER_EXEC_PATH=/opt/other/virglrenderer/build/server/virgl_render_server
```
