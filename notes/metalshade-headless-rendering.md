# Metalshade Headless Rendering in Alpine Guest

**Date**: 2026-01-28
**Goal**: Run ShaderToy/GLSL shaders in headless Alpine VM via Vulkan Venus

## ✅ Achievements

### Successfully Completed
1. **Transferred metalshade** to Alpine guest (`/root/metalshade/`)
2. **Fixed Linux compatibility**:
   - Added missing `#include <array>` header
   - Updated hardcoded paths from `/opt/3d/metalshade` → `/root/metalshade`
   - Removed macOS frameworks from Makefile
3. **Compiled shaders**: `vert.spv` (1,696 bytes), `frag.spv` (8,144 bytes)
4. **Validated shaders** load successfully into Vulkan Venus
5. **Discovered VK_EXT_headless_surface** available in guest

### Key Technical Findings
- **Device**: Virtio-GPU Venus (Apple M2 Pro) works in guest
- **Headless extensions**: VK_EXT_headless_surface, VK_KHR_display available
- **GLFW not needed**: Can create Vulkan instances without window system
- **Shader validation**: SPIR-V validates with `spirv-val`, loads into VkShaderModule

## ⚠️ Current Blocker

**Virtgpu Ring Fatal Error** when submitting render commands:
```
MESA-VIRTIO: debug: stuck in ring seqno wait with iter at 4096
MESA-VIRTIO: debug: aborting on ring fatal error at iter 4096
```

**Root cause**: Venus/virtgpu expects images created with special extensions:
- `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`
- `VK_EXT_external_memory_dma_buf`
- `VK_KHR_external_memory_fd`
- External memory handles for zero-copy host-guest sharing

Plain `VK_IMAGE_TILING_OPTIMAL` images fail during queue submission.

## 🛤️ Possible Next Steps

### Path 1: Full Virtgpu Integration (Most Complex, Best Performance)
**Integrate with vkcube_zerocopy approach**

**Steps**:
1. Study `/root/vkcube_zerocopy.c` blob creation logic
2. Create images with `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`
3. Use external memory import:
   ```c
   VkExternalMemoryImageCreateInfo extMemInfo = {
       .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
       .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
   };
   ```
4. Allocate virtgpu blob via DRM:
   ```c
   struct drm_virtgpu_resource_create_blob create = {
       .blob_mem = VIRTGPU_BLOB_MEM_HOST3D,
       .blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE,
       .size = width * height * 4
   };
   ioctl(drm_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &create);
   ```
5. Import blob's FD into Vulkan via `vkAllocateMemory` with external memory
6. Render to blob-backed images
7. Set blob as DRM scanout for QEMU display

**Pros**: Zero-copy, direct display, matches vkcube performance
**Cons**: Complex DRM/virtgpu plumbing, ~500+ lines of code
**Files to reference**: `/root/vkcube_zerocopy.c`, `/root/vkcube_working.c`

---

### Path 2: Offscreen Compute Path (Simpler, CPU Readback)
**Render to staging buffer, read back to host memory**

**Steps**:
1. Create staging buffer with `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT`
2. Render to device-local image (keep existing code)
3. Use `vkCmdCopyImageToBuffer` to copy image → staging buffer
4. Map staging buffer, read pixels on CPU
5. Save to file (PPM/PNG) or analyze in memory

**Pros**: Simpler, works with basic Vulkan, good for testing
**Cons**: CPU readback overhead, no direct display
**Use case**: Shader validation, automated testing, frame capture

**Code skeleton**:
```c
// Create staging buffer
VkBufferCreateInfo bufInfo = {
    .size = WIDTH * HEIGHT * 4,
    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT
};
// After render, copy image to buffer
vkCmdCopyImageToBuffer(cmd, image, layout, stagingBuffer, ...);
// Map and read
void* data;
vkMapMemory(device, stagingMemory, 0, size, 0, &data);
// data now contains RGBA pixels
```

---

### Path 3: Port Metalshade to DRM/KMS (Medium Complexity)
**Rewrite metalshade.cpp to use DRM directly (no GLFW, no surfaces)**

**Steps**:
1. Remove GLFW dependency entirely
2. Use DRM/KMS for display like vkcube_zerocopy
3. Create swapchain manually using external memory blobs
4. Keep shader loading/pipeline code from metalshade
5. Use vkcube's render loop with metalshade's shader system

