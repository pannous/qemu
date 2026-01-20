# Venus VK_EXT_external_memory_host Investigation

## Current State (2026-01-20)

### Good News: Infrastructure Already Exists!

The virglrenderer Venus code **already has full support** for VK_EXT_external_memory_host. The implementation is complete:

1. **Extension Detection** (`vkr_physical_device.c:278-334`)
   - Detects VK_EXT_external_memory_host availability
   - Queries `minImportedHostPointerAlignment` (16KB on macOS)
   - Sets `use_host_pointer_import = true`
   - Pretends to have VK_KHR_external_memory_fd for guest compatibility

2. **SHM Memory Path** (`vkr_device_memory.c:365-409`)
   - Creates anonymous SHM files via `os_create_anonymous_file()`
   - mmap's memory with proper alignment
   - Imports via `VkImportMemoryHostPointerInfoEXT`
   - Returns `VIRGL_RESOURCE_FD_SHM` type

3. **Resource Management** (`vkr_context.c`)
   - `vkr_context_create_resource_from_shm()` - creates SHM blobs
   - `vkr_context_import_resource_from_shm()` - imports SHM resources
   - Direct pointer access in `vkr_resource.u.data`

4. **Command Stream Integration** (`vkr_cs.c`, `vkr_transport.c`)
   - Direct pointer access to SHM buffers
   - No fd-based indirection needed

### The Problem: Context Creation Fails

Despite the infrastructure being in place, Venus context creation fails:

```
VKR_DEBUG: context_create: VENUS capset, proxy_initialized=1
VKR_DEBUG: proxy_context_create returned ctx=0x0
```

The render server logs show:
```
invalid client op 8
```

### Failure Chain Analysis

1. QEMU calls `virgl_renderer_context_create()` with VENUS capset
2. Proxy client sends CREATE_CONTEXT to render server
3. Render server spawns worker thread/subprocess
4. Worker calls `render_context_main()` → `render_state_init()` → `vkr_renderer_init()`
5. Then `render_state_create_context()` → `vkr_renderer_create_context()` → `vkr_context_create()`
6. **Somewhere in this chain, context creation fails and returns NULL**

### Suspected Issues

1. **Protocol Version Mismatch?**
   - "invalid client op 8" suggests receiving op code outside valid range (0-4)
   - Could be version mismatch between QEMU and virgl_render_server

2. **Vulkan Instance Creation?**
   - vkr_context_create() eventually creates a Vulkan instance
   - MoltenVK might be rejecting something during instance creation

3. **Missing Debug Output**
   - Need to add more logging to pinpoint exact failure location

## MoltenVK Compatibility Confirmed

Host vulkaninfo shows VK_EXT_external_memory_host is available:
```
VK_EXT_external_memory_host : extension revision 1
VK_EXT_external_memory_metal : extension revision 1
VK_KHR_external_memory : extension revision 1
```

## Architecture Summary

### Linux Path (fd-based):
```
Guest vkAllocateMemory(HOST_VISIBLE)
  → virglrenderer exports via VK_KHR_external_memory_fd
  → GetMemoryFdKHR returns opaque/dmabuf fd
  → fd shared with guest via virtio-gpu blob
```

### macOS Path (host pointer):
```
Guest vkAllocateMemory(HOST_VISIBLE)
  → virglrenderer creates SHM file
  → mmap's into host address space
  → Imports via VkImportMemoryHostPointerInfoEXT
  → SHM fd shared with guest (guest mmaps it)
  → Direct pointer access for command streams
```

## Next Steps

### 1. Add Debug Logging
Add logging to pinpoint exact failure:
- `vkr_context_create()` entry/exit
- Vulkan instance creation result
- Physical device enumeration
- Extension availability check

### 2. Check Protocol Version
Verify QEMU and virglrenderer are using compatible protocol:
- Check `RENDER_SERVER_VERSION` in both
- Verify socket communication works

### 3. Test Render Server Independently
Run virgl_render_server manually with debug:
```bash
VIRGL_DEBUG=all /opt/other/virglrenderer/install/libexec/virgl_render_server
```

### 4. Verify MoltenVK ICD Path
Ensure render server finds MoltenVK:
```bash
VK_ICD_FILENAMES=/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json \
  vulkaninfo --summary
```

### 5. Check Alignment Requirements
MoltenVK requires 16KB alignment for host pointer import.
Verify allocations respect this.

## Files Modified for Display (Working)

The software scanout for 2D resources is working:
- `hw/display/virtio-gpu-virgl.c` - Software scanout implementation

## Key Code Locations

| Component | File | Function |
|-----------|------|----------|
| Extension detect | vkr_physical_device.c:278 | (in enumeration) |
| SHM allocation | vkr_device_memory.c:365 | vkr_dispatch_vkAllocateMemory |
| SHM export | vkr_device_memory.c:645 | vkr_device_memory_export_blob |
| Context create | vkr_context.c | vkr_context_create |
| Render server | server/render_state.c | render_state_create_context |
| QEMU init | virtio-gpu-virgl.c:1500 | virtio_gpu_virgl_init |

## Summary

**The VK_EXT_external_memory_host support is already implemented in virglrenderer.**
The issue is that Venus context creation fails before it can use this path.
Need to debug why the render server rejects the context creation request.
