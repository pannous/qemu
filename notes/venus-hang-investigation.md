# Venus Hang Investigation - 2026-01-19

## Summary
Venus/Vulkan on macOS via MoltenVK hangs during initialization. The HVF crash was fixed, but commands are not being processed.

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

### Root Cause Analysis
The Venus protocol flow should be:
1. ✅ Create DRM context with VENUS capset
2. ✅ Create blob for command buffer
3. ✅ Map blob into guest memory
4. ❌ Send `vkCreateRingMESA` via SUBMIT_3D to set up ring buffer
5. ❌ Ring thread processes commands from shared memory

Step 4 never happens - the guest driver doesn't send SUBMIT_3D.

### Potential Causes
1. **Mesa Venus driver issue on aarch64** - Might have ARM-specific bugs
2. **Proxy mode incompatibility** - Server runs separately, may need different handling
3. **Kernel virtio-gpu driver issue** - May not properly handle Venus command submission on this kernel version
4. **Memory mapping issue** - Guest can't write to blob properly

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

### Next Steps to Try
1. Try x86_64 guest instead of aarch64
2. Check if older Mesa version works
3. Try non-proxy mode (if possible on macOS)
4. Add more debug logging to guest kernel virtio-gpu driver
5. Check if issue exists on Linux host (non-macOS)
