# Venus Hang Investigation - 2026-01-19

                                                                                                            
  Issue 1: HVF 16KB Page Alignment (FIXED)                                                                                   
                                                                                                                             
  - macOS Apple Silicon uses 16KB pages, guest uses 4KB                                                                      
  - HVF silently fails to map non-16KB-aligned blob memory                                                                   
  - Workaround: Using TCG instead of HVF fixes this                                                                          
                                                                                                                             
  Issue 2: VK_KHR_external_memory_fd Requirement (BLOCKING)                                                                  
                                                                                                                             
  This is the fundamental blocker. The Venus protocol chain:                                                                 
                                                                                                                             
  Host (virglrenderer) 
                        → enumerates MoltenVK device (SUCCESS, count=1)                                                       
                       → reports device to guest                                                                             
  Guest (Mesa Venus)  
                      → checks for VK_KHR_external_memory_fd                                                                 
                      → MoltenVK doesn't have it                                                                             
                      → REJECTS device → VK_ERROR_INITIALIZATION_FAILED                                                      
                                                                                                                             
  MoltenVK's external memory extensions:                                                                                     
  - VK_EXT_external_memory_host ✅                                                                                           
  - VK_EXT_external_memory_metal ✅                                                                                          
  - VK_KHR_external_memory_fd ❌ (POSIX-specific, not on macOS)                                                              
  - VK_EXT_external_memory_dma_buf ❌ (Linux-specific)                                                                       
                                                                                                                             
  The Mesa Venus driver requires fd-based external memory for sharing memory between guest and host. This is a fundamental   
  architecture decision - Venus was designed for Linux virtualization.                                                       
                                                               

## Summary
**STATUS: BLOCKED - Fundamental incompatibility discovered**

Venus/Vulkan on macOS via MoltenVK has two issues:
1. ~~HVF 16KB page alignment causes blob mapping failures~~ → Fixed by using TCG
2. **Mesa Venus driver requires `VK_KHR_external_memory_fd`** → MoltenVK doesn't support it

The guest driver rejects MoltenVK because it lacks POSIX fd-based external memory support.
This is a fundamental architecture mismatch - Venus was designed for Linux.

## Fixed Issues
1. **HVF Memory Unmap Crash** - Fixed in `accel/hvf/hvf-all.c`
   - `hv_vm_unmap` failed with HV_BAD_ARGUMENT when trying to unmap blob memory that was never mapped
   - Fix: Ignore HV_BAD_ARGUMENT errors since unmapping non-mapped regions is harmless

2. **SSH/Network** - Fixed static IP configuration for QEMU user networking
   - AF_PACKET issue prevents DHCP, use static IP: 10.0.2.15/24 gateway 10.0.2.2

## Current Status: Guest Driver Hang

### What Works
- QEMU boots with HVF + virtio-gpu-gl + Venus
- MoltenVK initialized (Metal shader cache created)
- Venus capset advertised (id=4, size=160)
- virtio-gpu context created
- Blob resources created and mapped
- Venus proxy context created successfully

### What Fails
- Guest Mesa Venus driver hangs after context creation
- NO `SUBMIT_3D` commands seen in QEMU traces
- `vkCreateInstance` blocks indefinitely
- The ring buffer for command transport is not being set up

### Trace Evidence
```
virtio_gpu_cmd_ctx_create ctx 0x2, name test_vk
VKR_DEBUG: context_create: VENUS capset, proxy_initialized=1
VKR_DEBUG: proxy_context_create returned ctx=...
VKR_DEBUG: vkr_renderer_create_context: success!
virtio_gpu_cmd_res_create_blob res 0x3, size 135168
virtio_gpu_cmd_res_map_blob res 0x3
virtio_gpu_cmd_ctx_res_attach ctx 0x2, res 0x3
<-- HANGS HERE, no ctx_submit ever appears -->
```

### Root Cause Analysis - FOUND!

**The issue is page size mismatch between host and guest:**
- macOS on Apple Silicon uses **16KB pages** (`pagesize` = 16384)
- Guest Alpine Linux uses **4KB pages**
- HVF `hv_vm_map()` requires 16KB alignment on Apple Silicon
- Blob allocations are only 4KB aligned, so HVF **silently fails** to map them
- When guest accesses unmapped memory, it hangs on unresolved page fault

