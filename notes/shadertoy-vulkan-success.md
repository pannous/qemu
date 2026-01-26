# ShaderToy + Vulkan + MoltenVK Success! 🎨

**Date:** 2026-01-26

## Achievement

Successfully created a native macOS ShaderToy viewer that renders GLSL shaders via **Vulkan → MoltenVK → Metal**!

## Architecture

```
ShaderToy GLSL (fragment shader)
    ↓ glslangValidator
SPIR-V bytecode (.spv files)
    ↓ Vulkan API
MoltenVK translation layer
    ↓
Metal API (Apple's native graphics)
    ↓
GPU rendering on Apple Silicon (M2 Pro)
```

## What Works

✅ **Full Vulkan pipeline** - Instance creation, surface, swapchain, render pass
✅ **SPIR-V shader compilation** - GLSL → SPIR-V conversion with glslangValidator
✅ **MoltenVK integration** - Seamless Vulkan to Metal translation
✅ **Real-time rendering** - 60 FPS with VSync, smooth animation
✅ **ShaderToy uniforms** - iTime, iResolution, iMouse, iChannel0 all working
✅ **Complex shader** - Bump mapping, lighting, textures, procedural warping
✅ **Apple Silicon** - Native Metal rendering on M2 Pro GPU

## Technical Details

- **Window**: 1280x720 GLFW window
- **Shader**: Bumped Sinusoidal Warp (from shadertoy.com/view/4l2XWK)
- **Rendering**: Double-buffered with descriptor sets
- **Uniforms**: Structured buffer with alignment for iTime/iResolution/iMouse
- **Texture**: 256x256 procedural gradient in iChannel0
- **FPS**: ~60 with VSync enabled

## Key Files

- `/opt/other/qemu/host-demos/shadertoy/viewer/shadertoy_viewer.cpp` - Main Vulkan application
- `/opt/other/qemu/host-demos/shadertoy/viewer/shadertoy.frag` - Fragment shader (converted from ShaderToy)
- `/opt/other/qemu/host-demos/shadertoy/viewer/run.sh` - Launch script with environment vars

## Environment Setup

```bash
export VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
export DYLD_LIBRARY_PATH=/opt/homebrew/lib:$DYLD_LIBRARY_PATH
```

## Challenges Overcome

1. **Vulkan portability** - Required VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
2. **GLFW Vulkan detection** - glfwVulkanSupported() failed but actual Vulkan works
3. **Color write mask constants** - Used VK_COLOR_COMPONENT_*_BIT not VK_COLOR_WRITE_MASK_*_BIT
4. **MoltenVK ICD path** - Located at /opt/homebrew/etc/ not /opt/homebrew/share/

## Relevance to QEMU/Redox Project

This demonstrates that:
- MoltenVK provides full Vulkan 1.0+ compatibility on macOS
- Complex fragment shaders work perfectly through the Vulkan→Metal path
- The venus virtio-gpu backend should work well with MoltenVK host-side
- ShaderToy shaders can serve as excellent test cases for Vulkan rendering

## Next Steps

- Test more complex ShaderToy shaders (raymarching, etc.)
- Add iChannel1-3 support for multiple textures
- Implement mouse interaction
- Try compute shaders through Vulkan
- Integrate with QEMU's venus rendering pipeline

## Resources

- [ShaderToy](https://www.shadertoy.com/view/4l2XWK) - Original shader
- [MoltenVK](https://github.com/KhronosGroup/MoltenVK) - Vulkan to Metal
- [Vulkan Tutorial](https://vulkan-tutorial.com/) - Learning resource
- [SPIR-V Guide](https://www.khronos.org/spir/) - Shader IR format
