# WebGL Gradient Cube - Browser Performance Baseline

A pure WebGL rotating gradient cube demo running in the browser for performance comparison.

## Purpose

This demo provides a browser-based performance baseline to compare against:
- Native Metal (host-demos/metal-cube) - Direct Metal API
- Guest Vulkan (guest-demos/*) - Virtualized graphics via Venus/MoltenVK

By comparing FPS across all three platforms, we can understand:
1. Browser graphics overhead (WebGL → Metal/OpenGL)
2. Virtualization overhead (Guest Vulkan → Venus → MoltenVK → Metal)

## Features

- Pure WebGL rendering (no frameworks)
- Rotating cube with per-vertex gradient colors
- Real-time FPS counter with average tracking
- Depth testing and backface culling
- Self-contained single HTML file

## Running

```bash
# Option 1: Open directly in browser
open index.html

# Option 2: Use the run script
./run.sh
```

The browser will open automatically showing the rotating cube.

## Performance Expectations

On modern browsers with hardware acceleration:
- Chrome/Safari on Apple Silicon: 60 FPS (display refresh rate)
- Limited by requestAnimationFrame() which syncs to VSync

The FPS counter shows both instantaneous and average FPS.

## GPU Info

Open browser console to see:
- GPU Renderer
- Vendor information
- WebGL version

## Comparison

Expected performance hierarchy:
1. **Native Metal**: 60 FPS (compositor limited)
2. **WebGL Browser**: 60 FPS (requestAnimationFrame VSync)
3. **Guest Vulkan**: TBD (measure virtualization overhead)

If guest performance is significantly lower, that indicates the overhead of the QEMU/Venus/MoltenVK stack.