**Evidence:**
```
virtio_gpu_cmd_res_create_blob res 0x3, size 4096  <-- Only 4KB, not 16KB aligned!
```

**HVF behavior from QEMU code** (`accel/hvf/hvf-all.c`):
```c
if (!QEMU_IS_ALIGNED(size, page_size) ||
    !QEMU_IS_ALIGNED(gpa, page_size)) {
    /* Not page aligned, so we can not map as RAM */
    add = false;  // <-- Silently skips mapping!
}
```

### Solution Options
1. **Align blob sizes to host page size** - Round up to 16KB on macOS
2. **Use TCG instead of HVF** - TCG can emulate unaligned accesses (but slower)
3. **Wait for upstream fix** - There's an RFC patch for checking page alignment

### Previous Incorrect Analysis
~~The Venus protocol flow was incorrectly identified as the issue.~~ The actual problem is:
1. ✅ Create DRM context with VENUS capset
2. ✅ Create blob for command buffer (4KB)
3. ❌ HVF silently fails to map 4KB blob (requires 16KB alignment)
4. ❌ Guest tries to access blob memory → page fault → hang
5. ❌ No SUBMIT_3D because driver hangs before it can send commands

## TCG Workaround - Partial Success

Using TCG instead of HVF fixes the blob memory mapping issue:

```bash
QEMU_ACCEL=tcg ./scripts/run-alpine.sh  # Or just run default (now TCG)
```

### What Works with TCG
- ✅ Blob creation and mapping
- ✅ Memory read/write to blob (test_blob_mmap passes completely)
- ✅ Venus context creation
- ✅ vkCreateInstance succeeds
- ✅ SUBMIT_3D commands go through to host virglrenderer

### What Still Fails
- ❌ vkEnumeratePhysicalDevices returns -3 (VK_ERROR_INITIALIZATION_FAILED)
- Host side successfully enumerates 1 device (MoltenVK/Metal)
- Guest doesn't receive the response properly

### Remaining Issue - ROOT CAUSE FOUND

**The issue is VK_KHR_external_memory_fd requirement:**

Host virglrenderer logs show:
```
vkr_instance_enumerate_physical_devices: result=0 count=1  <-- SUCCESS on host!
vkr_dispatch_vkEnumeratePhysicalDevices: returning count=1
vkr: missing VK_EXT_external_memory_dma_buf for udmabuf import!
```

The host successfully enumerates 1 physical device (MoltenVK) but the guest Mesa Venus
driver **rejects it** because MoltenVK doesn't support the required extension.

**Why the guest rejects the device:**
From Mesa's `vn_physical_device.c`:
```c
if (!physical_dev->renderer_extensions.KHR_external_memory_fd) {
   vk_free(alloc, physical_dev->extension_spec_versions);
   return VK_ERROR_INCOMPATIBLE_DRIVER;  // manifests as INITIALIZATION_FAILED
}
```

**MoltenVK's supported external memory extensions:**
```
VK_KHR_external_memory_capabilities
VK_EXT_external_memory_host
VK_EXT_external_memory_metal     <-- Metal-specific, not fd-based
VK_KHR_external_memory
```

