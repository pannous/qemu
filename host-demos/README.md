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

### webgpu-cube

A rotating gradient cube rendered with WebGPU in the browser.

Features:
- Frame time tracking (shows theoretical max FPS)
- Modern WebGPU API
- Requires Chrome 113+, Safari 18+, or Firefox Nightly

**Run:**
```bash
cd webgpu-cube
./serve.sh
# Open http://localhost:8000
```

### vkcube-native

A rotating gradient cube using Vulkan/MoltenVK (native, not virtualized).

Features:
- Frame time tracking (shows theoretical max FPS)
- IMMEDIATE present mode (uncapped, no VSync)
- Tight render loop for maximum performance

**Run:**
```bash
cd vkcube-native
make run
```

## Performance Comparison

Compare frame times across all platforms:
- **Native Metal** (metal-cube) - Direct Metal API
- **WebGPU Browser** (webgpu-cube) - WebGPU → Metal
- **WebGL Browser** (webgl-cube) - WebGL → Metal/OpenGL 
- **Native Vulkan** (vkcube-native) - Vulkan → MoltenVK → Metal
- **Guest Vulkan** (guest-demos/*) - Guest → Venus → virtio-gpu → QEMU → MoltenVK → Metal

Frame time reveals virtualization overhead even when FPS is display-limited.

## Performance Metrics

All demos now display frame time metrics:

**Format:** `FPS: X (avg: Y) | Frame time: Zms (W max FPS)`

**Example:** `FPS: 60.0 (avg: 60.0) | Frame time: 2.5ms (400 max FPS)`

This shows display-limited at 60 FPS, but GPU theoretically capable of 400 FPS.

### Frame Time vs FPS

- **FPS:** Frames rendered in the last second (may be VSync-limited)
- **Frame Time:** Average milliseconds per frame (shows true GPU capability)
- **Max FPS:** Theoretical maximum = 1000ms / frame_time

Frame time reveals true performance even when VSync limits displayed FPS.

## Expected Performance

On Apple Silicon with ProMotion display (120 Hz):
- **Native Vulkan:** ~120 FPS, 8.3ms frame time (display-limited)
  - Uncapped mode: ~250-420 FPS, 2-4ms frame time
- **WebGPU Browser:** ~60-120 FPS (browser VSync policy)
- **Native Metal:** ~60-120 FPS (display refresh rate)
- **Guest Vulkan:** TBD (measure to quantify virtualization overhead)

Standard 60Hz displays will show 16.67ms frame time when VSync-limited.
