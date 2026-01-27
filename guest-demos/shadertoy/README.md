# ShaderToy Viewer - Guest Demo

Native ShaderToy shader viewer running in the **Alpine guest VM** using Vulkan → Venus → virtio-gpu → MoltenVK → Metal on macOS host.

```
ShaderToy GLSL → SPIR-V → Vulkan (guest) → Venus → virtio-gpu → MoltenVK (host) → Metal → Display
```

## Quick Start

### From Host

```bash
# Install to guest and build
./install-to-guest.sh

# SSH into guest
ssh -p 2222 root@localhost

# Run viewer
cd /root/shadertoy
./shadertoy_viewer
```

### From Guest

```bash
cd /root/shadertoy
./build.sh      # First time only
./shadertoy_viewer
```

**Controls**: `ESC` exit | `F/F11` fullscreen (if supported)

## Features

- Vulkan rendering via Mesa Venus in Alpine guest
- Virtio-gpu blob resources for zero-copy rendering
- Real-time animation with iTime, iResolution, iMouse uniforms
- Texture support (iChannel0)
- Collection of example shaders

## Architecture

This is a **guest demo** that runs inside the Alpine VM:
- Uses Vulkan API through Mesa Venus driver
- Venus forwards Vulkan commands via virtio-gpu to host
- Host QEMU uses MoltenVK to render with Metal
- Rendered frames displayed via virtio-gpu scanout

Compare to **host demo** (`host-demos/shadertoy/`):
- Runs natively on macOS
- Uses MoltenVK directly
- No virtio-gpu layer

## Building

The build script compiles both shaders and viewer:

```bash
./build.sh
```

Dependencies (auto-installed):
- vulkan-tools, vulkan-loader, vulkan-headers
- mesa-vulkan-virtio (Venus driver)
- shaderc (GLSL → SPIR-V compiler)
- glfw-dev (windowing)
- g++, build-base

## Switching Shaders

```bash
./switch_shader.sh simple_gradient
./switch_shader.sh tunnel
./switch_shader.sh plasma
```

## Example Shaders

Pre-included in `examples/`:
- **simple_gradient.frag** - Animated color gradient
- **tunnel.frag** - Classic tunnel effect
- **plasma.frag** - Plasma wave animation

Additional shaders in `shaders/` directory.

## Importing ShaderToy Shaders

You can import shaders from shadertoy.com:

```bash
./import_shader.sh https://www.shadertoy.com/view/XsXXDn
```

Note: Browser automation may not work in headless guest. Consider importing on host first.

## Technical Details

- **Resolution**: 1280x720 (configurable in code)
- **Vulkan Driver**: Mesa Venus (virtio-gpu backend)
- **Rendering Path**: Venus → virtio-gpu → QEMU → MoltenVK → Metal
- **Page Size**: Requires 16KB page alignment (alpine-virt-16k kernel)

## Differences from Host Demo

| Aspect | Guest Demo | Host Demo |
|--------|-----------|-----------|
| **Environment** | Alpine VM | macOS native |
| **Vulkan Driver** | Mesa Venus | MoltenVK direct |
| **Graphics Path** | virtio-gpu | Native Metal |
| **Use Case** | Test full guest→host pipeline | Quick shader development |
| **Performance** | Slightly lower (VM overhead) | Native performance |

## Troubleshooting

**"Failed to create Vulkan instance"**
```bash
# Check Venus driver is loaded
ls /usr/lib/libvulkan_virtio.so

# Verify virtio-gpu device
lspci | grep -i virtio
```

**Shader compilation errors**
```bash
glslc shadertoy.frag -o frag.spv  # Check for syntax errors
```

**Black screen or no output**
- Ensure QEMU is running with HVF acceleration (not TCG)
- Check that virtio-gpu blob support is enabled
- Verify 16KB kernel is used (not 4KB)

## Performance Notes

Guest demos have additional overhead:
- Vulkan command marshaling through Venus
- virtio-gpu blob transfers
- VM virtualization layer

Typical performance: 30-60 FPS depending on shader complexity.

## References

- Host ShaderToy viewer: `../../host-demos/shadertoy/`
- Venus driver: Mesa virtio-gpu Vulkan implementation
- virtio-gpu blobs: Zero-copy rendering via DMA-BUF
- [ShaderToy](https://www.shadertoy.com/) - Shader examples