**Missing (required by Venus):**
- `VK_EXT_external_memory_dma_buf` - Linux-specific
- `VK_KHR_external_memory_fd` - POSIX fd-based (macOS doesn't use this)

### Environment
- QEMU: Custom build with HVF + virglrenderer
- virglrenderer: 1.2.0 with Venus + proxy mode
- MoltenVK: 1.4.0 (Homebrew)
- Guest: Alpine Linux 3.24.0_alpha (edge), kernel 6.12.1-3-virt
- Guest Mesa: 25.2.7 (mesa-vulkan-virtio)

### Debug Commands Used
```bash
# Enable QEMU traces
-trace "virtio_gpu*"

# Enable Venus debug (host)
export VKR_DEBUG=all
export MVK_CONFIG_LOG_LEVEL=2

# Enable Mesa debug (guest)
VN_DEBUG=all /tmp/test_vk
```

## Potential Solutions

### Option 1: Modify virglrenderer to use VK_EXT_external_memory_host
MoltenVK supports `VK_EXT_external_memory_host` which allows importing host memory
pointers into VkDeviceMemory. This could potentially replace fd-based sharing.

**Difficulty**: High - requires significant virglrenderer changes

### Option 2: Patch Mesa Venus driver
Remove or bypass the `VK_KHR_external_memory_fd` requirement for macOS hosts.

**Difficulty**: Medium - but may break memory sharing between guest and host

### Option 3: Use alternative GPU passthrough
The UTM project (QEMU for macOS) explored Venus + MoltenVK but abandoned it.
They now use Google's Android emulator graphics technology instead.
See: https://github.com/utmapp/UTM/issues/4551

**Difficulty**: Very High - different architecture entirely

### Option 4: Wait for upstream support
Mesa/virglrenderer may eventually add macOS support for Venus.

**Status**: Not planned - Venus is designed for Linux VMs

## Potential Fix: VK_EXT_external_memory_host Workaround

### Analysis (2026-01-20)

Deep analysis of virglrenderer code reveals a potential workaround using `VK_EXT_external_memory_host`:

**Key discoveries:**
1. MoltenVK supports `VK_EXT_external_memory_host` (verified via `vulkaninfo`)
2. Venus blob memory is already mmap'd on host side (`vkr_context.c:270`)
3. `vkr_resource` stores the host pointer in `res->u.data` for SHM type
4. Guest Venus driver only **checks** for `VK_KHR_external_memory_fd` but doesn't call fd APIs through protocol
5. The fd dispatch functions are NULL in virglrenderer (`vkr_physical_device.c:915-916`)

**Why this could work:**
- Venus protocol uses virtio-gpu blobs for memory sharing, not direct fd passing
- The host pointer is already available in blob memory
- `VK_EXT_external_memory_host` allows importing host pointers into Vulkan

### Proposed Virglrenderer Patch

**File: `src/venus/vkr_physical_device.c`**
```c
// In vkr_physical_device_init_extensions(), after line 283:
else if (!strcmp(props->extensionName, "VK_EXT_external_memory_host"))
   physical_dev->EXT_external_memory_host = true;

// After extension scanning, fake fd support if host pointer available:
#ifdef __APPLE__
if (!physical_dev->KHR_external_memory_fd && physical_dev->EXT_external_memory_host) {
   physical_dev->KHR_external_memory_fd = true;  // Fake it for guest
   physical_dev->use_host_pointer_import = true;  // New flag for internal use
}
#endif
```

**File: `src/venus/vkr_allocator.c`**
```c
// In vkr_allocator_init(), modify required_extensions for macOS:
#ifdef __APPLE__
static const char *required_extensions[] = {
   "VK_EXT_external_memory_host",  // Use host pointer on macOS
};
#else
static const char *required_extensions[] = {
   "VK_KHR_external_memory_fd",
};
#endif

// In vkr_allocator_allocate_memory(), use host pointer:
#ifdef __APPLE__
void *host_ptr = virgl_resource_get_map_ptr(res);  // Need to add this function
VkMemoryAllocateInfo alloc_info = {
   .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
   .pNext = &(VkImportMemoryHostPointerInfoEXT){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
      .pHostPointer = host_ptr
   },
   .allocationSize = res->vulkan_info.allocation_size,
   .memoryTypeIndex = res->vulkan_info.memory_type_index
};
#else
// Existing fd-based code
#endif
```

### Remaining Challenges

1. **Memory alignment**: `VK_EXT_external_memory_host` requires alignment to `minImportedHostPointerAlignment`
   - Need to query this and ensure blob memory meets requirement

2. **Memory type compatibility**: Host pointer import may not work with all memory types
   - Need to call `vkGetMemoryHostPointerPropertiesEXT` to check

3. **Extension advertisement**: Need to add `VK_KHR_external_memory_fd` to filtered extension list for guest

### SOLUTION WORKING! (2026-01-20)

**The patch works!** After implementing the changes:

```
$ vulkaninfo --summary
GPU0:
    apiVersion         = 1.2.0
    driverVersion      = 25.2.7
    vendorID           = 0x106b
    deviceID           = 0x1a020208
    deviceType         = PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
    deviceName         = Virtio-GPU Venus (Apple M2 Pro)
    driverID           = DRIVER_ID_MESA_VENUS
    driverName         = venus
    driverInfo         = Mesa 25.2.7
```

**Key findings:**
1. `minImportedHostPointerAlignment` = 16KB (same as macOS page size) ✅
2. Blob memory already aligned to page size ✅
3. Guest Venus driver only checks extension exists, doesn't use fd APIs ✅

### Files Changed (virglrenderer)

1. **`src/venus/vkr_physical_device.h`** - Added new fields:
   ```c
   bool EXT_external_memory_host;
   bool use_host_pointer_import;
   VkDeviceSize min_imported_host_pointer_alignment;
   ```

2. **`src/venus/vkr_physical_device.c`** - Two changes:
   - Detect `VK_EXT_external_memory_host` during extension scan
   - Add fallback: when no `VK_KHR_external_memory_fd` but has host pointer support,
     set `use_host_pointer_import=true` and fake fd support for guest

3. **`src/venus/vkr_device.c`** - Use `VK_EXT_external_memory_host` instead of
   `VK_KHR_external_memory_fd` when creating device on host pointer fallback path

### Test Results (2026-01-20)

| Test | Result |
|------|--------|
| Vulkan device enumeration | ✅ Works - "Virtio-GPU Venus (Apple M2 Pro)" |
| vkCreateInstance | ✅ Works |
| vkCreateDevice | ✅ Works |
| Memory allocation | ✅ Works - can allocate 1MB device memory |
| Buffer creation | ✅ Works - storage buffers created |
| Shader module creation | ✅ Works - SPIR-V processed correctly |
| Compute pipeline creation | ✅ Works |
| Compute shader execution | ✅ Works (after device memory fix) |
| OpenGL (virgl) | ❌ Falls back to llvmpipe (separate issue) |

### Device Memory Blob Export Fix (2026-01-20)

**Root cause of compute hang**: Device memory blob export failed because MoltenVK doesn't support
`VK_KHR_external_memory_fd` or `VK_EXT_external_memory_dma_buf` for exporting Vulkan device memory.

**Solution**: Added SHM-based fallback using `VK_EXT_external_memory_host`:
1. When allocating host-visible Vulkan memory and no fd export is available
2. Create a SHM file and mmap it
3. Use `VkImportMemoryHostPointerInfoEXT` to import the SHM pointer into Vulkan memory
4. Return the SHM fd when exporting the blob

**Files changed** (`src/venus/vkr_device_memory.c`):
- Added SHM-based host pointer import in `vkr_dispatch_vkAllocateMemory()`
- Added SHM export path in `vkr_device_memory_export_blob()`
- Added cleanup in `vkr_device_memory_release()`

**Files changed** (`src/venus/vkr_device_memory.h`):
- Added `shm_fd`, `shm_ptr`, `shm_size` fields to `vkr_device_memory` struct

**Verification**: Compute shader test now passes - creates buffer, allocates memory, records
command buffer with vkCmdFillBuffer, submits and verifies results.

### Remaining Work

1. [x] Basic Vulkan operations (instance, device, memory, buffers)
2. [x] Shader compilation and pipeline creation
3. [x] Fix blob resource operations for command submission (device memory export)
4. [x] Clean up debug prints
5. [ ] Handle fence/semaphore fd extensions similarly if needed
6. [ ] Recreate macOS socket compatibility layer for render server (SOCK_STREAM, signalfd)
7. [ ] Submit upstream patch

### Git Commits (virglrenderer)

1. `26e3a411` - feature(major): Add VK_EXT_external_memory_host fallback for macOS/MoltenVK
   - Files: vkr_physical_device.c/h, vkr_device.c
   - Extension detection and host pointer import setup

2. `9c656483` - fix(venus): Add SHM-based blob export for VK_EXT_external_memory_host
   - Files: vkr_device_memory.c/h
   - SHM allocation, mmap import, blob export

**Note**: macOS build also requires compatibility patches for render server (SOCK_CLOEXEC,
MSG_CMSG_CLOEXEC, sys/signalfd.h, clock_nanosleep). These patches were not committed and
need to be recreated if rebuilding. The working binary exists at `build/server/virgl_render_server`.

---

## Previous Conclusion (RESOLVED)

~~Venus on macOS via MoltenVK is fundamentally blocked~~ → **FIXED with virglrenderer patch**

The `VK_EXT_external_memory_host` approach works because:
- ✅ No guest-side changes needed (just extension advertisement)
- ✅ Host already has blob memory pointers
- ✅ MoltenVK supports the required extension
- ✅ Guest Mesa Venus driver only checks extension exists, doesn't call fd APIs
