# Root Cause Found: Zero-Copy Milestone Broke Venus Rendering

## Investigation Summary

After extensive git history analysis, I found that Venus rendering broke due to coordinated "zero-copy milestone" commits made on **Jan 23, 2026** to both QEMU and virglrenderer.

## Breaking Commits

### QEMU: commit 37f2c7c205 (Jan 23 13:43:55)
```
milestone zero-copy triangle

Modified files:
- hw/display/virtio-gpu-virgl.c  (+322 lines!)
- hw/display/virtio-gpu-gl.c
- hw/display/virtio-gpu-vk-swapchain.m
- include/hw/virtio/virtio-gpu.h
- scripts/run-alpine.sh
- guest-demos/triangle/test_tri.c
```

### virglrenderer: commit f48b5b19 (Jan 23 13:45:03)
```
milestone zero-copy triangle

Modified files (17 files, 301 insertions):
- src/venus/vkr_device_memory.c
- src/venus/vkr_context.c
- src/venus/vkr_physical_device.c
- src/venus/vkr_renderer.c
- src/proxy/proxy_context.c
- server/render_context.c
- server/render_state.c
- src/virglrenderer.c
- And 9 more files...
```

## Last Known Working State

**Date**: Jan 22, 2026
**Performance**: 273 FPS (vkcube_anim)
**Commits**:
- QEMU: e3601ea0d0 "codex wip" (Jan 22 23:37:11)
- virglrenderer: 19cf9e77 "codex wip" (Jan 22 23:38:09)

## What the Zero-Copy Changes Tried to Do

According to the commit messages, these were "scaffolding hacks" for zero-copy rendering:

1. **Legacy scanout hook**: Bypassed normal scanout flow to force host-swapchain present
2. **Hostptr selection heuristic**: Added "keep largest hostptr" logic
3. **Context guessing**: Tracked "last Venus ctx_id" in QEMU
4. **Format assumptions**: Hard-coded XRGB8888 / PIXMAN_x8r8g8b8
5. **Debug logging**: Added temporary logs to /tmp/vkr_hostptr.log

The commits acknowledge these were "pragmatic shims" not meant for long-term use.

## Current Symptoms

With the zero-copy changes:
- ❌ `vkcube_anim`: Hangs with "vn_ring_submit abort on fatal"
- ❌ `test_tri_new`: Hangs indefinitely
- ✅ `test_minimal`: Works (no rendering)

## Additional Confusion

After the breaking zero-copy commits, there were follow-up commits that made things worse:

### virglrenderer: commit 9b0a9ab2 (Jan 25)
```
fix(venus): Add fallback to export fd from virgl_resource for SHM resources
```
This was an attempt to fix boot issues caused by the zero-copy changes, but it didn't resolve the rendering hangs.

### QEMU: vkcube_anim.c modifications (Jan 24)
```
feature(minor): Add 60 FPS vsync with double buffering to vkcube
```
This added double buffering which further complicated debugging.

## Solution

To restore working Venus rendering, we need to:

1. **Revert the zero-copy changes** in both repositories
2. **Return to the Jan 22 working state** (or newer, avoiding the zero-copy code)
3. **Test with the original simple rendering path** before attempting zero-copy again

The zero-copy optimization was premature and broke basic functionality. We should first ensure the simple copy-based path works reliably before attempting zero-copy optimizations.

## Lessons Learned

1. **Don't break working code for optimization** - The copy-based path was working at 273 FPS, which is excellent performance
2. **Test rigorously after large changes** - 600+ lines of changes across 23 files needed more testing
3. **Keep optimizations separate** - Zero-copy should have been on a branch, not committed to main while broken
4. **Document known issues clearly** - The commit says "scaffolding hacks to be removed" but doesn't say rendering will be completely broken

## Next Steps

1. Create a `revert-zero-copy` branch
2. Revert both zero-copy commits
3. Verify Venus works again
4. If needed, re-implement zero-copy **incrementally** with proper testing at each step
5. Keep the working copy-based path as fallback
