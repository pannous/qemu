# macOS Venus Compatibility Notes

## Fence Handling
**Status: Compatible**

The fence callbacks in virtio-gpu-virgl.c are portable:
- `virgl_write_fence()` - processes fence queue via QEMU abstractions
- `virgl_write_context_fence()` - context-based fence processing
- GL context operations use QEMU's `dpy_gl_*` display layer

No macOS-specific changes needed for fence synchronization.

## Memory/dmabuf Edge Cases
**Status: Limited - dmabuf unavailable on macOS**

### Issue
Linux uses dmabuf for zero-copy buffer sharing between processes. macOS doesn't have dmabuf - it uses IOSurface instead. The Venus backend relies on dmabuf for blob resources with scanout.

### Affected Operations
1. `virgl_cmd_set_scanout_blob()` - requires `dmabuf_fd >= 0`
2. `virgl_renderer_resource_create_blob()` - returns `dmabuf_fd = -1` on macOS

### Implemented Fixes
1. **Improved error message** in `set_scanout_blob()` explaining macOS limitation
2. **Warning at blob creation** when dmabuf backing unavailable

### Workarounds for Users
- Use non-blob scanout path (`VIRTIO_GPU_CMD_SET_SCANOUT` instead of `SET_SCANOUT_BLOB`)
- Don't enable `blob=true` on the virtio-gpu device for macOS
- Non-blob resources work fine via OpenGL texture path

## Architecture Summary
```
Guest Vulkan App
    ↓ (Venus protocol)
virtio-gpu-gl device (venus=true)
    ↓
virglrenderer + Venus backend
    ↓
MoltenVK (Vulkan → Metal)
    ↓
Metal GPU

Scanout paths:
- Non-blob: OpenGL texture → works on macOS
- Blob: dmabuf → fails on macOS (no dmabuf support)
```

## Future Work
To fully support blob scanout on macOS would require:
1. IOSurface-based alternative to dmabuf
2. Integration with virglrenderer macOS support
3. Significant architectural changes
