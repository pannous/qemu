# Venus Fence Debugging Session - 2026-01-20

## Problem
vkQueueSubmit succeeds but fence never signals, causing vkWaitForFences to return VK_TIMEOUT.

## Confirmed Working
- Venus protocol communication
- Instance/Device creation
- Queue retrieval (uses vkGetDeviceQueue2 correctly, not vkGetDeviceQueue)
- vkCreateFence
- vkQueueSubmit (returns VK_SUCCESS)

## Test Output (Guest)
```
1. Instance OK
2. PhysDev OK
3. Device OK
4. Queue OK
5. Fence created
6. Submit: 0  (VK_SUCCESS)
7. Status: 1  (VK_NOT_READY)
8. Wait: 2    (VK_TIMEOUT)
-> crash: "stuck in ring seqno wait"
```

## Key Discovery: Suspicious MoltenVK Fence Handle

**Host debug output:**
```
[VKR] CreateFence: calling MoltenVK with device=0xa810d6818
[VKR] CreateFence: MoltenVK returned 0, fence=0x50000000005 (0x50000000005)
```

The fence handle `0x50000000005` is:
- The SAME value returned across ALL tests/devices
- NOT a valid pointer (direct MoltenVK tests return handles like 0xb5f080000)
- Suspicious encoding: (5 << 32) | 5

**Direct MoltenVK test (works):**
```
Device: 0xb5f044018
Fence1: 0xb5f080000  <- Normal pointer-like handle
Fence2: 0xb5f0800a0
Fence3: 0xb5f0801e0
Wait: 0 (VK_SUCCESS)  <- Fence SIGNALS correctly!
```

## Mesa Venus Driver Fence Mechanism

### Feedback Mechanism
Mesa uses a "feedback" system for fence polling optimization:

1. **Fence Creation** (`vn_queue.c:1560-1615`):
   - Allocates a feedback slot (shared memory)
   - Creates feedback command buffers per queue family
   - Sets `fence->feedback.pollable = true`

2. **Queue Submit** (`vn_queue.c:510-515`):
   ```c
   if (fence && fence->feedback.slot) {
      if (queue->can_feedback)
         submit->feedback_types |= VN_FEEDBACK_TYPE_FENCE;
      else
         fence->feedback.pollable = false;
   }
   ```

3. **Fence Status Check** (`vn_queue.c:1744-1763`):
   ```c
   if (fence->feedback.pollable) {
      result = vn_feedback_get_status(fence->feedback.slot);
   } else {
      result = vn_call_vkGetFenceStatus(dev->primary_ring, device, _fence);
   }
   ```

### Fence Wait (`vn_queue.c:1832-1870`)
Does NOT call host vkWaitForFences directly! Instead:
- Polls `vn_GetFenceStatus()` in a loop
- Uses `vn_relax_state` for sleep/retry

### can_feedback Check
`vn_queue_family_can_feedback()` requires queue to have GRAPHICS|COMPUTE|TRANSFER capability.
Apple M2 Pro should support this.

## Host-Side Code Flow (virglrenderer)

### Fence Creation (`vkr_queue_gen.h:16-34`)
```c
vn_replace_vkCreateFence_args_handle(args);  // Replace device handle
args->ret = vk->CreateFence(args->device, args->pCreateInfo, NULL,
                            &obj->base.handle.fence);  // Store MoltenVK handle
```

### Queue Submit (`vkr_queue.c:382-407`)
```c
vn_replace_vkQueueSubmit_args_handle(args);  // Translate fence: guest->host
vk->QueueSubmit(args->queue, args->submitCount, args->pSubmits, args->fence);
```

### Handle Translation (`vn_protocol_renderer_cs.h:153-158`)
```c
static inline uint64_t vn_cs_get_object_handle(const void **handle, VkObjectType type) {
   const struct vkr_object *obj = *(const struct vkr_object **)handle;
   return obj ? obj->handle.u64 : 0;
}
```

