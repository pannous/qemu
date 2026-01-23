# Zero-Copy Improvements Plan

## Immediate Improvements (This Session)

### 1. Remove Debug Logging ✓
- Remove all `vkr_hostptr_log()` calls (43 instances)
- Remove `/tmp/vkr_hostptr.log` file creation
- Clean compile warnings

### 2. Add Resource → Hostptr Binding
**Problem:** Currently using global `last_venus_ctx_id` heuristic
**Solution:** Per-resource tracking structure

```c
struct VirtIOGPUVenusResource {
    uint32_t resource_id;
    uint32_t ctx_id;
    void *hostptr;
    uint64_t hostptr_size;
    QTAILQ_ENTRY(VirtIOGPUVenusResource) next;
};

// In VirtIOGPUGL:
QTAILQ_HEAD(, VirtIOGPUVenusResource) venus_resources;
```

**Implementation:**
- Track hostptr in `virgl_cmd_resource_create_blob` when Venus creates resource
- Lookup by resource_id in scanout path (not context_id)
- Remove `last_venus_ctx_id` global tracking

### 3. Multi-Format Support (Partial)
**Problem:** Hardcoded PIXMAN_x8r8g8b8 / XRGB8888
**Solution:** Add format detection helper

```c
pixman_format_code_t virgl_to_pixman_format(uint32_t virgl_format) {
    switch (virgl_format) {
        case VIRGL_FORMAT_B8G8R8X8_UNORM: return PIXMAN_x8r8g8b8;
        case VIRGL_FORMAT_B8G8R8A8_UNORM: return PIXMAN_a8r8g8b8;
        case VIRGL_FORMAT_R8G8B8X8_UNORM: return PIXMAN_x8b8g8r8;
        case VIRGL_FORMAT_R8G8B8A8_UNORM: return PIXMAN_a8b8g8r8;
        default: return PIXMAN_x8r8g8b8; // fallback
    }
}
```

### 4. Environment Variable Cleanup
**Remove:**
- `VKR_PRESENT_HOSTPTR` - make zero-copy default behavior
- `VKR_PRESENT_TIMER` - make timer-based present default

**Keep:** Make behavior automatic, no environment variables needed

## Later Improvements (Future PRs)

### 5. SET_SCANOUT_BLOB Path (Needs Venus Driver Changes)
**Problem:** Using legacy SET_SCANOUT instead of SET_SCANOUT_BLOB
**Blockers:**
- Mesa Venus driver needs to use SET_SCANOUT_BLOB for headless scanout
- Guest vkcube demo needs to trigger proper blob scanout
**Timeline:** After guest driver updates

### 6. Proper Resource Query in Proxy Mode
**Problem:** `virgl_renderer_resource_get_info` fails in proxy mode
**Solution:** Implement proxy-mode resource query RPC
**Timeline:** Needs virglrenderer changes

## Files to Modify (This Session)

1. `hw/display/virtio-gpu-virgl.c` - Main improvements
2. `include/hw/virtio/virtio-gpu.h` - Add venus_resources tracking
3. `/opt/other/virglrenderer/src/venus/vkr_device_memory.c` - Clean logging
4. `/opt/other/virglrenderer/src/venus/vkr_context.c` - Clean logging

## Testing After Changes

```bash
cd /opt/other/virglrenderer && ninja -C builddir install
cd /opt/other/qemu && make -j8
./scripts/run-alpine.sh

# In guest:
cd /root/vkcube
./vkcube_anim  # Should still show 47 FPS
```

## Success Criteria

- ✓ No debug logging in production code
- ✓ No environment variables required for zero-copy
- ✓ Proper resource tracking (remove last_venus_ctx_id)
- ✓ Same or better performance (47 FPS maintained)
- ✓ Cleaner, more maintainable code
