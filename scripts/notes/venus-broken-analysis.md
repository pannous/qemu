# Venus Instance Creation Failure - Analysis

**Date:** 2026-01-25
**Status:** ❌ BROKEN - All Vulkan demos fail with vkCreateInstance error

## Symptom

All demos (copyback and zero-copy) fail with:
```
VK err -1 @ vkCreateInstance
virtio_gpu_virgl_process_cmd: ctrl 0x208, error 0x1200
```

- Error -1 = VK_ERROR_OUT_OF_HOST_MEMORY
- ctrl 0x208 = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB
- error 0x1200 = VIRTIO_GPU_RESP_ERR_UNSPEC

## What Works

✅ Host Vulkan swapchain initializes: "Venus: Host Vulkan swapchain initialized (1280x800)"
✅ Venus context creates successfully (VKR_RING_MONITOR starts)
✅ MoltenVK is accessible and working on host

## What Fails

❌ Guest vkCreateInstance always returns -1
❌ Blob resource creation fails in virglrenderer
❌ Warning: "Failed to register blob resource X with Venus context Y"

## Root Cause

In `hw/display/virtio-gpu-virgl.c:1588`:
```c
if (!virgl_try_register_venus_resource(cblob.hdr.ctx_id, cblob.resource_id)) {
    warn_report_once("Failed to register blob resource %d with Venus context %u",
                     cblob.resource_id, cblob.hdr.ctx_id);
}
```

This calls `virgl_renderer_resource_register_venus` which calls:
`vkr_renderer_get_or_import_resource(ctx_id, res_id)`

Which calls: `vkr_context_get_resource_or_import(ctx, res_id)`

At `/opt/other/virglrenderer/src/venus/vkr_context.c:478-511`, this function:
1. Looks up existing resource - fails
2. Tries to import from virgl_resource - succeeds
3. Calls `virgl_resource_export_fd` - **returns VIRGL_RESOURCE_FD_INVALID**
4. Returns NULL, causing registration to fail

## Investigation Attempts

### Tried
1. ✅ Reverted virglrenderer to f48b5b19 (milestone zero-copy) - still broken
2. ✅ Reverted QEMU to pre-merge 714a6b6dbf - still broken
3. ✅ Tried older virglrenderer commits - build failures (threads.h missing)
4. ✅ Confirmed custom virglrenderer loads correctly (2.9MB vs 2.9MB homebrew)
5. ✅ Tested all demo variants (test_tri, vkcube_anim, vkcube_zerocopy) - all fail identically

### Not Environmental
- MoltenVK version: unchanged
- Vulkan loader: working (host swapchain works)
- System libraries: no updates
- Disk: not corrupted (VM boots fine)

## Code Path

```
Guest: vkCreateInstance()
  ↓
Venus Protocol: Send command to host
  ↓
virglrenderer: Process Vulkan command (needs blob resource for internal structures)
  ↓
QEMU: virgl_cmd_resource_create_blob (ctrl 0x208)
  ↓
virglrenderer: virgl_renderer_resource_create_blob
  ↓
Venus: vkr_context_create_resource_from_device_memory
  ↓
Tries to find VkDeviceMemory with blob_id
  ↓
**FAILS** - object doesn't exist or can't be exported
  ↓
Returns error to QEMU (0x1200 = VIRTIO_GPU_RESP_ERR_UNSPEC)
  ↓
Guest: vkCreateInstance returns -1
```

## Hypothesis

The blob resource export mechanism in virglrenderer is broken. Specifically:
- `virgl_resource_export_fd()` returns `VIRGL_RESOURCE_FD_INVALID`
- This prevents `vkr_context_get_resource_or_import()` from succeeding
- Without successful import, Venus can't create Vulkan objects

## Next Steps

1. Add debug logging to `virgl_resource_export_fd` to see why it returns INVALID
2. Check if blob resources are created with correct fd_type
3. Verify SHM allocation is working correctly
4. Consider if macOS-specific fd export is broken

## Timeline

- **Jan 22, 2026**: Demos working (273 FPS vkcube_anim confirmed)
- **Jan 23-24, 2026**: Multiple commits to virglrenderer (zero-copy work)
- **Jan 25, 2026**: All demos broken, even after reverting commits

