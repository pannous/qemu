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

## Vulkan Extension Filtering
**Status: Handled automatically by MoltenVK**

Extension filtering is NOT done in QEMU. The filtering chain:
1. Guest queries extensions via Venus protocol
2. virglrenderer queries Vulkan driver
3. MoltenVK reports supported extensions to virglrenderer
4. Extensions flow back to guest

MoltenVK (1.4+) supports Vulkan 1.4 with known limitations:
- Pipeline statistics query pool (`VK_QUERY_TYPE_PIPELINE_STATISTICS`) not supported
- PVRTC compressed formats require direct host-visible mapping
- Some features require specific Metal versions (3.0+)
- Some features require Apple GPU specifically

See: https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md

## Unix Domain Socket Communication
**Status: Fixed**

### Issue
macOS doesn't support `SOCK_SEQPACKET` for Unix domain sockets. virglrenderer uses `SOCK_SEQPACKET` because it preserves message boundaries (each `send()` corresponds to one `recv()`). With `SOCK_STREAM`, message boundaries are lost and messages can concatenate.

### Symptoms
- "invalid request size (48) or fd count (1) for context op 1" errors
- Messages arriving with wrong sizes due to boundary loss
- "Bad file descriptor" errors on render_server startup

### Implemented Fixes
1. **Message framing protocol**: Added 8-byte length-prefix header for macOS:
   ```c
   struct stream_msg_header {
      uint32_t size;      /* payload size */
      uint32_t fd_count;  /* number of fds attached */
   };
   ```
   Applied to both `src/proxy/proxy_socket.c` and `server/render_socket.c`

2. **CLOEXEC fix**: Only set CLOEXEC on parent's socket fd (fd[0]), not the child's (fd[1]).
   The render_server receives fd[1] via exec, so it must NOT have CLOEXEC set.

3. **fd_count initialization**: Fixed early initialization to 0 before any error paths.

### Files Modified (virglrenderer)
- `src/proxy/proxy_socket.c` - Proxy-side (QEMU process) framing + CLOEXEC fix
- `server/render_socket.c` - Server-side framing
- `server/render_common.c` - macOS stderr logging

## Portability Enumeration
**Status: Fixed**

### Issue
MoltenVK requires `VK_KHR_portability_enumeration` extension and the `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` flag during instance creation. Without these, the Vulkan loader doesn't enumerate MoltenVK physical devices.

### Symptoms
- `vkCreateInstance` returns `VK_ERROR_INCOMPATIBLE_DRIVER` (-9)
- "Found no drivers!" error from Vulkan loader

### Fix
Added conditional code in `vkr_instance.c` for macOS:
```c
#ifdef __APPLE__
   ext_names[ext_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
   create_info->flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
```

### Result
Venus now successfully detects MoltenVK and exposes:
- Device: Virtio-GPU Venus (Apple M2 Pro)
- Vulkan API: 1.2.0
- Driver: Mesa Venus 25.2.7

## Swapchain / Display - Host-Side Vulkan Swapchain
**Status: Implemented**

### Solution Architecture
Instead of modifying guest Mesa to expose `VK_KHR_swapchain`, we implemented host-side Vulkan swapchain in QEMU. This intercepts blob scanout commands and presents via a host-managed swapchain.

```
Guest Vulkan App (renders to blob)
    ↓ SET_SCANOUT_BLOB
QEMU virtio-gpu-virgl.c (intercept scanout)
    ↓
Host Vulkan Swapchain (MoltenVK)
    ↓ IOSurface bridge
CAMetalLayer (cocoa.m)
    ↓
macOS Display
```

### Key Insight
Rather than adding swapchain commands to the Venus protocol (which would require complex Mesa changes), we intercept at the existing virtio-gpu scanout level where blob frames are already received.

### Implementation Files
| File | Purpose |
|------|---------|
| `ui/cocoa.m` | Added CAMetalLayer to QemuCocoaView |
| `hw/display/virtio-gpu-vk-swapchain.m` | **NEW** - Host Vulkan swapchain via MoltenVK |
| `hw/display/virtio-gpu-vk-swapchain.h` | **NEW** - Header file |
| `hw/display/virtio-gpu-virgl.c` | Integrated swapchain in `virgl_cmd_set_scanout_blob()` |
| `include/hw/virtio/virtio-gpu.h` | Added `vk_swapchain` to VirtIOGPUGL struct |
| `hw/display/meson.build` | Added new source files |
| `meson.build` | Added Metal framework to cocoa dependency |

