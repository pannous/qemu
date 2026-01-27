# Venus Regression Investigation - 2026-01-27

## Problem
Venus rendering programs (vkcube_anim, test_tri_new) are hanging or crashing with "vn_ring_submit abort on fatal" errors, despite working previously (Jan 22-23 at 273 FPS).

## Actions Taken
1. ✅ Rebuilt virglrenderer cleanly (`./build.sh --clean --venus --release`)
2. ✅ Rebuilt QEMU (`./rebuild-qemu.sh`)
3. ✅ Restarted Alpine VM
4. ✅ Enabled Venus debug logging (`VKR_DEBUG=all`)

## Current Status

### What Works
- ✅ `test_minimal`: Creates/destroys fences successfully
- ✅ Vulkan instance/device creation
- ✅ Venus protocol communication (basic)

### What Fails
- ❌ `vkcube_anim`: Hangs/crashes with "vn_ring_submit abort on fatal"
- ❌ `test_tri_new`: Hangs indefinitely
- ❌ Rendering programs fail even with `VN_PERF=no_fence_feedback`

## Key Observations

### Fence Feedback Workaround Doesn't Help
Previous notes (venus-fence-debugging.md) documented that `VN_PERF=no_fence_feedback` fixed fence issues. This workaround no longer helps, suggesting the problem has evolved.

### Programs Hang at Different Points
- `test_minimal`: Completes successfully (no rendering)
- `test_tri_new`: Prints "fb_id=42 crtc_id=36" then hangs
- `vkcube_anim`: Prints title then hangs immediately

### Debug Output is Minimal
With `VKR_DEBUG=all` enabled, we see very little Venus activity in the logs when rendering programs run, suggesting they're hanging before reaching the Venus layer or the communication is broken.

## Possible Root Causes

### 1. Mesa Driver State Corruption
The guest Mesa Venus driver may have accumulated state issues from extensive testing. The driver might need to be rebuilt or the VM image replaced.

### 2. Ring Protocol Desynchronization
The Venus ring protocol between guest and host may have become desynchronized, causing all queue submissions to fail.

### 3. Host-Side Resource Exhaustion
MoltenVK or Metal resources on the host may be exhausted or in a bad state, requiring a full Mac reboot.

### 4. Recent Code Changes
While no system-wide code changes were made since Jan 23 (only shadertoy_drm.c was modified), something in the environment may have changed.

### 5. VM Image Corruption
The Alpine VM overlay image may have become corrupted, causing Mesa to malfunction.

## Next Steps

### Immediate Actions
1. **Reboot the Mac**: Clear any Metal/MoltenVK caches and reset GPU state
2. **Fresh VM image**: Try with a clean Alpine VM overlay
3. **Rebuild Mesa**: Rebuild the guest Mesa driver in case it's corrupted
4. **Check host resources**: Verify MoltenVK and Metal are functioning correctly

### Diagnostic Steps
1. **Test with vkcube --wsi display**: Try the native vkcube to see if the issue is in our code
2. **Run minimal C Venus test**: Create the smallest possible rendering program
3. **Check QEMU logs**: Look for any virtio-gpu errors
4. **Monitor Metal activity**: Use Xcode Instruments to see if Metal commands are reaching the GPU

### Alternative Approaches
1. **Try a different MoltenVK version**: Test with source-built /opt/other/MoltenVK/ instead of Homebrew
2. **Simplify the stack**: Remove any custom virglrenderer patches and test vanilla
3. **Compare with working state**: Git bisect to find when things broke (if committed)

## Environment
- macOS: Apple M2 Pro
- MoltenVK: 1.4.0 (Homebrew)
- QEMU: 10.2.50 (custom build with Venus support)
- virglrenderer: 1.2.0 (custom build, commit 68b2f216)
- Alpine Linux: aarch64 with 16KB kernel
- Mesa: Venus driver (version unknown - need to check)

## Related Notes
- venus-fence-debugging.md: Original fence feedback issue (resolved previously)
- venus-investigation-summary.md: Shows it was working at 273 FPS
- venus-macos-status.md: Documents the working configuration from Jan 22-23
