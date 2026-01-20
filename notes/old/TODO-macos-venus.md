# TODO: macOS Venus/Vulkan Support

## Critical Blocker

**Homebrew QEMU lacks virglrenderer/Venus support.**

The stock `brew install qemu` does NOT include:
- `virtio-vga-gl` device
- `virtio-gpu-gl` device
- Venus protocol support

**Solution:** Build QEMU from source with virglrenderer.

---

## Completed ✓

- [x] **MoltenVK ICD Auto-Discovery**
  - Added `setup_moltenvk_icd()` for automatic ICD path detection
  - Searches Homebrew paths, respects user environment variables
  - Commit: `21a89d90a3`

- [x] **Fence/Memory Edge Case Analysis**
  - Confirmed fences are portable (callback-based)
  - Identified dmabuf as main blocker for blob scanout
  - Added macOS-specific warnings and error messages
  - Commit: `9476799f2d`

- [x] **Vulkan Extension Documentation**
  - Confirmed extensions filtered by MoltenVK, not QEMU
  - Documented known MoltenVK limitations
  - Commit: `a61139578c`

---

## Next Steps - High Priority

### 1. Build & Test Infrastructure
- [ ] **Set up QEMU build with Venus support on macOS**
  - Configure with `--enable-virglrenderer --enable-opengl`
  - Verify virglrenderer has Venus support enabled
  - Test basic compilation of modified virtio-gpu-virgl.c

- [ ] **Install and verify MoltenVK**
  ```bash
  brew install molten-vk
  # Verify ICD is discoverable
  ls /opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json
  ```

- [ ] **Build virglrenderer with Venus + macOS support**
  - May need patches for macOS compatibility
  - Verify `virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VENUS)` returns valid size

### 2. Basic Functionality Testing
- [ ] **Test Venus capset advertisement**
  - Verify guest sees `VIRTIO_GPU_CAPSET_VENUS` in capset list
  - Check `virgl_renderer_fill_caps()` returns valid data

- [ ] **Test non-blob scanout path**
  - Use `VIRTIO_GPU_CMD_SET_SCANOUT` (not blob)
  - Verify OpenGL texture display works via `dpy_gl_scanout_texture()`

- [ ] **Test basic Vulkan rendering**
  - Simple Vulkan triangle app in guest
  - Verify fence callbacks fire correctly
  - Check for any synchronization issues

### 3. Redox OS Integration
- [ ] **Test Redox OS boot with Venus**
  - Verify Redox kernel loads virtio-gpu driver
  - Check Venus capset negotiation
  - Test basic display output

- [ ] **Test Redox Vulkan support**
  - Verify Mesa Venus driver works in Redox
  - Run simple Vulkan test applications
  - Profile performance vs. software rendering

---

## Next Steps - Medium Priority

### 4. Performance Optimization
- [ ] **Profile Venus command submission overhead**
  - Measure latency of Vulkan command round-trips
  - Identify bottlenecks in serialization/deserialization

- [ ] **Test with MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS**
  - May improve or degrade performance depending on workload
  - Document optimal MoltenVK configuration

- [ ] **Investigate async command submission**
  - Check if virglrenderer supports async Venus mode
  - Could reduce latency for GPU-bound workloads

### 5. Advanced Features
- [ ] **Investigate IOSurface for blob resources**
  - Research virglrenderer IOSurface support
  - Could enable blob scanout on macOS
  - Significant architectural work required

- [ ] **External memory support**
  - `VK_KHR_external_memory` for cross-process sharing
  - May need MoltenVK + virglrenderer coordination

- [ ] **Timeline semaphores**
  - `VK_KHR_timeline_semaphore` for advanced sync
  - Check MoltenVK support status

---

## Next Steps - Low Priority / Future

### 6. Documentation & Upstream
- [ ] **Document macOS Venus setup guide**
  - Step-by-step instructions for end users
  - Troubleshooting common issues

- [ ] **Consider upstream contribution**
  - MoltenVK ICD setup could benefit other macOS users
  - Coordinate with QEMU maintainers on approach

- [ ] **Add macOS CI testing**
  - Automated build verification on macOS
  - Basic functional tests

### 7. Alternative Approaches
- [ ] **Evaluate gfxstream as alternative**
  - `virtio-gpu-rutabaga` device with gfxstream-vulkan
  - May have different macOS compatibility characteristics

- [ ] **Research Apple Paravirtualized Graphics**
  - `apple-gfx` device uses Metal directly
  - Different architecture, macOS-to-macOS only
  - Could inform future Venus improvements

---

## Known Blockers

| Issue | Impact | Workaround |
|-------|--------|------------|
| No dmabuf on macOS | Blob scanout fails | Use non-blob scanout |
| virglrenderer macOS support | May need patches | Build from source with fixes |
| MoltenVK pipeline statistics | Some Vulkan features unavailable | Application must handle |

---

## Testing Matrix

| Component | Status | Notes |
|-----------|--------|-------|
| MoltenVK ICD discovery | Implemented | Needs runtime test |
| Venus initialization | Implemented | Needs build test |
| Fence callbacks | Should work | Needs runtime test |
| Non-blob scanout | Should work | Needs runtime test |
| Blob scanout | Will fail | Expected, documented |
| Extension filtering | MoltenVK handles | Needs verification |
| Redox guest | Unknown | Primary test target |

---

---

## Final Validation: vkcube Demo

### Test Environment Ready
- **Alpine Linux ISO**: `../redox/venus-test/alpine-virt-x86_64.iso` (63MB)
- **Test scripts**: `../redox/scripts/venus-*.sh`

### vkcube Test Procedure

1. **Build QEMU with Venus** (see above)

2. **Boot Alpine Linux**
   ```bash
   ./qemu-system-x86_64 \
       -M q35 -cpu max -smp 2 -m 2G \
       -device virtio-vga-gl,hostmem=1G,blob=true,venus=true \
       -vga none -display cocoa,gl=es \
       -object memory-backend-memfd,id=mem1,size=2G \
       -machine memory-backend=mem1 \
       -cdrom ../redox/venus-test/alpine-virt-x86_64.iso \
       -boot d -usb -device usb-tablet \
       -net nic,model=virtio -net user
   ```

3. **Inside Alpine** (login as root, no password):
   ```bash
   apk update
   apk add mesa-vulkan-virtio vulkan-tools mesa-dri-gallium
   vulkaninfo --summary
   vkcube
   ```

4. **Expected Output**:
   ```
   GPU id : 0 (Virtio-GPU Venus (Intel/AMD/Apple via MoltenVK))
   ```

### Success Criteria
- [ ] `vulkaninfo` shows "Virtio-GPU Venus"
- [ ] `vkcube` renders spinning cube
- [ ] No GPU hangs or crashes
- [ ] Fence synchronization works (smooth animation)

---

## Commands Reference

```bash
# Build QEMU (example)
mkdir build && cd build
../configure --target-list=x86_64-softmmu \
             --enable-virglrenderer \
             --enable-opengl \
             --enable-cocoa
make -j$(sysctl -n hw.ncpu)

# Run VM with Venus (non-blob for macOS)
./qemu-system-x86_64 \
    -machine q35 \
    -cpu host \
    -accel hvf \
    -m 4G \
    -device virtio-gpu-gl,venus=true \
    -display cocoa,gl=es \
    -drive file=redox.img,format=raw

# Check MoltenVK
vulkaninfo | grep -i moltenvk

# Debug Venus
VIRGL_DEBUG=all ./qemu-system-x86_64 ...
```
