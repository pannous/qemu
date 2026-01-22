# Venus Vulkan Cube Demo

Spinning rainbow cube rendered via Venus (Vulkan-over-virtio) on macOS with MoltenVK.

## Files

- `vkcube_anim.c` - Main demo source (Vulkan + DRM scanout)
- `cube.vert` / `cube.frag` - GLSL shaders
- `cube.vert.spv` / `cube.frag.spv` - Pre-compiled SPIR-V (optional)
- `build.sh` - Build script with dependency installation

## Usage

Copy files to guest `/root/` and run:

```bash
./build.sh    # Install deps + compile
./vkcube_anim # Run demo (10 seconds)
```

## Requirements

- Alpine Linux guest with virtio-gpu Venus
- Packages: vulkan-tools mesa-vulkan-virtio libdrm-dev shaderc build-base

## Features

- Uses DRM dumb buffers (not GBM) for reliable animation
- ~700-1200 fps on Apple M2 Pro via Venus/MoltenVK
- Rainbow gradient faces with depth testing
