# Venus Swapchain Support on macOS Progress

## Summary

Successfully patched Mesa to expose VK_KHR_swapchain extension on macOS with MoltenVK where sync_fd is not available.

## Changes Made

### 1. vn_device.c (line 304-308)

**Original:**
```c
/* see vn_queue_submission_count_batch_semaphores */
if (!app_exts->KHR_external_semaphore_fd && has_wsi) {
   assert(physical_dev->renderer_sync_fd.semaphore_importable);
   extra_exts[extra_count++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
}
```

**Patched:**
```c
/* see vn_queue_submission_count_batch_semaphores
 * Only add external semaphore fd when the renderer supports sync_fd import.
 * On macOS with MoltenVK, sync_fd isn't available but WSI can still work
 * via the fallback fence wait mechanism in vn_wsi_fence_wait().
 */
if (!app_exts->KHR_external_semaphore_fd && has_wsi &&
    physical_dev->renderer_sync_fd.semaphore_importable) {
   extra_exts[extra_count++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
}
```

### 2. vn_physical_device.c (lines 1172-1181)

**Original:**
```c
#ifdef VN_USE_WSI_PLATFORM
   if (physical_dev->renderer_sync_fd.semaphore_importable) {
      exts->KHR_incremental_present = true;
      exts->KHR_swapchain = true;
      exts->KHR_swapchain_mutable_format = true;
      exts->EXT_hdr_metadata = true;
      exts->EXT_swapchain_maintenance1 = true;
   }
```

**Patched:**
```c
#ifdef VN_USE_WSI_PLATFORM
   /* Enable swapchain unconditionally - WSI has fallback for no sync_fd */
   exts->KHR_swapchain = true;
   if (physical_dev->renderer_sync_fd.semaphore_importable) {
      exts->KHR_incremental_present = true;
      exts->KHR_swapchain_mutable_format = true;
      exts->EXT_hdr_metadata = true;
      exts->EXT_swapchain_maintenance1 = true;
   }
```

## Verified Results

1. **vulkaninfo now shows VK_KHR_swapchain** - extension revision 70
2. **VkDevice creation works** - No more assertion failures
3. **vkcube starts with WSI** - Selects GPU, creates swapchain

## Current Issue

vkcube gets stuck during rendering:
```
MESA-VIRTIO: debug: stuck in fence wait with iter at 1024
MESA-VIRTIO: debug: aborting on expired ring alive status at iter 1024
```

This is a Venus protocol ring timeout, not a swapchain extension issue. The rendering commands are being submitted but the fence wait never completes.

## Potential Causes for Fence Timeout

1. Host-side virglrenderer may not be responding to ring commands
2. MoltenVK may have issues with the operations Venus is requesting
3. The blob memory/scanout path may need additional host-side work

## Progress Update (2026-01-21)

### What Works
- VK_KHR_swapchain exposed via pre-built aarch64 driver from `/opt/other/mesa/build-docker/`
- `vulkaninfo` shows swapchain extension present
- Alpine VM boots with Venus and detects virtio-gpu

### Current Problem
vkcube crashes with assertion before rendering starts:
```
Assertion failed: physical_dev->renderer_sync_fd.semaphore_importable
(../src/virtio/vulkan/vn_device.c: vn_device_fix_create_info: 331)
```

The pre-built driver has the vn_physical_device.c patch (exposes swapchain) but NOT the vn_device.c patch (removes assertion).

### Attempted Solutions
1. **Build Mesa 25.0.2 in VM** - Completed meson configure, but:
   - Build directory at `/root/mesa-25.0.2` points to old `/tmp/` path
   - Need to reconfigure with correct source path

2. **Applied both patches to VM Mesa source**:
   - `/root/mesa-25.0.2/src/virtio/vulkan/vn_physical_device.c` ✓
   - `/root/mesa-25.0.2/src/virtio/vulkan/vn_device.c` ✓

### Build Configuration Issue
The meson build was configured with source at `/tmp/mesa-25.0.2`, but we copied to `/root/mesa-25.0.2`. The build.ninja still references the old path, causing:
```
ERROR: Neither source directory '/tmp/mesa-25.0.2' nor build directory '.' contain a build file meson.build.
```

