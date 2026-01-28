# ShaderToy Viewer

Shader testing tool based on vkcube_anim's proven architecture.

## Usage

```bash
./shadertoy_viewer <vert.spv> <frag.spv> [duration_sec]
```

## Building

```bash
gcc -I/usr/include/libdrm -o shadertoy_viewer shadertoy_viewer.c -ldrm -lvulkan -lgbm -lm
```

## Shader Requirements

Your shaders MUST use `binding = 0, set = 0` for the uniform buffer:

```glsl
layout(binding = 0, set = 0) uniform UniformBufferObject {
    vec3 iResolution;  // viewport resolution (in pixels)
    float iTime;       // shader playback time (in seconds)
    vec4 iMouse;       // mouse pixel coords (currently unused)
} ubo;
```

## Reference Shaders

### Vertex Shader: `shadertoy.vert`
- Generates fullscreen quad (6 vertices, no vertex buffer needed)
- Outputs `fragCoord` in pixel coordinates [0, resolution]

### Fragment Shader: `shadertoy_gradient.frag`
- Simple animated gradient demonstrating uniform usage
- Shows how to access iTime, iResolution

## Compiling Shaders

```bash
glslangValidator -V shadertoy.vert -o shadertoy.vert.spv
glslangValidator -V your_shader.frag -o your_shader.frag.spv
```

## shadertoy_viewer_v2.c - With Texture Support

Extended version that provides texture input at binding 1.

### Usage
```bash
./shadertoy_viewer_v2 <vert.spv> <frag.spv> [duration_sec]
```

### Features
- binding 0: UniformBufferObject (iResolution, iTime, iMouse)
- binding 1: sampler2D (256x256 procedural checkerboard texture)

### Building
```bash
gcc -I/usr/include/libdrm -o shadertoy_viewer_v2 shadertoy_viewer_v2.c -ldrm -lvulkan -lgbm -lm
```

### Shader Requirements
```glsl
layout(binding = 0, set = 0) uniform UniformBufferObject { ... }
layout(binding = 1, set = 0) uniform sampler2D iChannel0;  // Optional texture input
```

## Performance

Tested on Virtio-GPU Venus (Apple M2 Pro):
- **405 FPS** with animated gradient shader (shadertoy_viewer)
- **306 FPS** with bumped_sinusoidal_warp shader (shadertoy_viewer_v2)
- **No fence hangs** with Venus/virtgpu

## Common Issues

### Blue screen / No output
- **Check binding numbers**: Uniform buffer MUST be at `binding = 0`
- Fragment shader should use the same binding as vertex shader
- The viewer only provides ONE descriptor binding at index 0

### Example Error
❌ **Won't work** (binding = 1):
```glsl
layout(binding = 1, set = 0) uniform UniformBufferObject { ... }
```

✅ **Correct** (binding = 0):
```glsl
layout(binding = 0, set = 0) uniform UniformBufferObject { ... }
```
