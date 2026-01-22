# Zero-Copy Vulkan Triangle Demo

RGB triangle rendered via Venus with **zero-copy** GBM→Vulkan→Scanout.

## Architecture

```
GBM blob (SCANOUT) ←── import fd ──→ VkImage ←── render
      │
      └──→ DRM scanout (same memory, NO COPY!)
```

1. Create GBM buffer with `GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING`
2. Get DMA-BUF fd via `gbm_bo_get_fd()`
3. Import into Vulkan via `VK_EXT_external_memory_dma_buf`
4. Render directly to imported VkImage
5. Scanout the GBM buffer - **no memcpy needed!**

## Files

- `test_tri.c` - Zero-copy Vulkan demo
- `tri.vert/frag` - GLSL shaders with embedded vertices
- `build.sh` - Build script

## Usage

```bash
./build.sh   # Install deps + compile
./test_tri   # Shows triangle for 5 seconds
```

## Required Extensions

- `VK_KHR_external_memory_fd`
- `VK_EXT_external_memory_dma_buf`
- `VK_EXT_image_drm_format_modifier`