### How It Works
1. **Initialization** (`virtio_gpu_virgl_init`):
   - Get CAMetalLayer from Cocoa display
   - Create Vulkan instance with VK_EXT_metal_surface
   - Create VkSurfaceKHR from Metal layer
   - Create VkSwapchainKHR with BGRA8 format
   - Enable Metal layer visibility

2. **Presentation** (`virgl_cmd_set_scanout_blob`):
   - Map blob resource to get host pointer
   - Acquire swapchain image
   - Copy blob data to staging buffer
   - Blit staging buffer → swapchain image
   - Present via `vkQueuePresentKHR`

3. **Fallback**: If Vulkan swapchain fails, falls back to software scanout via pixman

### What Works
- `vkCreateInstance` ✓
- `vkCreateDevice` ✓
- `vkAllocateMemory` ✓
- Compute shaders ✓
- All non-WSI Vulkan operations ✓
- **Blob scanout via host Vulkan swapchain** ✓ (NEW)

### MoltenVK Portability Extensions
Required for host swapchain to work with MoltenVK:
- Instance extension: `VK_KHR_portability_enumeration`
- Instance flag: `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`
- Device extension: `VK_KHR_portability_subset`

Without these, `vkCreateInstance` returns `VK_ERROR_INCOMPATIBLE_DRIVER` (-9).

### Known Limitations
- Single display output only (no multi-monitor)
- Host swapchain format fixed to BGRA8
- VSync via CAMetalLayer display link

## HOST_VISIBLE Memory Mapping - VK_EXT_external_memory_host
**Status: Fixed (2026-01-20)**

### Problem
vkMapMemory on HOST_VISIBLE memory failed with VK_ERROR_MEMORY_MAP_FAILED (-5).

### Root Cause
Two issues in virglrenderer:
1. **No fd export mechanism on macOS**: MoltenVK doesn't support `VK_KHR_external_memory_fd` or `VK_EXT_external_memory_dma_buf`, which Venus uses for blob memory sharing.
2. **SHM size validation too strict**: Proxy rejected aligned SHM allocations.

### Solution (in virglrenderer)
1. **VK_EXT_external_memory_host fallback path** (`vkr_device_memory.c`):
   - Detect when fd export unavailable but host pointer import available
   - Create SHM file via `os_create_anonymous_file()`
   - mmap the SHM to get a host pointer
   - Import as Vulkan memory via `VkImportMemoryHostPointerInfoEXT`
   - Align to 16KB (`minImportedHostPointerAlignment` on macOS)
   - Export blob with `VIRGL_RESOURCE_FD_SHM` type

2. **Fixed SHM validation** (`proxy_context.c`):
   - Changed `size != expected_size` to `size < expected_size`
   - Allows alignment padding (16KB) beyond requested size (e.g., 4KB)

### Verification
```
$ /tmp/test_mem
vkAllocateMemory: 0
vkMapMemory: 0 ptr=0xffffb3fbc000
write OK!
```

### Commits
- virglrenderer: `0b3d075a` - fix: Allow SHM blob size >= expected for alignment padding

## Blob Scanout Test
**Status: Working (2026-01-20)**

### Test: GBM + Vulkan + DRM Scanout
```
$ /tmp/test_blob
Display: 1280x800
GBM BO: stride=5120, prime_fd=6
FB: 42
Vulkan: Virtio-GPU Venus (Apple M2 Pro)
Vulkan: rendered 1280x800 blue
Copied to GBM buffer
Setting mode...
Blue screen for 3s...
Done!
```

### What This Proves
1. **Venus Vulkan rendering**: Used "Virtio-GPU Venus (Apple M2 Pro)" device
2. **HOST_VISIBLE memory mapping**: vkMapMemory works for data transfer
3. **GBM blob resource**: Created via `gbm_bo_create()` with SCANOUT flag
4. **DRM blob scanout**: `drmModeSetCrtc()` triggered SET_SCANOUT_BLOB path
5. **Display output**: Blue screen displayed for 3 seconds

### Flow
```
Guest: Vulkan (Venus) → HOST_VISIBLE buffer → GBM buffer → DRM scanout
                                                    ↓
                                            SET_SCANOUT_BLOB
                                                    ↓
Host: virtio-gpu-virgl.c → Host Vulkan swapchain → CAMetalLayer
```

## Future Work
- Multi-display support
- HDR/wide color gamut
- Zero-copy via IOSurface-Vulkan interop (currently uses staging buffer copy)
