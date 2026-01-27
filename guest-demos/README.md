# Guest Demos for QEMU Venus/VirtIO-GPU

Vulkan demos that run **inside the Alpine guest VM** to test the complete rendering pipeline:

```
Guest Vulkan → Mesa Venus → virtio-gpu → QEMU → MoltenVK → Metal → macOS Display
```

## Available Demos

### 1. Triangle (`triangle/`)
Basic Vulkan triangle demo - minimal test case for Venus driver.

**Build & Run:**
```bash
./install-to-guest.sh
ssh -p 2222 root@localhost
cd /root/triangle && ./test_tri
```

### 2. VKCube (`vkcube/`)
Animated rotating cube with textures - tests 3D rendering pipeline.

**Build & Run:**
```bash
./install-to-guest.sh
ssh -p 2222 root@localhost
cd /root/vkcube && ./vkcube_anim
```

### 3. ShaderToy Viewer (`shadertoy/`)
Real-time shader viewer for ShaderToy GLSL shaders - tests complex fragment shaders and uniforms.

**Build & Run:**
```bash
cd shadertoy
./install-to-guest.sh
ssh -p 2222 root@localhost
cd /root/shadertoy && ./shadertoy_viewer
```

**Features:**
- Import shaders from shadertoy.com
- Live animation with iTime, iResolution, iMouse
- Collection of example shaders (plasma, tunnel, gradients)
- Shader hot-swapping

See `shadertoy/README.md` for details.

## Architecture

### Guest Environment
- **OS**: Alpine Linux with 16KB page size kernel
- **Vulkan Driver**: Mesa Venus (virtio-gpu backend)
- **GPU**: virtio-gpu with blob resource support

### Rendering Pipeline
1. Guest app calls Vulkan API
2. Venus driver marshals commands
3. virtio-gpu transports to host QEMU
4. QEMU/MoltenVK executes on Metal
5. Results displayed via virtio-gpu scanout

### Zero-Copy Path
```
GBM blob ←import→ Vulkan image (same memory!) → render → scanout
```

The key is importing the GBM buffer's DMA-BUF fd into Vulkan via `VK_KHR_external_memory_fd`.

## Building Demos

Each demo has its own `build.sh` script that:
1. Installs Alpine packages (vulkan-tools, mesa-vulkan-virtio, etc.)
2. Compiles shaders (`.vert`/`.frag` → `.spv`)
3. Compiles C/C++ viewer code

### From Host (Recommended)
```bash
cd <demo-directory>
./install-to-guest.sh  # Copies files, SSHs in, builds automatically
```

### From Guest (Manual)
```bash
ssh -p 2222 root@localhost
cd /root/<demo-name>
./build.sh
./<demo-executable>
```

## Requirements

### Host
- macOS with Apple Silicon or Intel
- QEMU with HVF acceleration
- MoltenVK installed
- Alpine VM running with 16KB kernel

### Guest (Alpine VM)
Auto-installed by build scripts:
- `vulkan-tools`, `vulkan-loader`, `vulkan-headers`
- `mesa-vulkan-virtio` (Venus driver)
- `mesa-dev`, `mesa-gbm`
- `libdrm`, `libdrm-dev`
- `shaderc` (GLSL compiler)
- `build-base`, `g++`
- `glfw-dev` (for windowed demos)

## Comparison: Guest vs Host Demos

| Aspect | Guest Demos (here) | Host Demos |
|--------|-------------------|------------|
| **Location** | Inside Alpine VM | Native macOS |
| **Vulkan Driver** | Mesa Venus | MoltenVK direct |
| **Purpose** | Test full pipeline | Quick development |
| **Performance** | VM overhead | Native speed |
| **Use Case** | Integration testing | Shader prototyping |

Host demos: `../host-demos/`

## Troubleshooting

### "Failed to create Vulkan instance"
```bash
# Check Venus driver
ls /usr/lib/libvulkan_virtio.so

# Verify virtio-gpu device
lspci | grep -i virtio
```

### Black screen or crash
- Ensure QEMU uses HVF (not TCG)
- Verify 16KB kernel: `getconf PAGESIZE` → should be `16384`
- Check virtio-gpu blob support is enabled
- Check QEMU command includes `-device virtio-vga-gl`

### SSH connection failed
```bash
# Check QEMU is running with port forwarding
ps aux | grep qemu | grep 2222

# Or use debug script
cd /opt/other/qemu
./scripts/debug-venus.sh
```

## Development Workflow

1. **Prototype on host** (`host-demos/`) - faster iteration
2. **Test in guest** (`guest-demos/`) - verify full pipeline
3. **Deploy to Redox** - final target OS (future)

## Performance Notes

Guest demos have additional latency:
- Vulkan command marshaling (Venus)
- virtio-gpu queue processing
- VM virtualization overhead

Typical impact: 10-30% slower than native.

## References

- Venus driver: Mesa's virtio-gpu Vulkan implementation
- virtio-gpu: Paravirtualized GPU for VMs
- MoltenVK: Vulkan → Metal translation layer
- [QEMU Graphics](https://www.qemu.org/docs/master/system/devices/virtio-gpu.html)
- [Mesa Venus](https://docs.mesa3d.org/drivers/venus.html)
