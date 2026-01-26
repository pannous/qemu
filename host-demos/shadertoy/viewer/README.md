# ShaderToy Viewer for macOS

A native ShaderToy shader viewer using **Vulkan + MoltenVK** to render GLSL shaders with Metal on macOS.

## Architecture

```
ShaderToy GLSL → SPIR-V → Vulkan API → MoltenVK → Metal → macOS Display
```

## Features

- ✅ Converts ShaderToy GLSL shaders to Vulkan SPIR-V
- ✅ Renders using Vulkan API with MoltenVK backend
- ✅ Hardware-accelerated Metal rendering on Apple Silicon
- ✅ Real-time animation with iTime uniform
- ✅ Texture support (iChannel0)
- ✅ Full bump mapping and lighting effects

## Requirements

- macOS with Vulkan SDK (via Homebrew)
- MoltenVK (`brew install molten-vk`)
- GLFW (`brew install glfw`)
- glslang for shader compilation (`brew install glslang`)

## Building

```bash
make
```

This compiles:
1. `shadertoy.vert` → `vert.spv` (vertex shader)
2. `shadertoy.frag` → `frag.spv` (fragment shader with bump mapping)
3. `shadertoy_viewer.cpp` → `shadertoy_viewer` (Vulkan application)

## Running

```bash
./run.sh
```

Or manually:
```bash
export VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
export DYLD_LIBRARY_PATH=/opt/homebrew/lib:$DYLD_LIBRARY_PATH
./shadertoy_viewer
```

## Current Shader

**Bumped Sinusoidal Warp** - A beautiful metallic surface with:
- Sinusoidal planar deformation
- Point-lit bump mapping
- Procedural texture warping
- Specular highlights
- Faux environment mapping

Press ESC or close the window to exit.

## Adding New Shaders

1. Save ShaderToy shader as `.shade` file (or any extension)
2. Convert to Vulkan GLSL format:
   - Replace `mainImage(out vec4 fragColor, in vec2 fragCoord)` with `main()`
   - Use `ubo.iTime`, `ubo.iResolution`, `ubo.iMouse` for uniforms
   - Update `layout(binding = 1) uniform sampler2D iChannel0` for textures
3. Compile to SPIR-V: `glslangValidator -V yourshader.frag -o frag.spv`
4. Run viewer

## Technical Details

- **Window**: 1280x720 (configurable)
- **Render Loop**: ~60 FPS with VSync
- **Texture**: 256x256 procedural gradient (modifiable)
- **Uniform Buffer**: iTime, iResolution, iMouse updated per frame
- **Descriptor Sets**: Double-buffered for smooth rendering

## References

- [ShaderToy](https://www.shadertoy.com/) - Original shader source
- [MoltenVK](https://github.com/KhronosGroup/MoltenVK) - Vulkan to Metal translation
- [SPIR-V](https://www.khronos.org/spir/) - Shader intermediate representation
- [Vulkan SDK](https://vulkan.lunarg.com/) - Vulkan API

## Sources

- [GitHub - schwa/ShaderConverter](https://github.com/schwa/ShaderConverter)
- [SwiftUI Metal Shader Tutorials + Replacement from GLSL to MSL | by IKEH | Medium](https://medium.com/@ikeh1024/swiftui-metal-shader-tutorials-replacement-from-glsl-to-msl-6e97b7307dc2)
- [Metal shader converter - Metal - Apple Developer](https://developer.apple.com/metal/shader-converter/)
- [ISF for Metal – Now open source! — VDMX](https://vdmx.vidvox.net/blog/isf-for-metal)
- [Shadertoy to ISF - VIDVOX](https://discourse.vidvox.net/t/shadertoy-to-isf/1412)
- [GitHub - repi/shadertoy-browser](https://github.com/repi/shadertoy-browser)
- [ISF](https://isf.video/)
