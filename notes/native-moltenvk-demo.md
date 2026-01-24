# Native macOS MoltenVK Demo

Created: 2026-01-24

## What Was Built

A native macOS application that renders a rotating gradient cube using Vulkan → MoltenVK → Metal.

### Location
`/opt/other/qemu/host-demos/vkcube-native/`

### Files
- `vkcube_native.m` - Main Objective-C/C application (570 lines)
- `cube.vert` / `cube.frag` - GLSL shaders
- `Makefile` - Build system
- `README.md` - Overview
- `USAGE.md` - Detailed usage guide

## Purpose

Performance baseline for comparing against QEMU+Venus guest rendering.

## Architecture

```
Cocoa Window (NSWindow + CAMetalLayer)
    ↓
Vulkan 1.1 API
    ↓
MoltenVK (Portability ICD)
    ↓
Metal Framework
    ↓
macOS Display
```

## Features

1. **Same Geometry as Guest**: Uses identical cube vertices and colors
2. **FPS Counter**: Prints frames/second for benchmarking
3. **Proper Vulkan**: Full swapchain, command buffers, sync objects
4. **Native Performance**: No virtualization overhead

## Building

```bash
cd host-demos/vkcube-native
make
```

Requirements:
- MoltenVK (Homebrew: `brew install molten-vk`)
- Vulkan SDK headers (Homebrew: `brew install vulkan-headers`)
- glslc shader compiler

## Running

```bash
./vkcube_native
```

Opens a 800x600 window with rotating rainbow cube.
FPS printed to stdout every second.

## Performance Comparison

| Environment | Expected FPS | Notes |
|-------------|--------------|-------|
| Native (this) | 60+ | VSync limited, ~200-500 without |
| QEMU+Venus | 30-50 | Depends on copyback vs direct |
| % of Native | 50-80% | Virtualization overhead |

## Technical Details

### Vulkan Setup
- Instance with portability enumeration (required for MoltenVK)
- Metal surface via VK_EXT_metal_surface
- Swapchain with FIFO present mode
- Triple buffering (MAX_FRAMES = 3)

### Rendering
- Single vertex buffer with interleaved pos+color
- Uniform buffer for MVP matrix
- Perspective projection with rotating model
- Clear color: dark blue (0.1, 0.1, 0.15)

### Math
- Hand-rolled matrix functions (identity, mul, perspective, lookat, rotate)
- Rotation on both Y and X axes for visual interest
- 0.8 radian FOV, 0.1-100 near/far planes

## Issues Encountered

1. **Portability Enumeration Required**
   - MoltenVK is marked as portability driver in ICD
   - Must enable VK_KHR_portability_enumeration extension
   - Must set VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR flag

2. **Duplicate MoltenVK Warning**
   - Two copies: /opt/homebrew and /usr/local
   - Causes Class MVKBlockObserver warning
   - Non-fatal but should clean up /usr/local version

3. **API Version**
   - Using VK_API_VERSION_1_1 for better compatibility
   - 1.0 also works but 1.1 is recommended

## Next Steps

1. Compare FPS with guest demos
2. Profile to identify bottlenecks
3. Consider disabling VSync for max FPS testing
4. Test with different window sizes
5. Add GPU timing queries for detailed metrics

## References

- Guest demo: `/opt/other/qemu/guest-demos/vkcube/`
- MoltenVK docs: https://github.com/KhronosGroup/MoltenVK
- Vulkan portability: https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_KHR_portability_subset.html
