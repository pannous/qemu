# Using vkcube_anim as Base for Shader Testing

**Date**: 2026-01-28
**Status**: Recommended approach ✅

## Why vkcube_anim?

vkcube_anim is the **proven working** basis for shader testing because:

1. **✅ Works perfectly** - 1100 FPS with Venus/virtgpu on Apple Silicon
2. **✅ Has working uniform buffers** - properly created, mapped, and bound
3. **✅ Has working descriptors** - layout includes descriptor sets in pipeline
4. **✅ Has render loop** - continuous rendering with timing
5. **✅ Proven stable** - no fence hangs, no Venus errors

## Key Differences vs test_tri

### vkcube_anim (works):
```c
// Descriptor layout WITH uniform buffer
VkDescriptorSetLayoutBinding binding = {
    .binding = 0,
    .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    .descriptorCount = 1,
    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT
};

// Pipeline layout INCLUDES descriptor layout
VkPipelineLayoutCreateInfo layout_info = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1,
    .pSetLayouts = &descLayout  // ← KEY!
};

// During command recording:
vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipelineLayout, 0, 1, &descSet, 0, NULL);  // ← BINDS!
vkCmdDraw(cmd, 36, 1, 0, 0);
```

### shadertoy_drm (hangs):
```c
// Pipeline layout EMPTY
VkPipelineLayoutCreateInfo layout_info = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    // Missing: .setLayoutCount, .pSetLayouts
};

// Never binds descriptors!
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
// vkCmdBindDescriptorSets() ← MISSING!
vkCmdDraw(cmd, 3, 1, 0, 0);
```

## Shader Structure

### Cube Vertex Shader (cube.vert.spv)
```glsl
layout(binding = 0, set = 0) uniform UBO {
    mat4 mvp;
} ubo;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = ubo.mvp * vec4(inPos, 1.0);
    fragColor = inColor;
}
```

### Cube Fragment Shader (cube.frag.spv)
```glsl
layout(location = 0) in vec3 inColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(inColor, 1.0);
}
```

## Modification Plan for ShaderToy Support

### Required Changes:

1. **Add second uniform buffer** (binding 1):
```c
typedef struct {
    float iResolution[3];
    float iTime;
    float iMouse[4];
    float padding[8]; // Align to 64 bytes
} ShaderToyUBO;
```

2. **Update descriptor layout** to have 2 bindings:
```c
VkDescriptorSetLayoutBinding bindings[2] = {
    {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT},
    {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT}
};
```

3. **Change to fullscreen quad** (6 vertices, no vertex buffer):
- Generate positions in vertex shader
- Remove `vkCmdBindVertexBuffers`
- Change `vkCmdDraw(cmd, 36, ...)` to `vkCmdDraw(cmd, 6, ...)`

4. **Load custom shaders** from argv[1] and argv[2]

### Test Shaders Created:

- `/root/shadertoy_vkcube.vert.spv` - fullscreen quad with shadertoy UBO
- `/root/shadertoy_vkcube.frag.spv` - animated gradient using iTime

## Files

### Host:
- `/opt/other/qemu/guest-demos/vkcube/vkcube_anim.c` - Working reference ✅

### Guest (/root/):
- `shadertoy_from_vkcube.c` - Copy of vkcube_anim.c (compiles, runs)
- `shadertoy_vkcube.vert` / `.spv` - Fullscreen quad vertex shader
- `shadertoy_vkcube.frag` / `.spv` - Simple gradient fragment shader

## Next Steps

1. ✅ Copy vkcube_anim.c to guest
2. ✅ Create shadertoy test shaders
3. ⏳ Modify to accept shader paths as arguments
4. ⏳ Add second UBO for shadertoy uniforms
5. ⏳ Change from cube (36 verts) to fullscreen quad (6 verts)
6. ⏳ Test with simple gradient shader
7. ⏳ Test with more complex shadertoy shaders

## Success Criteria

If this works, we'll have:
- ✅ Proven stable base (vkcube_anim architecture)
- ✅ Working uniform buffers
- ✅ Working descriptor binding
- ✅ Ability to test custom GLSL shaders
- ✅ No fence hangs!
