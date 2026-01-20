# Venus Queue Submit Investigation (2026-01-20)

## Goal
Make vkcube work with Venus on macOS by implementing VK_KHR_swapchain support.

## Current Blocker
Before implementing swapchain, discovered that **vkQueueSubmit + fence wait doesn't work**. Fences never get signaled, causing timeout.

## Findings

### 1. Memory Mapping Works
HOST_VISIBLE memory and vkMapMemory work correctly:
```
vkAllocateMemory: 0
vkMapMemory: 0 ptr=0xffff...
write OK!
```

### 2. Object Creation Works
- vkCreateInstance ✓
- vkCreateDevice ✓
- vkCreateFence ✓
- vkCreateCommandPool ✓
- vkCreateRenderPass ✓
- vkCreateGraphicsPipelines ✓

### 3. Queue Submission Fails
```c
vkQueueSubmit(queue, 1, &si, fence);  // Returns VK_SUCCESS
vkWaitForFences(device, 1, &fence, VK_TRUE, 1000000000);  // Returns VK_TIMEOUT (2)
```

Even an empty submit with fence times out.

### 4. vkGetDeviceQueue Issue
In `/opt/other/virglrenderer/src/venus/vkr_queue.c:359`:
```c
static void vkr_dispatch_vkGetDeviceQueue(...) {
   /* Must use vkGetDeviceQueue2 for proper device queue initialization. */
   vkr_context_set_fatal(ctx);  // Sets context fatal!
}
```

When guest calls `vkGetDeviceQueue`, virglrenderer **sets the context fatal**. All subsequent commands may silently fail.

However, using `vkGetDeviceQueue2` causes a **segfault** on the guest side.

### 5. Host QueueSubmit Not Returning Results
Added debug logging:
```c
fprintf(stderr, "[VKR] vkQueueSubmit: calling host...\n");
vk->QueueSubmit(...);
fprintf(stderr, "[VKR] vkQueueSubmit: ret=%d\n", args->ret);  // Never seen
```

The "calling host" log appears but "ret=" never shows, suggesting:
- Host QueueSubmit hangs, OR
- Context is fatal before we reach the log, OR
- stderr from render_server not captured in main QEMU log

### 6. Host MoltenVK Works Directly
Tested MoltenVK on host outside of QEMU:
```
vkQueueSubmit → fence signaled → vkWaitForFences: VK_SUCCESS
```
MoltenVK itself works correctly.

## Root Cause Theories

1. **Context Fatal**: The vkGetDeviceQueue path sets context fatal, breaking subsequent commands
2. **Fence Handle Translation**: Guest/host fence handle mapping might be wrong
3. **Async Fence Signaling**: Venus expects async fence processing that isn't working on macOS
4. **render_server Communication**: Commands might not be reaching the render_server

## Debug Code Added
- `/opt/other/virglrenderer/src/venus/vkr_queue.c`
  - `vkr_dispatch_vkGetDeviceQueue`: Added FATAL warning
  - `vkr_dispatch_vkQueueSubmit`: Added fence translation and return logging
  - `vkr_dispatch_vkWaitForFences`: Added fence and timeout logging

## Next Steps

### Immediate
1. **Capture render_server stderr**: The stderr from render_server needs to be redirected to see debug output
2. **Test vkGetDeviceQueue2**: Fix the segfault when using vkGetDeviceQueue2
3. **Add fence status polling**: Test if fence is ever signaled using vkGetFenceStatus in a loop

### If Context Fatal is the Issue
- Investigate why vkGetDeviceQueue sets fatal
- Possibly allow it for backward compatibility
- OR fix guest Mesa to use vkGetDeviceQueue2

### If Fence Signaling is the Issue
- Check fence creation/translation in `vkr_fence_create`
- Verify host fence is passed correctly to MoltenVK
- Check if external fence export is interfering (VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT)

### Alternative Approach
If queue submission can't be fixed easily:
- Implement a software fallback that:
  1. Uses HOST_VISIBLE memory directly (works)
  2. CPU-copies rendered data to GBM buffers (works)
  3. Scanouts via existing DRM path (works)

This avoids GPU queue synchronization but loses GPU parallelism.

## Files Modified
- `/opt/other/virglrenderer/src/venus/vkr_queue.c` - Debug logging added (needs cleanup)
- `/opt/other/qemu/notes/venus-swapchain-implementation.md` - Design doc created

## Test Programs
- `/tmp/vk_test.c` (in guest) - Basic fence test
- `/tmp/mvk_test.c` (on host) - Direct MoltenVK test