### Next Steps
1. **Reconfigure Mesa build in VM** with correct paths
2. **Complete ninja build** of patched Mesa
3. **Copy built libvulkan_virtio.so to /usr/lib/**
4. **Test vkcube** - should pass device creation assertion
5. **If renders**: original fence timeout issue may still appear
6. **Debug host-side** virglrenderer if fence timeout persists

### Files Needed in VM
Both patches must be present:
- `vn_physical_device.c` - Enables KHR_swapchain without sync_fd requirement
- `vn_device.c` - Removes assertion requiring semaphore_importable

### Commands to Run in VM
```bash
cd /root/mesa-25.0.2
rm -rf build
meson setup build \
    -Dprefix=/usr \
    -Dgallium-drivers= \
    -Dvulkan-drivers=virtio \
    -Dplatforms=x11,wayland \
    -Dglx=disabled \
    -Degl=disabled \
    -Dgles1=disabled \
    -Dgles2=disabled \
    -Dllvm=disabled \
    -Dvalgrind=disabled
ninja -C build
cp build/src/virtio/vulkan/libvulkan_virtio.so /usr/lib/
xvfb-run -a vkcube
```

## Progress Update (2026-01-21 Evening)

### Major Breakthrough: VK_KHR_display Now Works!

**Problem solved**: Venus driver wasn't enumerating DRM displays for VK_KHR_display WSI.

**Root cause**: `vn_wsi_init()` passed `fd=-1` to `wsi_device_init()`, disabling display enumeration.

**Fix applied** to `/opt/other/mesa/src/virtio/vulkan/vn_wsi.c`:
```c
/* Try to open DRM primary node for VK_KHR_display support */
int display_fd = -1;
drmDevicePtr devices[8];
int num_devices = drmGetDevices2(0, devices, 8);
for (int i = 0; i < num_devices; i++) {
   if (devices[i]->available_nodes & (1 << DRM_NODE_PRIMARY)) {
      display_fd = open(devices[i]->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
      if (display_fd >= 0)
         break;
   }
}
drmFreeDevices(devices, num_devices);
// ... pass display_fd to wsi_device_init() ...
```

**Result**:
```
$ /root/test_display
Instance OK
Display count: 1
  Display 0: monitor (1280x800)
```

### Test Results

| Test | Result |
|------|--------|
| Compute pipeline (vkCmdBindPipeline) | ✅ PASS |
| Graphics pipeline (render pass + bind) | ✅ PASS |
| VK_KHR_display enumeration | ✅ PASS (1 display found) |
| vkcube --wsi display | ❌ CRASH (resource creation) |

### Current Issue: Blob Resource Creation Fails

vkcube crashes with DRM errors during scanout setup:
```
[drm:virtio_gpu_dequeue_ctrl_func] *ERROR* response 0x1200 (command 0x10c)
[drm:virtio_gpu_dequeue_ctrl_func] *ERROR* response 0x1203 (command 0x102)
```

- 0x10c = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB → UNSPEC error
- 0x102 = VIRTIO_GPU_CMD_RESOURCE_CREATE → OUT_OF_MEMORY error

The blob scanout resource creation is failing on the host side.

### HVF Issue Analysis (2026-01-21)

**Root Cause Identified**: Guest Linux kernel uses 4KB pages, but HVF requires 16KB alignment.

**Debug Evidence** (strace on HVF VM):
```
[pid  2040] ioctl(3, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, ...) = 0
[pid  2040] ioctl(3, DRM_IOCTL_VIRTGPU_MAP, ...) = 0
[pid  2040] mmap(NULL, 131268, ..., 3, 0x100409000) = 0xffff94897000
[hangs after this - guest waiting for host response via ring buffer]
```

Offset `0x100409000` is only 4KB aligned (0x409000 % 16384 = 4096), NOT 16KB aligned.

**Flow**:
1. Guest Linux virtio-gpu driver allocates blob offset using kernel page size (4KB)
2. QEMU's `memory_region_add_subregion()` places blob at that offset in hostmem BAR
3. HVF's `hvf_set_phys_mem()` sees non-16KB-aligned region and sets `add=false`
4. Region falls back to MMIO emulation - extremely slow or broken
5. Guest ring buffer writes not visible to host → hang

**Solutions** (in order of preference):
1. **Guest kernel with 16KB pages** - Rebuild with `CONFIG_ARM64_16K_PAGES=y`
   - Natural fix: all kernel allocations become 16KB aligned
   - Requires custom Alpine kernel or use different distro

2. **QEMU alignment enforcement** - Added in virtio-gpu-virgl.c
   - Detects and reports alignment errors when HVF is active
   - Helps identify issues but doesn't fix the guest-side allocation

3. **Host-side aligned wrapper** - Complex, not recommended
   - Would need to intercept and realign all blob mappings
   - Significant performance overhead from copy

**Current Status**: TCG works, HVF requires 16KB-page guest kernel.

### Build Notes

Cross-compile Mesa for aarch64:
```bash
cd /opt/other/mesa
docker run --rm -v $(pwd):/mesa -w /mesa --platform linux/arm64 alpine:edge sh -c '
apk add meson ninja gcc g++ pkgconf libdrm-dev vulkan-headers wayland-dev \
        wayland-protocols libxkbcommon-dev libx11-dev libxcb-dev \
        xcb-util-keysyms-dev python3 py3-mako py3-yaml py3-packaging \
        flex bison glslang linux-headers libxshmfence-dev libxrandr-dev
ninja -C builddir-arm64 src/virtio/vulkan/libvulkan_virtio.so
'
scp -P 2222 builddir-arm64/src/virtio/vulkan/libvulkan_virtio.so root@localhost:/usr/lib/
```

Guest needs: `apk add xcb-util-keysyms`

### Next Steps

1. **Debug blob creation failure** - Check QEMU/virglrenderer logs
2. **Investigate hostmem allocation** - May need more hostmem or different config
3. **Fix HVF 16KB alignment** - Required for acceptable performance
