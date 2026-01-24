# Metal Gradient Cube - Host Performance Baseline

A pure native macOS Metal rotating gradient cube demo for performance comparison against the QEMU/Venus/MoltenVK virtualized graphics stack.

## Purpose

This demo serves as a performance baseline to measure the overhead of the virtualization layers:
- Guest Vulkan application → Venus → virtio-gpu → QEMU → MoltenVK → Metal → GPU

By comparing FPS between this native demo and the guest demos, we can quantify the performance impact of our virtualization stack.

## Features

- Native Metal rendering (no virtualization overhead)
- Rotating cube with per-vertex gradient colors
- Real-time FPS counter
- Depth testing and backface culling
- 800x600 window

## Building

```bash
make
```

## Running

```bash
make run
# or
./metal-cube
```

## Performance Expectations

On Apple Silicon (M1/M2/M3):
- Expected: 1000+ FPS (VSync disabled)
- CPU usage: minimal

The FPS is printed to stdout in real-time.

## Comparison

Compare this native performance against:
- `guest-demos/triangle` - Simple Vulkan triangle
- `guest-demos/vkcube` - Vulkan spinning cube

The performance ratio gives us the virtualization overhead factor.
