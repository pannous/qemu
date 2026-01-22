# Venus Investigation Summary

## Status: WORKING ✅

Venus Vulkan rendering works on macOS via MoltenVK. Spinning cube demo runs at 700-1200 fps.

## Previously Suspected Issues (RESOLVED)

### ~~Issue 1: vkGetDeviceQueue sets context fatal~~
**Status**: NOT AN ISSUE - works fine in practice. Our demos call `vkGetDeviceQueue` without problems.

### ~~Issue 2: Queue Submission Fences Never Signal~~
**Status**: NOT AN ISSUE - fences work correctly. Tested with thousands of frames.

## Actual Issue Found

### GBM Buffer Mapping Fails After Scanout
**This was the real bug causing black screens in animation loops.**

When a GBM buffer is displayed via `drmModeSetCrtc`, subsequent `gbm_bo_map()` calls return NULL:
```
Frame 0: gbm_bo_map: 0x7fff75f34000 (success)
Frame 1: gbm_bo_map: 0 (FAILED - buffer locked for scanout)
```

**Solution**: Use DRM dumb buffers instead of GBM for animation:
```c
struct drm_mode_create_dumb create = {.width=W, .height=H, .bpp=32};
drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
struct drm_mode_map_dumb map = {.handle = create.handle};
drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
void *ptr = mmap(NULL, create.size, PROT_READ|PROT_WRITE, MAP_SHARED, drm_fd, map.offset);
// ptr stays valid even while buffer is displayed!
```

## Working Demos

| Demo | Location | Description |
|------|----------|-------------|
| `test_tri` | `guest-demos/triangle/` | RGB triangle, single frame, GBM |
| `vkcube_anim` | `guest-demos/vkcube/` | Spinning cube, animation, DRM dumb |

## Architecture

```
Guest (Alpine)                    Host (macOS)
┌─────────────────┐              ┌─────────────────┐
│ vkcube_anim     │              │                 │
│ ↓               │              │                 │
│ Mesa Venus      │──virtio-gpu──│ virglrenderer   │
│ (VK driver)     │              │ (Venus backend) │
│                 │              │ ↓               │
│                 │              │ MoltenVK        │
│                 │              │ ↓               │
│                 │              │ Metal           │
└─────────────────┘              └─────────────────┘
```

## Performance

- ~700-1200 fps for spinning cube on Apple M2 Pro
- Vulkan fence synchronization works correctly
- UBO updates work per-frame
- Depth testing works
