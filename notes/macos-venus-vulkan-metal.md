# macOS Venus/Vulkan via MoltenVK - Comprehensive Analysis

## Project Goal
Enable Vulkan → Metal passthrough on macOS for Redox OS guests via QEMU's Venus backend.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        REDOX OS GUEST                           │
│  ┌─────────────┐                                                │
│  │ Vulkan App  │                                                │
│  └──────┬──────┘                                                │
│         │ Vulkan API calls                                      │
│  ┌──────▼──────┐                                                │
│  │ Mesa Venus  │  Guest Vulkan driver                           │
│  │   Driver    │  Serializes Vulkan commands                    │
│  └──────┬──────┘                                                │
│         │ Venus protocol over virtio-gpu                        │
└─────────┼───────────────────────────────────────────────────────┘
          │ virtio ring
┌─────────┼───────────────────────────────────────────────────────┐
│         │                    QEMU HOST                          │
│  ┌──────▼──────┐                                                │
│  │ virtio-gpu  │  hw/display/virtio-gpu-virgl.c                 │
│  │   device    │  Handles Venus capset, fences, resources       │
│  └──────┬──────┘                                                │
│         │ virgl_renderer_* API                                  │
│  ┌──────▼──────┐                                                │
│  │virglrenderer│  External library                              │
│  │Venus backend│  Deserializes & replays Vulkan commands        │
│  └──────┬──────┘                                                │
│         │ Vulkan API                                            │
│  ┌──────▼──────┐                                                │
│  │  MoltenVK   │  Vulkan → Metal translation layer              │
│  └──────┬──────┘                                                │
│         │ Metal API                                             │
│  ┌──────▼──────┐                                                │
│  │ Metal GPU   │  Apple Silicon / AMD GPU                       │
│  └─────────────┘                                                │
└─────────────────────────────────────────────────────────────────┘
```

## Component Analysis

### 1. QEMU virtio-gpu-virgl (hw/display/virtio-gpu-virgl.c)

**Purpose:** QEMU device model that bridges guest virtio-gpu to host virglrenderer.

**Key Functions:**
| Function | Line | Purpose |
|----------|------|---------|
| `virtio_gpu_virgl_init()` | 1180 | Initialize virglrenderer with Venus flags |
| `setup_moltenvk_icd()` | 1148 | [NEW] macOS MoltenVK ICD auto-discovery |
| `virtio_gpu_virgl_get_capsets()` | 1252 | Report Venus capset to guest |
| `virgl_write_fence()` | 1046 | Handle fence completion callbacks |
| `virgl_write_context_fence()` | 1076 | Handle context-based fence callbacks |
| `virgl_cmd_resource_create_blob()` | 665 | Create blob resources |
| `virgl_cmd_set_scanout_blob()` | 805 | Set scanout from blob (requires dmabuf) |

**Venus Initialization Flow:**
```c
virtio_gpu_virgl_init()
  ├─ [macOS] setup_moltenvk_icd()     // Set VK_ICD_FILENAMES
  ├─ flags |= VIRGL_RENDERER_VENUS
  ├─ flags |= VIRGL_RENDERER_RENDER_SERVER
  └─ virgl_renderer_init(flags)       // External library takes over
```

### 2. Capset System

**Defined Capsets** (include/standard-headers/linux/virtio_gpu.h):
```c
VIRTIO_GPU_CAPSET_VIRGL   = 1   // OpenGL
VIRTIO_GPU_CAPSET_VIRGL2  = 2   // OpenGL enhanced
VIRTIO_GPU_CAPSET_GFXSTREAM_VULKAN = 3
VIRTIO_GPU_CAPSET_VENUS   = 4   // Vulkan via Venus
VIRTIO_GPU_CAPSET_CROSS_DOMAIN = 5
VIRTIO_GPU_CAPSET_DRM     = 6
```

**Venus Capset Registration:**
- Only added if `venus=true` device property set
- Only added if virglrenderer reports non-zero `capset_max_size`
- Actual extension data comes from `virgl_renderer_fill_caps()`

### 3. Fence Handling

**Status: Portable - No macOS changes needed**

Fence callbacks are invoked by virglrenderer when GPU work completes:
- `virgl_write_fence()` - Legacy fence completion
- `virgl_write_context_fence()` - Context-based fence completion (Venus)

Both use QEMU abstractions (`virtio_gpu_ctrl_response_nodata()`) that are platform-independent.

### 4. Memory/Resource Handling

**Resource Types:**
1. **Regular resources** - Created via `virgl_cmd_create_resource_2d/3d()`
2. **Blob resources** - Created via `virgl_cmd_resource_create_blob()` (VIRGL 1.0+)

**Blob Resource Flow:**
```
virgl_cmd_resource_create_blob()
  ├─ virtio_gpu_create_mapping_iov()  // Map guest memory
  ├─ virgl_renderer_resource_create_blob()
  ├─ virgl_renderer_resource_get_info()
  │     └─ info.fd → res->base.dmabuf_fd  // -1 on macOS!
  └─ [macOS] warn if dmabuf_fd < 0
