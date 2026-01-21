# Venus Swapchain Status - 2026-01-21

## Summary

VK_KHR_swapchain works with patched Mesa. WSI operations (acquire, present) work, but
**any command buffer containing vkCmdBindPipeline causes the host to hang** in MoltenVK's
vkQueueSubmit. This affects both graphics and compute pipelines.

## Root Cause Analysis

### The Problem
When a Vulkan command buffer containing `vkCmdBindPipeline` is submitted:
1. Guest records command buffer (works fine - commands are encoded locally)
2. Guest calls vkQueueSubmit (sends Venus protocol to host)
3. virglrenderer decodes protocol and calls host vkQueueSubmit
4. **MoltenVK hangs** in vkQueueSubmit - never returns

### What Works
- Pipeline creation (vkCreateGraphicsPipelines, vkCreateComputePipelines) ✅
- Command buffer recording with pipeline bind (local operation) ✅
- Clear-only render passes (no pipeline bind) ✅
- Image barriers and layout transitions ✅
- Semaphore signaling/waiting ✅
- All WSI operations (surface, swapchain, acquire, present) ✅

### What Fails
- Submitting command buffer with vkCmdBindPipeline (graphics) ❌
- Submitting command buffer with vkCmdBindPipeline (compute) ❌
- Any draw calls (which require pipeline bind) ❌
- vkcube render frame ❌

### Debug Output
```
Device OK
Image OK
Framebuffer OK
Shaders OK
Pipeline OK
Recording...
Submitting...
DBG: vn_queue_submit enter, batch_type=0 batch_count=1
  batch[0]: wait=0 cmd=1 signal=0
DBG: sync vn_call_vkQueueSubmit        <- Enters virglrenderer QueueSubmit
MESA-VIRTIO: debug: vn_ring_submit abort on fatal  <- Host never responds
```

### Evidence
1. Command recording completes (test_record_only test passes)
2. Pipeline creation completes (test_pipeline test passes)
3. Clear-only submission completes (test_clear test passes)
4. Pipeline bind submission hangs (test_bind test fails)
5. Same behavior with compute pipelines

## Technical Details

### Venus Command Flow for Pipeline Bind
```
Guest: vkCmdBindPipeline(cmdBuf, bindPoint, pipeline)
  → Venus encodes: VN_CMD_ENQUEUE(vkCmdBindPipeline, ...)
  → Stored in guest command buffer stream

Guest: vkQueueSubmit(queue, 1, &submit, fence)
  → Venus sends submit to host via virtio-gpu ring
  → virglrenderer receives, calls vkr_dispatch_vkQueueSubmit()
  → virglrenderer calls host vk->QueueSubmit()
  → MoltenVK should execute command buffer
  → **HANG: MoltenVK never returns**
```

### virglrenderer Code Path
In `vkr_queue.c:406`:
```c
mtx_lock(&queue->vk_mutex);
args->ret =
   vk->QueueSubmit(args->queue, args->submitCount, args->pSubmits, args->fence);  // HANGS HERE
mtx_unlock(&queue->vk_mutex);
```

## Attempted Mitigations

| Attempt | Result |
|---------|--------|
| VKR_DEBUG=validate | No validation errors before hang |
| MVK_CONFIG_DEBUG_MODE=1 | No debug output |
| MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1 | Still hangs |
| VN_PERF=no_async_queue_submit | Hang is more visible |
| Force QueueSubmit2→QueueSubmit1 | No effect |

## Hypothesis

The hang is in MoltenVK's command buffer execution, specifically when processing
the pipeline bind. Possible causes:

1. **Metal shader compilation** happening lazily during execution
   - Pipeline was created, but Metal PSO might compile on first use
   - Could be stuck in shader compiler

2. **MoltenVK bug** with certain render pass / pipeline combinations
   - The command buffer has: BeginRenderPass → BindPipeline → EndRenderPass
   - Something about this sequence might trigger a bug

3. **Thread deadlock** in MoltenVK
   - The host uses mutexes around QueueSubmit
   - MoltenVK might have internal locking that conflicts

4. **Metal command buffer state issue**
   - The recorded Metal commands might be in an invalid state

## Files Modified

| File | Change |
|------|--------|
| `vn_physical_device.c:1097-1157` | Gate fence/semaphore handles on sync_fd support |
| `vn_physical_device.c:1226` | Unconditionally enable KHR_swapchain |
| `vn_device.c:334` | Gate external_semaphore_fd on semaphore_importable |
| `vn_wsi.c:827-882` | Add fallback queue submit for acquire semaphore |
| `vn_queue.c` | Debug output for submit tracing |

## Next Steps

1. **Debug MoltenVK directly**
   - Build debug MoltenVK from source
   - Add tracing to MVKCommandBuffer::submit()
   - Check Metal command encoder state

2. **Try different shader/pipeline**
   - Use simplest possible vertex/fragment shaders
   - Remove vertex input (use gl_VertexIndex)
   - Try different render pass configurations

3. **Test with different Vulkan implementation**
   - Try running the same test natively on macOS
   - If native works but Venus doesn't, issue is in protocol translation
   - If native also hangs, issue is in MoltenVK

4. **Check MoltenVK issues**
   - Search MoltenVK GitHub for similar hangs
   - Check if there are known issues with Apple Silicon

## Environment

- Guest: Alpine Linux edge (aarch64)
- Custom Mesa: Built in Docker (alpine:edge aarch64)
- Host: macOS with MoltenVK 1.4.0
- QEMU: virtio-gpu-gl with venus=on, blob=on

## Test Commands

```bash
# Copy patched library to guest
scp -P 2222 /tmp/libvulkan_virtio.so root@localhost:/usr/lib/

# Run tests
ssh -p 2222 root@localhost 'VN_PERF=no_fence_feedback /tmp/test_clear'    # Works
ssh -p 2222 root@localhost 'VN_PERF=no_fence_feedback /tmp/test_pipeline' # Works
ssh -p 2222 root@localhost 'VN_PERF=no_fence_feedback /tmp/test_bind'     # Hangs
```
