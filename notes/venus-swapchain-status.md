# Venus Swapchain Status - 2026-01-20

## Summary

Venus blob scanout works, but vkcube fails because Mesa doesn't expose VK_KHR_swapchain.

## Working Components

1. **DRM/KMS Scanout**: Direct framebuffer works via dumb buffers
2. **Venus Protocol**: Commands forwarded to MoltenVK correctly
3. **Fence Workaround**: `VN_PERF=no_fence_feedback` fixes fence signaling
4. **Host Vulkan Swapchain**: QEMU's `virtio-gpu-vk-swapchain.m` implementation exists

## Blocked

### vkcube Error
```
vkEnumerateDeviceExtensionProperties failed to find the VK_KHR_swapchain extension.
```

### Root Cause Analysis

Mesa Venus driver (vn_physical_device.c) has:
```c
#ifdef VN_USE_WSI_PLATFORM
   exts->KHR_swapchain = true;
#endif
```

The Alpine `mesa-vulkan-virtio` package:
- Links against WSI libraries (libxcb, libxcb-dri3, etc.)
- Contains "VK_KHR_swapchain" string in binary
- Does NOT advertise VK_KHR_swapchain at runtime

This suggests either:
1. `VN_USE_WSI_PLATFORM` not defined at compile time
2. Runtime bug in extension enumeration
3. Mesa build configuration issue

### Evidence
- `vulkaninfo` shows 104 device extensions, NO swapchain
- Instance has surface extensions (VK_KHR_surface, xcb_surface, xlib_surface)
- `strings libvulkan_virtio.so | grep KHR_swapchain` finds the string
- Mesa version: 26.0.0-devel (git-c3f7d9bd1e)

## Tested

| Test | Result |
|------|--------|
| DRM dumb buffer → CRTC | ✅ Works (red screen) |
| GBM buffer → CRTC | ✅ Works (gradient) |
| X11 + vkcube | ❌ Missing VK_KHR_swapchain |
| Vulkan device enumeration | ✅ "Virtio-GPU Venus (Apple M2 Pro)" |
| Vulkan fence + VN_PERF | ✅ Works with workaround |

## Test Commands

```bash
# In Alpine guest:
# Kill X first to get DRM master
kill -9 $(pgrep Xorg) 2>/dev/null

# DRM dumb buffer test (shows red)
/tmp/test_drm

# GBM buffer test (shows green-blue gradient)
/tmp/test_gbm
```

## Next Steps

### Option A: Build Custom Mesa (Recommended)
1. Clone mesa from /opt/other/mesa
2. Configure with `-Dvulkan-drivers=virtio -Dplatforms=x11`
3. Verify `VN_USE_WSI_PLATFORM` is defined
4. Install in Alpine guest

### Option B: Test Blob Scanout Without Swapchain
1. Create Venus blob resource
2. Render to blob with Vulkan
3. Use SET_SCANOUT_BLOB to display
4. Bypasses need for guest swapchain

### Option C: Investigate Alpine Build
1. Check Alpine's mesa APKBUILD
2. Verify meson configuration options
3. Report bug if VN_USE_WSI_PLATFORM should be set

## Files

| File | Purpose |
|------|---------|
| `/opt/other/mesa/src/virtio/vulkan/vn_physical_device.c:1211-1217` | Swapchain extension enable |
| `/opt/other/mesa/src/virtio/vulkan/meson.build:118-123` | VN_USE_WSI_PLATFORM condition |
| `/opt/other/qemu/hw/display/virtio-gpu-vk-swapchain.m` | Host-side swapchain |

## Environment

- Guest: Alpine Linux edge (aarch64)
- Mesa: 26.0.0-devel
- Host: macOS with MoltenVK 1.4.0
- QEMU: virtio-gpu with venus=on, blob=on
