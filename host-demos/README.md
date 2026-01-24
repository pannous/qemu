# Host Demos - Native macOS Performance Baselines

Native Metal and graphics demos running directly on macOS host for performance comparison against virtualized guest demos.

## Purpose

These demos provide performance baselines to measure the overhead of the QEMU/Venus/MoltenVK virtualization stack.

## Demos

### metal-cube

A rotating gradient cube rendered with native Metal.

**Build & Run:**
```bash
cd metal-cube
make run
```

Compare FPS against:
- `guest-demos/triangle` - Vulkan triangle via Venus/MoltenVK
- `guest-demos/vkcube` - Vulkan cube via Venus/MoltenVK

## Expected Performance

On Apple Silicon (M1/M2/M3):
- Host native: 1000+ FPS
- Guest via Venus: TBD (measure to quantify overhead)

The ratio gives us the virtualization performance cost.
