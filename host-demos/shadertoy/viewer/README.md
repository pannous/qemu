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

### Controls

- **F** or **F11** - Toggle fullscreen mode
- **ESC** - Exit viewer

## Current Shader

**Bumped Sinusoidal Warp** - A beautiful metallic surface with:
- Sinusoidal planar deformation
- Point-lit bump mapping
- Procedural texture warping
- Specular highlights
- Faux environment mapping

## Included Example Shaders

Try different shaders using `./switch_shader.sh`:

```bash
./switch_shader.sh simple_gradient  # Animated color gradient
./switch_shader.sh tunnel           # Classic tunnel effect
./switch_shader.sh plasma           # Plasma waves
./switch_shader.sh shadertoy        # Back to default
```

After switching, run `./run.sh` to view the new shader.

## Adding New Shaders

### From ShaderToy Website

1. Visit [ShaderToy](https://www.shadertoy.com/) and find a shader you like
2. Copy the shader code
3. Convert to Vulkan GLSL format (see HOWTO.md):
   - Replace `mainImage(out vec4 fragColor, in vec2 fragCoord)` with `main()`
   - Add Vulkan header and uniform buffer declarations
   - Use `ubo.iTime`, `ubo.iResolution`, `ubo.iMouse` for uniforms
4. Save to `examples/yourshader.frag`
5. Switch to it: `./switch_shader.sh yourshader`

### Download Shader Packs

Several pre-compiled ShaderToy collections are available:

- **[Geeks3D Shadertoy Demopack](https://www.geeks3d.com/hacklab/20231203/shadertoy-demopack-v23-12-3/)** - Curated collection with single-pass and multi-pass shaders
- **[Raspberry Pi ShaderToy Collection](https://forums.raspberrypi.com/viewtopic.php?t=247036)** - 100+ OpenGL ES 3.0 examples
- **[VirtualDJ Shaders Pack](https://www.virtualdjskins.co.uk/blog/shaders-for-virtualdj)** - 450+ shaders optimized for real-time use
- **[shadertoy-rs](https://github.com/fmenozzi/shadertoy-rs)** - Desktop client to browse and download shaders

Note: Downloaded shaders will need conversion to Vulkan GLSL format.

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
