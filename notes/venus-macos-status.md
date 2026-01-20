# Venus on macOS - Current Status (2026-01-20)

## Summary

Venus (Vulkan-over-virtio) now works on macOS with MoltenVK. The full rendering + display pipeline has been verified.

## What Works

| Feature | Status | Notes |
|---------|--------|-------|
| Venus protocol | ✅ Working | Commands forwarded to MoltenVK |
| Vulkan instance/device | ✅ Working | "Virtio-GPU Venus (Apple M2 Pro)" |
| HOST_VISIBLE memory | ✅ Working | Via VK_EXT_external_memory_host + SHM |
| vkMapMemory | ✅ Working | Fixed SHM validation in virglrenderer |
| Blob resources | ✅ Working | GBM creates blob-backed buffers |
| DRM scanout | ✅ Working | SET_SCANOUT_BLOB triggers display |
| Host Vulkan swapchain | ✅ Working | MoltenVK → CAMetalLayer |

## Key Fixes Made

### 1. VK_EXT_external_memory_host Fallback (virglrenderer)
MoltenVK lacks `VK_KHR_external_memory_fd`. Implemented SHM-backed memory import:
- Create anonymous SHM file
- mmap to host pointer
- Import via `VkImportMemoryHostPointerInfoEXT`
- Export as `VIRGL_RESOURCE_FD_SHM` blob

### 2. SHM Size Validation Fix (virglrenderer)
macOS requires 16KB alignment for host pointer import. Changed validation from `size == expected` to `size >= expected`.

**Commit**: `0b3d075a`

### 3. MoltenVK Portability Extensions (QEMU)
Added required MoltenVK extensions to host swapchain:
- `VK_KHR_portability_enumeration`
- `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`
- `VK_KHR_portability_subset`

## Test Results

### Memory Mapping Test
```
vkAllocateMemory: 0
vkMapMemory: 0 ptr=0xffffb3fbc000
write OK!
```

### Blob Scanout Test
```
Display: 1280x800
GBM BO: stride=5120, prime_fd=6
Vulkan: Virtio-GPU Venus (Apple M2 Pro)
Vulkan: rendered 1280x800 blue
Copied to GBM buffer
Blue screen for 3s...
Done!
```

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                         GUEST                               │
├─────────────────────────────────────────────────────────────┤
│  Vulkan App → Mesa Venus Driver → virtio-gpu commands       │
│                    ↓                                        │
│  GBM buffer ← VK render ← HOST_VISIBLE memory (SHM-backed) │
│       ↓                                                     │
│  DRM scanout (drmModeSetCrtc)                               │
└───────────────────────────┬─────────────────────────────────┘
                            │ SET_SCANOUT_BLOB
┌───────────────────────────┴─────────────────────────────────┐
│                          HOST                               │
├─────────────────────────────────────────────────────────────┤
│  QEMU virtio-gpu-virgl.c                                    │
│       ↓                                                     │
│  virglrenderer + Venus backend                              │
│       ↓                                                     │
│  MoltenVK (Vulkan → Metal)                                  │
│       ↓                                                     │
│  Host Vulkan Swapchain → CAMetalLayer → macOS display       │
└─────────────────────────────────────────────────────────────┘
```

## Next Steps

### 1. Test vkcube/kmscube with Venus
- Need to build apps that use the Venus Vulkan path
- kmscube currently falls back to llvmpipe (OpenGL)
- vkcube needs VK_KHR_swapchain which Venus doesn't expose

### 2. Implement Direct Blob Scanout
Current flow copies data through staging buffer. Optimize:
- Direct VkImage from blob memory
- Zero-copy via IOSurface-Vulkan interop (MTLTexture)

### 3. Add Venus Swapchain Support
Two options:
- **Option A**: Implement VK_KHR_swapchain proxy in virglrenderer
- **Option B**: Keep current GBM→DRM flow, improve performance

### 4. Multi-Display Support
Current implementation: single display only.

### 5. Performance Profiling
- Measure frame timing
- Compare to native Vulkan
- Identify bottlenecks

## Files Modified

### QEMU
- `hw/display/virtio-gpu-vk-swapchain.m` - Host Vulkan swapchain
- `hw/display/virtio-gpu-virgl.c` - Blob scanout integration
- `ui/cocoa.m` - CAMetalLayer support

### virglrenderer
- `src/venus/vkr_device_memory.c` - VK_EXT_external_memory_host path
- `src/venus/vkr_physical_device.c` - Extension detection
- `src/proxy/proxy_context.c` - SHM size validation fix
- `src/proxy/proxy_socket.c` - macOS socket framing
- `server/render_socket.c` - macOS socket framing

## Running

```bash
# Start VM with Venus
./scripts/run-alpine.sh

# In guest, verify Venus works
vulkaninfo | grep "Virtio-GPU Venus"

# Test memory mapping
/tmp/test_mem

# Test blob scanout
/tmp/test_blob
```
