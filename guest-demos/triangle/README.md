# Venus Triangle Demo

Simple RGB triangle rendered via Venus (Vulkan-over-virtio).

## Files

- `test_tri.c` - Main source (hardcoded triangle vertices in shader)
- `tri.vert/frag` - GLSL shaders with embedded vertex positions
- `build.sh` - Build script

## Usage

```bash
./build.sh   # Install deps + compile
./test_tri   # Shows triangle for 5 seconds
```

## Notes

- Uses GBM buffer (single frame only - GBM can't remap after scanout)
- For animation, see the vkcube demo which uses DRM dumb buffers
