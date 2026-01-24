# Host Demos - Native macOS Performance Baselines

Native Metal and graphics demos running directly on macOS host for performance comparison against virtualized guest demos.

## Purpose

These demos provide performance baselines to measure the overhead of the QEMU/Venus/MoltenVK virtualization stack.

## Demos

### metal-cube

A rotating gradient cube rendered with native Metal API.

**Build & Run:**
```bash
cd metal-cube
make run
```

### webgl-cube

A rotating gradient cube rendered with WebGL in the browser.

**Run:**
```bash
cd webgl-cube
./run.sh
# or: open index.html
```

### vkcube-native

A rotating gradient cube using Vulkan/MoltenVK (native, not virtualized).

**Run:**
```bash
cd vkcube-native
./run.sh
```

## Performance Comparison

Compare FPS across all platforms:
- **Native Metal** (metal-cube) - Direct Metal API
- **WebGL Browser** (webgl-cube) - WebGL → Metal/OpenGL
- **Native Vulkan** (vkcube-native) - Vulkan → MoltenVK → Metal
- **Guest Vulkan** (guest-demos/*) - Guest → Venus → virtio-gpu → QEMU → MoltenVK → Metal

## Expected Performance

On Apple Silicon (M1/M2/M3):
- Native Metal: ~60 FPS (compositor limited)
- WebGL: ~60 FPS (requestAnimationFrame VSync)
- Native Vulkan: ~60 FPS (presentation limited)
- Guest Vulkan: TBD (measure to quantify virtualization overhead)

All are capped by display refresh rate (60Hz) or compositor. For uncapped performance, need to measure render time per frame rather than FPS.
