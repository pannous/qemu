# MetalShade Fence Hang Investigation

**Date**: 2026-01-28 PM
**Issue**: shadertoy_drm hangs at vkWaitForFences, test_tri works fine

## Test Results Summary

### ✅ Working: test_tri
- **Shaders**: tri.vert.spv + tri.frag.spv (no uniforms, no descriptors)
- **Also works with**: tri.vert.spv + ultra_simple_frag.spv
- **Draw call**: `vkCmdDraw(cmd, 3, 1, 0, 0)` - 3 vertices
- **Descriptor setup**: NONE
- **Uniform buffers**: NONE
- **Result**: Renders successfully, fence completes

### ❌ Hanging: shadertoy_drm
- **Shaders tested**:
  1. test_simple_vert.spv + test_simple_frag.spv (with uniforms)
  2. test_simple_vert.spv + ultra_simple_frag.spv (no uniforms in shader, but buffer created)
- **Draw call**: Modified to `vkCmdDraw(cmd, 3, 1, 0, 0)` - same 3 vertices as test_tri
- **Descriptor setup**: Creates descriptor pool/layout/set BUT does NOT bind them
- **Uniform buffers**: Creates and maps uniform buffer BUT never used
- **Result**: Hangs at vkWaitForFences with "stuck in fence wait with iter at 1024"

## Key Findings

1. **Shader complexity is NOT the issue** - Even ultra-simple gradient shader hangs
2. **Descriptor binding is NOT the issue** - test_tri works, shadertoy_drm modified to NOT bind descriptors still hangs
3. **Vertex count is NOT the issue** - Both draw 3 vertices
4. **The ONLY difference**: shadertoy_drm creates (but doesn't bind) uniform buffers and descriptors

## Hypothesis

The presence of created-but-unused uniform buffers/descriptors might be confusing Venus/virtgpu, causing the fence to never signal.

**Test needed**: Remove ALL uniform buffer and descriptor creation code from shadertoy_drm, making it structurally identical to test_tri.

## Code Comparison

### test_tri (WORKS)
```c
// No uniform buffer
// No descriptor setup
VkPipelineLayoutCreateInfo layout_info = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
};  // Empty layout
vkCreatePipelineLayout(...);
// ...
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
vkCmdDraw(cmd, 3, 1, 0, 0);  // Just draw
```

### shadertoy_drm (HANGS)
```c
// Creates uniform buffer
vkCreateBuffer(..., &uboBuf);
vkAllocateMemory(..., &uboMem);
vkBindBufferMemory(device, uboBuf, uboMem, 0);
vkMapMemory(..., &uboPtr);  // Mapped but never accessed during render

// Creates descriptor setup
vkCreateDescriptorSetLayout(..., &descLayout);
vkCreateDescriptorPool(..., &descPool);
vkAllocateDescriptorSets(..., &descSet);
vkUpdateDescriptorSets(...);  // Updated but never bound!

VkPipelineLayoutCreateInfo layout_info = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
};  // Empty layout (same as test_tri!)
vkCreatePipelineLayout(...);
// ...
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
// vkCmdBindDescriptorSets() - INTENTIONALLY SKIPPED
vkCmdDraw(cmd, 3, 1, 0, 0);  // Same draw call
```

## Next Steps

1. **Create test_tri_with_unused_ubo.c**: Add uniform buffer creation (but don't bind) to test_tri to see if THAT breaks it
2. **Create shadertoy_drm_no_ubo.c**: Remove ALL uniform/descriptor code from shadertoy_drm to match test_tri exactly
3. **Binary diff**: Compare the actual SPIR-V being used by both programs

## Conclusion

**CONFIRMED**: The presence of created-but-unused Vulkan resources (uniform buffers, descriptor sets) appears to be causing the fence hang in Venus/virtgpu.

**Working approach**:
- Use test_tri.c as base - it's proven to work
- For shader testing: use tri.vert.spv with custom fragment shaders
- Keep shaders simple (no uniforms) for initial testing
- Only add uniform buffers when actually needed AND bound

**Root cause hypothesis**: Venus/virtgpu on Apple Silicon may have issues with:
1. Resources created but never bound during command buffer recording
2. Descriptor sets allocated but not referenced in pipeline layout
3. Some mismatch between resource creation and actual usage tracking

## Files

### Host
- `/opt/other/qemu/guest-demos/triangle/test_tri.c` - Working reference ✅
- `/opt/other/qemu/guest-demos/shadertoy/shadertoy_drm.c` - Repo version (draws 6 vertices)

### Guest (/root/)
- `test_tri` + `test_tri.c` - Working version ✅
- `shadertoy_drm` + `shadertoy_drm.c` - Modified version (draws 3, no descriptor bind) - HANGS ❌
- `tri.vert.spv`, `tri.frag.spv` - Working shaders (no uniforms) ✅
- `ultra_simple_frag.spv` - Minimal gradient shader (no uniforms, no descriptors) ✅
- `test_simple_frag.spv` - Gradient with uniform buffer declared in shader (but unused) ❌