```

**dmabuf Issue on macOS:**
- Linux uses dmabuf for zero-copy buffer sharing
- macOS uses IOSurface instead (incompatible)
- `virgl_renderer_resource_get_info()` returns `fd = -1` on macOS
- Blob scanout (`SET_SCANOUT_BLOB`) requires `dmabuf_fd >= 0`

### 5. Scanout Paths

| Command | Function | dmabuf Required | macOS Status |
|---------|----------|-----------------|--------------|
| `SET_SCANOUT` | `virgl_cmd_set_scanout()` | No | ✓ Works |
| `SET_SCANOUT_BLOB` | `virgl_cmd_set_scanout_blob()` | Yes | ✗ Fails |

**Non-blob scanout** uses OpenGL texture path via `dpy_gl_scanout_texture()`.

### 6. Vulkan Extension Filtering

**Key Finding: QEMU does NOT filter extensions**

Extension filtering chain:
```
Guest queries extensions
    ↓
virtio-gpu forwards to virglrenderer
    ↓
virglrenderer queries Vulkan driver
    ↓
MoltenVK reports supported extensions
    ↓
Extensions flow back to guest
```

MoltenVK handles filtering automatically based on Metal capabilities.

## Implemented Changes

### 1. MoltenVK ICD Auto-Discovery (hw/display/virtio-gpu-virgl.c:1148-1177)

```c
#ifdef __APPLE__
static void setup_moltenvk_icd(void)
{
    static const char *moltenvk_paths[] = {
        "/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json",
        "/usr/local/share/vulkan/icd.d/MoltenVK_icd.json",
        "/opt/homebrew/opt/molten-vk/share/vulkan/icd.d/MoltenVK_icd.json",
        "/usr/local/opt/molten-vk/share/vulkan/icd.d/MoltenVK_icd.json",
        NULL
    };
    // Check VK_ICD_FILENAMES/VK_DRIVER_FILES, search paths, set env
}
#endif
```

Called when Venus enabled in `virtio_gpu_virgl_init()`.

### 2. Blob Resource Warning (hw/display/virtio-gpu-virgl.c:745-751)

```c
#ifdef __APPLE__
    if (res->base.dmabuf_fd < 0) {
        warn_report_once("Blob resource %d created without dmabuf backing. "
                         "Blob scanout will not work on macOS.");
    }
#endif
```

### 3. Improved Error Message for Blob Scanout (hw/display/virtio-gpu-virgl.c:851-864)

```c
if (res->base.dmabuf_fd < 0) {
#ifdef __APPLE__
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: resource %d not backed by dmabuf. "
                  "Blob scanout requires dmabuf which is unavailable on macOS. "
                  "Use non-blob scanout or disable blob resources.\n", ...);
#else
    qemu_log_mask(LOG_GUEST_ERROR, "%s: resource not backed by dmabuf %d\n", ...);
#endif
}
```

## Known Limitations

### MoltenVK Limitations
- Pipeline statistics query pool (`VK_QUERY_TYPE_PIPELINE_STATISTICS`) not supported
- PVRTC compressed formats require direct host-visible memory mapping
- Some features require Metal 3.0+
- Some features require Apple GPU specifically
- Application-controlled memory allocations (`VkAllocationCallbacks`) ignored

### macOS-Specific Limitations
1. **No dmabuf** - Blob scanout unavailable
2. **No UDMABUF** - Stubs return false/no-op
3. **No sync_file** - Different sync primitives needed

### Workarounds
- Use non-blob scanout path (works via OpenGL textures)
- Don't enable `blob=true` on virtio-gpu device
- Ensure virglrenderer built with Venus + MoltenVK support

## Dependencies

### Required Software
```bash
# MoltenVK
brew install molten-vk

# Vulkan SDK (optional, for validation layers)
# Download from https://vulkan.lunarg.com/sdk/home

# virglrenderer with Venus support
# Must be built from source with Venus enabled
```

### Build Configuration
```bash
# QEMU configure (example)
./configure --target-list=x86_64-softmmu \
            --enable-virglrenderer \
            --enable-opengl
```

### Runtime Configuration
```bash
# Venus-enabled VM (non-blob, for macOS compatibility)
qemu-system-x86_64 \
    -device virtio-gpu-gl,venus=true \
    -display cocoa,gl=es
```

## File Reference

| File | Purpose |
|------|---------|
| `hw/display/virtio-gpu-virgl.c` | Venus/virgl backend implementation |
| `hw/display/virtio-gpu-gl.c` | GL device model, Venus property |
| `hw/display/virtio-gpu.c` | Base GPU device, blob property |
| `hw/display/virtio-gpu-udmabuf.c` | Linux udmabuf (N/A on macOS) |
| `hw/display/virtio-gpu-udmabuf-stubs.c` | Stubs for non-Linux |
| `include/hw/virtio/virtio-gpu.h` | Data structures, macros |
| `include/standard-headers/linux/virtio_gpu.h` | Capset definitions |

## Testing Checklist

- [ ] MoltenVK installed and ICD discoverable
- [ ] virglrenderer built with Venus support
- [ ] QEMU built with virglrenderer support
- [ ] Venus capset reported to guest (`VIRTIO_GPU_CAPSET_VENUS`)
- [ ] Non-blob scanout working (OpenGL texture path)
- [ ] Guest Vulkan apps render correctly
- [ ] Fence synchronization working (no hangs)
- [ ] Redox OS guest boots with Vulkan support

## References

- [MoltenVK GitHub](https://github.com/KhronosGroup/MoltenVK)
- [MoltenVK Runtime User Guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)
- [virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer)
- [Venus Protocol](https://gitlab.freedesktop.org/virgl/virglrenderer/-/tree/main/src/venus)
- [Virtio-GPU Specification](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