**Pros**: Native headless, reuses metalshade shader infrastructure
**Cons**: Significant refactoring, mixes C/C++
**Estimated effort**: ~2-3 hours

**Key changes**:
- `initWindow()` → `initDRM()` (open `/dev/dri/card0`)
- `createSurface()` → blob allocation
- `createSwapchain()` → external memory images
- Keep: shader loading, uniforms, descriptor sets

---

### Path 4: Test on Host macOS (Immediate Validation)
**Run metalshade on host where GLFW + MoltenVK work**

**Steps**:
1. `cd /opt/3d/metalshade`
2. `make clean && make`
3. `./run.sh`
4. Verify shaders render correctly on host
5. Use this as ground truth for guest implementation

**Pros**: Immediate visual confirmation shaders work
**Cons**: Doesn't solve guest rendering, but validates shader correctness
**Time**: 5 minutes

---

### Path 5: Hybrid Approach (Recommended)
**Combine multiple paths for incremental progress**

**Phase 1**: Validate on host (Path 4)
- Confirm shaders render correctly with GLFW/MoltenVK
- Capture reference frames

**Phase 2**: Implement CPU readback in guest (Path 2)
- Simple staging buffer approach
- Save frames to `/root/output_*.ppm`
- Compare with host reference frames

**Phase 3**: Optimize with virtgpu blobs (Path 1)
- Only if needed for performance
- Use DRM blobs for zero-copy

---

## 📁 Reference Files

### Guest VM
- `/root/metalshade/` - Metalshade source and shaders
- `/root/metalshade/{vert,frag}.spv` - Compiled SPIR-V shaders
- `/root/vkcube_zerocopy.c` - Working DRM/blob example
- `/root/shader_headless.c` - Our basic headless test (validates shaders)
- `/root/test_simple.c` - Minimal validation test (proven working)

### Host
- `/opt/3d/metalshade/` - Original metalshade with GLFW
- `/opt/other/qemu/notes/` - This file
- `/tmp/shader_nosurface.c` - Attempted full renderer (hits virtgpu error)

---

## 🎯 Recommended Next Action

**Start with Path 5 (Hybrid)**:

1. **Immediate** (5 min): Test on host to verify shaders work
   ```bash
   cd /opt/3d/metalshade && ./run.sh
   ```

2. **Short-term** (1 hour): Implement Path 2 (CPU readback)
   - Extend `/root/test_simple.c` with staging buffer
   - Render 10 frames, save to PPM files
   - Validates full pipeline without DRM complexity

3. **Long-term** (if needed): Path 1 (virtgpu blobs)
   - Only if CPU readback is too slow
   - Port vkcube's blob code to metalshade renderer

---

## 💡 Key Insights

1. **GLFW is not the blocker** - VK_EXT_headless_surface works fine
2. **Shaders are valid** - They compile and load successfully
3. **Virtgpu is the challenge** - Needs external memory for image sharing
4. **vkcube_zerocopy is the template** - Shows how to do it correctly
5. **CPU readback is viable** - Simple path that works now

---

## 🔗 Related Work

- **QEMU virtio-gpu**: `/opt/other/qemu/hw/display/virtio-gpu.c`
- **Mesa Venus driver**: `/opt/other/mesa/src/virtio/vulkan/`
- **vkcube demos**: `/root/demos/` in guest
- **MoltenVK on host**: `/opt/homebrew/Cellar/molten-vk/1.4.0/`

---

## ✍️ Notes

- Alpine VM has only 2GB RAM - be careful with large allocations
- Disk space tight - use `/tmp` for output files
- vkcube_zerocopy achieves 1100 FPS, shows Venus performance is excellent
- The shader (Bumped_Sinusoidal_Warp.frag) is complex - simpler shaders may work better for testing


## DRM Blob Integration Attempts (2026-01-28 PM)

Attempted to combine vkcube's DRM blob approach with shader rendering:

### What We Tried:
1. **DRM virtgpu blob creation** - ✅ Works (blob created successfully)
2. **CPU mapping via mmap** - ❌ Fails with EINVAL (known limitation)
3. **Prime FD export** - ✅ Works (got FD for external memory)
4. **Vulkan external memory import** - ❌ Fails with VK_ERROR_INCOMPATIBLE_DRIVER (-8)