## Theories

### Theory 1: MoltenVK Handle Encoding Issue
MoltenVK returns `0x50000000005` which may be valid in some contexts but not usable as a fence.
- Could be an object pool index rather than pointer
- May require special handling on macOS
- **Test needed**: Check MoltenVK source for fence handle format

### Theory 2: Feedback Command Not Executing
The feedback mechanism requires:
1. Feedback command buffer in submission
2. Host GPU executes command
3. Writes to shared memory
4. Guest polls shared memory

If host doesn't process feedback commands, fence stays VK_NOT_READY.

### Theory 3: Ring Communication Broken
The "stuck in ring seqno wait" error suggests the ring protocol between guest and host is broken after the fence timeout. This may be a symptom, not the cause.

## Files with Debug Prints

### virglrenderer (already added)
- `/opt/other/virglrenderer/src/venus/vkr_queue.c`:
  - vkr_dispatch_vkGetDeviceQueue2 (line 327)
  - vkr_dispatch_vkQueueSubmit (line 389-407)
  - vkr_dispatch_vkCreateFence (line 449)
  - vkr_dispatch_vkWaitForFences (line 504)

- `/opt/other/virglrenderer/builddir/src/vkr_queue_gen.h`:
  - vkr_fence_create_driver_handle (line 27-32) - shows MoltenVK call and return

## Debug Configuration

```bash
# In run-alpine.sh (already enabled):
export VKR_DEBUG=all
export MVK_CONFIG_LOG_LEVEL=2

# Custom render_server path:
export RENDER_SERVER_EXEC_PATH=/opt/other/virglrenderer/builddir/server/virgl_render_server
```

## SOLUTION FOUND!

**Root Cause: Feedback mechanism is broken, not fences themselves.**

### Working Test
```bash
VN_PERF=no_fence_feedback /tmp/test_fence
```
Output:
```
Wait: 0   # VK_SUCCESS - fence signals!
[VKR] vkGetFenceStatus: ret=1  # NOT_READY initially
[VKR] vkGetFenceStatus: ret=0  # SUCCESS after polling
```

### Why Feedback Fails
The feedback mechanism uses:
1. Feedback command buffers that write to shared memory when GPU work completes
2. Guest polls shared memory instead of calling host

When feedback is enabled (default), the feedback command doesn't execute/write properly.
When feedback is disabled, direct vkGetFenceStatus polling to host works.

## Next Steps to Fix

1. **Investigate feedback command execution**
   - Why aren't feedback commands writing to shared memory?
   - Is the host executing feedback command buffers?

2. **Check feedback command buffer content**
   - `vn_feedback_cmd_alloc()` creates these commands
   - They use `vkCmdFillBuffer` or similar to write status

3. **Possible causes:**
   - Feedback command buffers not included in submission
   - Host not executing command buffers properly
   - Shared memory mapping issues between guest/host

4. **Temporary workaround:**
   - Add `VN_PERF=no_fence_feedback` to guest environment
   - This forces polling path which works

## Key Source Files

| Location | Purpose |
|----------|---------|
| `/opt/other/mesa/src/virtio/vulkan/vn_queue.c` | Guest fence/queue handling |
| `/opt/other/mesa/src/virtio/vulkan/vn_feedback.c` | Feedback mechanism |
| `/opt/other/virglrenderer/src/venus/vkr_queue.c` | Host queue dispatch |
| `/opt/other/virglrenderer/builddir/src/vkr_queue_gen.h` | Generated fence creation |
| `/opt/other/virglrenderer/src/venus/venus-protocol/vn_protocol_renderer_handles.h` | Handle translation |

## Environment

- macOS with Apple M2 Pro
- MoltenVK 1.4.0 (homebrew)
- Custom virglrenderer with Venus backend
- Guest: Alpine Linux aarch64 (TCG emulation)