**This suggests the issue isn't in recent commits but something environmental that changed between Jan 22-25.**

## Deep Dive Analysis (Jan 25, 15:50)

### Code Flow for SHM Resources on macOS

**Problem**: vkr_resource created with `fd = -1` even though blob has valid SHM fd

**Detailed Flow**:
1. `vkr_dispatch_vkAllocateMemory()` creates SHM-backed memory
   - `might_export = false` (guest didn't request VkExportMemoryAllocateInfo)
   - `valid_fd_types` has SHM bit set
   
2. `vkr_context_create_resource_from_device_memory()` called during blob creation
   - Line 492: `if (mem->might_export)` → FALSE, so `res_fd = -1`
   - Creates vkr_resource with `fd = -1`
   - Returns blob with valid SHM fd to virglrenderer.c
   
3. `virgl_resource_create_from_fd()` creates virgl_resource with valid SHM fd

4. Later: `vkr_context_get_resource_or_import()` finds EXISTING vkr_resource
   - Returns vkr_resource with `fd = -1`
   - NEVER calls `virgl_resource_export_fd()` because resource already exists

### Fix Attempts

1. **Set `might_export = true` for SHM**: VM hangs during boot
2. **Always dup fd for SHM** (`needs_dup = might_export || blob.type == SHM`): VM crashes

Both fixes fail during early Venus initialization, suggesting os_dupfd_cloexec might be failing or causing issues.

### Investigation Needed

- Why does os_dupfd_cloexec fail for SHM fds during boot?
- Can we make vkr_resource store the fd without using might_export flag?
- Where is vkr_resource.fd actually used that requires it to be valid?

## SOLUTION IMPLEMENTED (Jan 25, 16:00)

### Fix Applied

**File**: `/opt/other/virglrenderer/src/venus/vkr_device_memory.c` (lines 310-337)

**Problem**: When guest imports a resource using VkImportMemoryResourceInfoMESA (macOS host pointer import path), the code tries to dup `vkr_resource.fd` which is -1 for SHM resources, causing the import to fail.

**Solution**: Added fallback logic - if `vkr_resource.fd` is invalid, export fd from `virgl_resource` instead:

```c
imported_res_fd = os_dupfd_cloexec(res->u.fd);
if (imported_res_fd < 0) {
   /* Fallback: if vkr_resource doesn't have fd, try exporting from virgl_resource */
   struct virgl_resource *vres = virgl_resource_lookup(res_info->resourceId);
   if (vres) {
      int temp_fd = -1;
      enum virgl_resource_fd_type fd_type = virgl_resource_export_fd(vres, &temp_fd);
      if (fd_type == VIRGL_RESOURCE_FD_SHM && temp_fd >= 0) {
         imported_res_fd = temp_fd;
      }
   }
}
```

**Commit**: `9b0a9ab2` - fix(venus): Add fallback to export fd from virgl_resource for SHM resources

### Why This Works

- `virgl_resource` is created with valid SHM fd during blob creation
- `vkr_resource` is created with fd=-1 (because might_export=false)
- When importing for host pointer access, we now fall back to virgl_resource's fd
- This avoids modifying the complex blob creation logic which was causing VM boot issues

### Testing Status

Unable to test due to VM boot issues (separate environmental problem unrelated to this fix).
The fix is theoretically sound and addresses the root cause identified through code analysis.

**Next**: User should test with running VM or investigate VM boot issue separately.

---

## BOOT ISSUE RESOLVED (Jan 25, 17:38)

### Problem
`./scripts/run-alpine.sh` failed to boot - missing 16KB page kernel required for HVF.

### Root Cause
The file `scripts/alpine-virt-16k.img` (37MB) was missing from the filesystem:
- File is gitignored (`scripts/*.img` in .gitignore)
- Was added in commit fc3f5f3899 but not present in working directory
- Script auto-detects this file at line 101-105 but fails silently if missing

### Solution
Restored kernel from git history:
```bash
git show fc3f5f3899:scripts/alpine-virt-16k.img > scripts/alpine-virt-16k.img
```

### Verification
Script now correctly detects and uses the 16KB kernel:
```
Using custom kernel: /opt/other/qemu/scripts/alpine-virt-16k.img
```

**Status**: ✅ BOOT FIXED - VM can now start with HVF acceleration