### Root Cause:
Venus on macOS host doesn't support:
- DMA_BUF import for VK_IMAGE_TILING_LINEAR
- Host pointer import requires different setup
- Needs DRM_FORMAT_MODIFIER_EXT path (complex)

### Working Path:
vkcube_zerocopy works because it:
1. Uses DRM_FORMAT_MODIFIER_EXT (not LINEAR)
2. Queries supported modifiers first
3. Uses complex modifier-aware import
4. Falls back to LINEAR + copy if needed

### Pragmatic Next Step:
**Skip blob import, prove shader rendering works:**
1. Use regular VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
2. Render shaders successfully
3. Read back via staging buffer
4. THEN optimize with blobs if needed

The blob path is proven to work (vkcube), but it's orthogonal to proving shader rendering works.


## Cherry-Pick from Zero-Copy Branch (2026-01-28 Late PM)

Successfully extracted DRM-based shadertoy viewer from `zero-copy` branch!

### What We Got:
- **File**: `guest-demos/shadertoy/shadertoy_drm.c` (~618 lines)
- **Architecture**: DRM/GBM + LINEAR HOST_VISIBLE images + memcpy to scanout
- **Status on zero-copy**: Implemented but has Venus fence wait error

### Test Results with Metalshade Shaders:

✅ **Much Better Progress**:
1. ✅ DRM device opened
2. ✅ Vulkan instance created  
3. ✅ Device + queue created
4. ✅ Image + memory allocated (HOST_VISIBLE)
5. ✅ Image view created
6. ✅ Render pass created
7. ✅ Framebuffer created
8. ✅ Shaders loaded (vert.spv + frag.spv)
9. ✅ Uniform buffer created & mapped
10. ✅ Descriptor sets created & updated
11. ✅ Pipeline created
12. ✅ Command buffer recorded
13. ✅ Commands submitted to queue
14. ❌ **Hangs at fence wait**: `stuck in fence wait with iter at 1024`
15. ❌ **Ring abort**: `aborting on ring fatal error at iter 1024`

### Key Difference from Our Attempts:
- Our `shader_nosurface.c`: Failed at queue submit (ring error immediately)
- Zero-copy `shadertoy_drm.c`: Submits successfully, hangs waiting for GPU completion

### This Means:
- ✅ Command submission works!
- ✅ Venus accepts our commands
- ❌ GPU doesn't complete the work (or Venus doesn't poll properly)

### Comparison to Working test_tri:
- test_tri renders successfully (triangle displays)
- shadertoy_drm hangs at fence
- **Likely culprit**: Shader complexity or descriptor set mismatch

### Next Debug Steps:
1. Try simple gradient shader (not complex bump-mapped one)
2. Compare shadertoy_drm descriptor setup vs test_tri
3. Check if iChannel0 texture is causing issues
4. Try without uniform buffer first

This is the closest we've gotten to working shader rendering!


## SOLUTION FOUND (2026-01-28 Evening)

Successfully created **shadertoy_viewer.c** based on vkcube_anim architecture!

### Test Results:
- ✅ **test_simple shaders**: Works perfectly
- ✅ **shadertoy_simple shaders**: **405 FPS** with animated gradient!
- ❌ **shadertoy_vkcube shaders**: Blue screen only (binding mismatch)

### Root Cause of Blue Screen:
The viewer creates ONE descriptor binding at `binding = 0`:
```c
.pBindings=&(VkDescriptorSetLayoutBinding){
    0,  // binding = 0 ← CRITICAL!
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
    VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT
}
```

Shaders MUST match this:
- ✅ `binding = 0` → Works (405 FPS)
- ❌ `binding = 1` → Blue screen (reading wrong/nonexistent data)

### Files:
- `/opt/other/qemu/guest-demos/shadertoy/shadertoy_viewer.c` - Working viewer
- `/opt/other/qemu/guest-demos/shadertoy/shadertoy.vert` - Reference vertex shader
- `/opt/other/qemu/guest-demos/shadertoy/shadertoy_gradient.frag` - Reference fragment shader
- `/opt/other/qemu/guest-demos/shadertoy/README.md` - Usage guide

### Performance:
- 405 FPS with animated gradient (1280x800)
- No fence hangs with Venus/virtgpu on Apple Silicon
- Based on proven vkcube_anim architecture (1100 FPS)
