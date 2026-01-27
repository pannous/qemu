# ShaderToy Viewer - DRM Edition

Direct rendering ShaderToy viewer for Alpine Linux guest without any display server (no X11/Wayland/GLFW required).

## Architecture

Same proven architecture as `triangle` and `vkcube` demos:

```
VkImage (LINEAR, HOST_VISIBLE) ← Render shader
         ↓ memcpy
Double-buffered GBM (XRGB8888)
         ↓
DRM scanout (immediate mode)
```

## Features

- **No display server required** - Direct DRM/GBM rendering
- **ShaderToy compatibility** - iTime, iResolution, iMouse uniforms
- **Shader compilation** - Automatic GLSL to SPIR-V conversion
- **Animation loop** - 60 FPS frame-rate limiting
- **Time-based rendering** - Specify duration or run indefinitely

## Building

### In Alpine Guest

```bash
cd /root/shadertoy
./build.sh
```

This installs:
- Vulkan headers and Mesa Venus driver
- libdrm, gbm for display
- shaderc for GLSL compilation
- build tools (g++, make)

**Note**: Removed `glfw-dev` dependency - not needed!

## Usage

### Basic Usage

```bash
# Run default shader for 30 seconds
./shadertoy_viewer_drm

# Run specific shader
./shadertoy_viewer_drm /opt/3d/metalshade/tunnel.frag

# Run for custom duration (60 seconds)
./shadertoy_viewer_drm /opt/3d/metalshade/plasma.frag 60

# Run indefinitely (Ctrl+C to stop)
./shadertoy_viewer_drm shader.frag 999999
```

### Command Line Arguments

```
./shadertoy_viewer_drm [shader_path] [duration]

shader_path: Path to .frag shader file (default: /opt/3d/metalshade/shadertoy.frag)
duration:    Runtime in seconds (default: 30)
```

## Shader Format

Shaders can be in two formats:

### 1. ShaderToy Format (`.frag`)

```glsl
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    fragColor = vec4(uv, 0.5 + 0.5 * sin(iTime), 1.0);
}
```

These are automatically converted to Vulkan GLSL.

### 2. Vulkan GLSL Format (`.glsl`)

```glsl
#version 450

layout(binding = 0) uniform UniformBufferObject {
    vec3 iResolution;
    float iTime;
    vec4 iMouse;
} ubo;

layout(location = 0) out vec4 fragColor;

void main() {
    vec2 fragCoord = gl_FragCoord.xy;
    vec2 uv = fragCoord / ubo.iResolution.xy;
    fragColor = vec4(uv, 0.5 + 0.5 * sin(ubo.iTime), 1.0);
}
```

These are used directly without conversion.

## Output

```
ShaderToy Viewer - DRM Direct Rendering
Shader: /opt/3d/metalshade/tunnel.frag
Duration: 30.0 seconds
✓ Using GLSL shader: /opt/3d/metalshade/tunnel.glsl
✓ Compiled: /opt/3d/metalshade/tunnel.frag.spv
✓ Vertex shader: /opt/3d/metalshade/tunnel.vert.spv
Display: 1280x720
GPU: Venus (llvmpipe)
✓ Running shader animation
Controls: Ctrl+C to stop

Frame 150: 60.0 FPS | Time: 2.5s / 30.0s
```

## Controls

- **Ctrl+C** - Stop rendering and exit
- No keyboard/mouse controls yet (coming soon)

## How It Works

1. **Shader Compilation** (before graphics init):
   - Convert ShaderToy `.frag` → Vulkan `.glsl` (if needed)
   - Compile `.glsl` → `.frag.spv` (SPIR-V)
   - Copy generic vertex shader → `.vert.spv`

2. **DRM/GBM Setup**:
   - Open `/dev/dri/card0`
   - Find connected display and mode
   - Create 2 scanout buffers (double buffering)
   - Create DRM framebuffers

3. **Vulkan Setup**:
   - Create device and queue
   - Allocate LINEAR + HOST_VISIBLE image (for CPU access)
   - Create render pass and framebuffer
   - Load compiled SPIR-V shaders
   - Create pipeline

4. **Render Loop** (60 FPS):
   - Update uniforms (iTime, iResolution, iMouse)
   - Render to VkImage
   - Copy VkImage → GBM buffer (memcpy, row-by-row)
   - Scanout via `drmModeSetCrtc`
   - Swap buffers
   - Sleep to maintain 60 FPS

## Differences from GLFW Version

| Feature | GLFW Version | DRM Version |
|---------|-------------|-------------|
| Display Server | Required (X11/Wayland) | None (direct DRM) |
| Dependencies | glfw, wayland/X11 libs | libdrm, gbm |
| Window Management | GLFW handles | Direct DRM modesetting |
| Input | Keyboard/mouse callbacks | Time-based only |
| Fullscreen | Toggle via F11 | Always fullscreen |
| Shader Browsing | Arrow keys | Via command line |
| Platform | macOS (MoltenVK) | Alpine guest (Venus) |

## Troubleshooting

### "Failed to open /dev/dri/card0"

Ensure you're running in the Alpine guest with DRM access:

```bash
ls -l /dev/dri/card0
# Should show: crw-rw---- 1 root video
```

### "Shader compilation failed"

Check shader syntax:

```bash
# Manually compile to see errors
glslc shader.frag -o test.spv
```

### "No connected display"

The guest's virtual display should always be available. Check QEMU is running with `-device virtio-gpu-gl-pci`.

### Low FPS

Check if Venus is working:

```bash
# Should show Venus (llvmpipe) or Venus (hardware)
vulkaninfo | grep deviceName
```

## Performance

Expected performance:
- **Simple shaders** (gradients): 60 FPS solid
- **Medium shaders** (raymarching): 30-60 FPS
- **Complex shaders** (multiple passes): 15-30 FPS

Bottleneck is CPU-based rendering (llvmpipe) + memcpy to GBM.

## Next Steps

1. **Add keyboard input** - Non-blocking stdin/termios for shader switching
2. **Shader hot-reload** - Watch for file changes and recompile
3. **Mouse support** - Basic mouse position tracking
4. **Multiple shaders** - Browse through shader list
5. **Screenshot capture** - Save frames to PNG
6. **Zero-copy path** - Import GBM fd as VkImage (remove memcpy)

## Files

- `shadertoy_viewer_drm.cpp` - DRM-based viewer (this implementation)
- `shadertoy_viewer.cpp` - Original GLFW-based viewer (macOS)
- `build.sh` - Build script for Alpine guest
- `README_DRM.md` - This file

## See Also

- `../triangle/test_tri.c` - Simple DRM+Vulkan triangle demo
- `../vkcube/vkcube_anim.c` - Animated cube with double buffering
- `/opt/other/qemu/notes/swapchain.md` - Venus swapchain architecture
