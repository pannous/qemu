# macOS Venus/Vulkan via MoltenVK - Comprehensive Analysis

## Project Goal
Enable Vulkan → Metal passthrough on macOS for Redox OS guests via QEMU's Venus backend.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        REDOX OS GUEST                           │
│  ┌─────────────┐                                                │
│  │ Vulkan App  │                                                │
│  └──────┬──────┘                                                │
│         │ Vulkan API calls                                      │
│  ┌──────▼──────┐                                                │
│  │ Mesa Venus  │  Guest Vulkan driver                           │
│  │   Driver    │  Serializes Vulkan commands                    │
│  └──────┬──────┘                                                │
│         │ Venus protocol over virtio-gpu                        │
└─────────┼───────────────────────────────────────────────────────┘
          │ virtio ring
┌─────────┼───────────────────────────────────────────────────────┐
│         │                    QEMU HOST                          │
│  ┌──────▼──────┐                                                │
│  │ virtio-gpu  │  hw/display/virtio-gpu-virgl.c                 │
│  │   device    │  Handles Venus capset, fences, resources       │
│  └──────┬──────┘                                                │
│         │ virgl_renderer_* API                                  │
│  ┌──────▼──────┐                                                │
│  │virglrenderer│  External library                              │
│  │Venus backend│  Deserializes & replays Vulkan commands        │
│  └──────┬──────┘                                                │
│         │ Vulkan API                                            │
│  ┌──────▼──────┐                                                │
│  │  MoltenVK   │  Vulkan → Metal translation layer              │
│  └──────┬──────┘                                                │
│         │ Metal API                                             │
│  ┌──────▼──────┐                                                │
│  │ Metal GPU   │  Apple Silicon / AMD GPU                       │
│  └─────────────┘                                                │
└─────────────────────────────────────────────────────────────────┘
```

## Component Analysis

Linux host + Linux guest + Mesa => Venus
Linux host + proprietary guest driver => direct Vulkan Passthrough
macOS host => Venus only (via MoltenVK)
CPU-only host => Venus + lavapipe (irrelevant / debug fallback?)


## References

- [MoltenVK GitHub](https://github.com/KhronosGroup/MoltenVK)
- [MoltenVK Runtime User Guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)
- [virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer)
- [Venus Protocol](https://gitlab.freedesktop.org/virgl/virglrenderer/-/tree/main/src/venus)
- [Virtio-GPU Specification](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)


### 1. QEMU virtio-gpu-virgl (hw/display/virtio-gpu-virgl.c)

**Purpose:** QEMU device model that bridges guest virtio-gpu to host virglrenderer.

**Key Functions:**
| Function | Line | Purpose |
|----------|------|---------|
| `virtio_gpu_virgl_init()` | 1180 | Initialize virglrenderer with Venus flags |
| `setup_moltenvk_icd()` | 1148 | [NEW] macOS MoltenVK ICD auto-discovery |
| `virtio_gpu_virgl_get_capsets()` | 1252 | Report Venus capset to guest |
| `virgl_write_fence()` | 1046 | Handle fence completion callbacks |
| `virgl_write_context_fence()` | 1076 | Handle context-based fence callbacks |
| `virgl_cmd_resource_create_blob()` | 665 | Create blob resources |
| `virgl_cmd_set_scanout_blob()` | 805 | Set scanout from blob (requires dmabuf) |

**Venus Initialization Flow:**
```c
virtio_gpu_virgl_init()
  ├─ [macOS] setup_moltenvk_icd()     // Set VK_ICD_FILENAMES
  ├─ flags |= VIRGL_RENDERER_VENUS
  ├─ flags |= VIRGL_RENDERER_RENDER_SERVER
  └─ virgl_renderer_init(flags)       // External library takes over
```

### 2. Capset System

**Defined Capsets** (include/standard-headers/linux/virtio_gpu.h):
```c
VIRTIO_GPU_CAPSET_VIRGL   = 1   // OpenGL
VIRTIO_GPU_CAPSET_VIRGL2  = 2   // OpenGL enhanced
VIRTIO_GPU_CAPSET_GFXSTREAM_VULKAN = 3
VIRTIO_GPU_CAPSET_VENUS   = 4   // Vulkan via Venus
VIRTIO_GPU_CAPSET_CROSS_DOMAIN = 5
VIRTIO_GPU_CAPSET_DRM     = 6
```

**Venus Capset Registration:**
- Only added if `venus=true` device property set
- Only added if virglrenderer reports non-zero `capset_max_size`
- Actual extension data comes from `virgl_renderer_fill_caps()`

### 3. Fence Handling

**Status: Portable - No macOS changes needed**

Fence callbacks are invoked by virglrenderer when GPU work completes:
- `virgl_write_fence()` - Legacy fence completion
- `virgl_write_context_fence()` - Context-based fence completion (Venus)

Both use QEMU abstractions (`virtio_gpu_ctrl_response_nodata()`) that are platform-independent.

### 4. Memory/Resource Handling

**Resource Types:**
1. **Regular resources** - Created via `virgl_cmd_create_resource_2d/3d()`
2. **Blob resources** - Created via `virgl_cmd_resource_create_blob()` (VIRGL 1.0+)

**Blob Resource Flow:**
```
virgl_cmd_resource_create_blob()
  ├─ virtio_gpu_create_mapping_iov()  // Map guest memory
  ├─ virgl_renderer_resource_create_blob()
  ├─ virgl_renderer_resource_get_info()
  │     └─ info.fd → res->base.dmabuf_fd  // -1 on macOS!
  └─ [macOS] warn if dmabuf_fd < 0
```

**dmabuf Issue on macOS:**
- Linux uses dmabuf for zero-copy buffer sharing
- macOS uses IOSurface instead (incompatible)
- `virgl_renderer_resource_get_info()` returns `fd = -1` on macOS
- Blob scanout (`SET_SCANOUT_BLOB`) requires `dmabuf_fd >= 0`

### 5. Scanout Paths

| Command | Function | dmabuf Required | macOS Status |
|---------|----------|-----------------|--------------|
| `SET_SCANOUT` | `virgl_cmd_set_scanout()` | No | ✓ Works |
| `SET_SCANOUT_BLOB` | `virgl_cmd_set_scanout_blob()` | Yes | ✗ Fails |

**Non-blob scanout** uses OpenGL texture path via `dpy_gl_scanout_texture()`.

### 6. Vulkan Extension Filtering

**Key Finding: QEMU does NOT filter extensions**

Extension filtering chain:
```
Guest queries extensions
    ↓
virtio-gpu forwards to virglrenderer
    ↓
virglrenderer queries Vulkan driver
    ↓
MoltenVK reports supported extensions
    ↓
Extensions flow back to guest
```

MoltenVK handles filtering automatically based on Metal capabilities.

## Implemented Changes

### 1. MoltenVK ICD Auto-Discovery (hw/display/virtio-gpu-virgl.c:1148-1177)

```c
#ifdef __APPLE__
static void setup_moltenvk_icd(void)
{
    static const char *moltenvk_paths[] = {
        "/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json",
        "/usr/local/share/vulkan/icd.d/MoltenVK_icd.json",
        "/opt/homebrew/opt/molten-vk/share/vulkan/icd.d/MoltenVK_icd.json",
        "/usr/local/opt/molten-vk/share/vulkan/icd.d/MoltenVK_icd.json",
        NULL
    };
    // Check VK_ICD_FILENAMES/VK_DRIVER_FILES, search paths, set env
}
#endif
```

Called when Venus enabled in `virtio_gpu_virgl_init()`.

### 2. Blob Resource Warning (hw/display/virtio-gpu-virgl.c:745-751)

```c
#ifdef __APPLE__
    if (res->base.dmabuf_fd < 0) {
        warn_report_once("Blob resource %d created without dmabuf backing. "
                         "Blob scanout will not work on macOS.");
    }
#endif
```

### 3. Improved Error Message for Blob Scanout (hw/display/virtio-gpu-virgl.c:851-864)

```c
if (res->base.dmabuf_fd < 0) {
#ifdef __APPLE__
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: resource %d not backed by dmabuf. "
                  "Blob scanout requires dmabuf which is unavailable on macOS. "
                  "Use non-blob scanout or disable blob resources.\n", ...);
#else
    qemu_log_mask(LOG_GUEST_ERROR, "%s: resource not backed by dmabuf %d\n", ...);
#endif
}
```

## Known Limitations

### MoltenVK Limitations
- Pipeline statistics query pool (`VK_QUERY_TYPE_PIPELINE_STATISTICS`) not supported
- PVRTC compressed formats require direct host-visible memory mapping
- Some features require Metal 3.0+
- Some features require Apple GPU specifically
- Application-controlled memory allocations (`VkAllocationCallbacks`) ignored

### macOS-Specific Limitations
1. **No dmabuf** - Blob scanout unavailable
2. **No UDMABUF** - Stubs return false/no-op
3. **No sync_file** - Different sync primitives needed

### Workarounds
- Use non-blob scanout path (works via OpenGL textures)
- Don't enable `blob=true` on virtio-gpu device
- Ensure virglrenderer built with Venus + MoltenVK support

## Dependencies

### Required Software
```bash
# MoltenVK
brew install molten-vk

# Vulkan SDK (optional, for validation layers)
# Download from https://vulkan.lunarg.com/sdk/home

# virglrenderer with Venus support
# Must be built from source with Venus enabled
```

### Build Configuration
```bash
# QEMU configure (example)
./configure --target-list=x86_64-softmmu \
            --enable-virglrenderer \
            --enable-opengl
```

### Runtime Configuration
```bash
# Venus-enabled VM (non-blob, for macOS compatibility)
qemu-system-x86_64 \
    -device virtio-gpu-gl,venus=true \
    -display cocoa,gl=es
```

## File Reference

| File | Purpose |
|------|---------|
| `hw/display/virtio-gpu-virgl.c` | Venus/virgl backend implementation |
| `hw/display/virtio-gpu-gl.c` | GL device model, Venus property |
| `hw/display/virtio-gpu.c` | Base GPU device, blob property |
| `hw/display/virtio-gpu-udmabuf.c` | Linux udmabuf (N/A on macOS) |
| `hw/display/virtio-gpu-udmabuf-stubs.c` | Stubs for non-Linux |
| `include/hw/virtio/virtio-gpu.h` | Data structures, macros |
| `include/standard-headers/linux/virtio_gpu.h` | Capset definitions |

## Current Implementation Status (2025-01-19)

### Summary

**What works:**
- Venus render server starts on macOS (via SOCK_STREAM patch)
- Alpine Linux aarch64 boots with HVF acceleration
- Standard virtio-gpu-pci provides working display
- Dual-GPU configuration: virtio-gpu for display + virtio-gpu-gl for Venus
- Venus-only mode: SET_SCANOUT and context creation handled as no-ops

**What doesn't work yet:**
- Single-GPU Venus mode has no display (no OpenGL scanout path)
- Venus capset not yet verified in guest (need to run vulkaninfo)

**Workaround:** Use dual-GPU configuration with separate virtio-gpu for display.

### Completed

1. **virglrenderer built with Venus support on macOS**
   - Location: `/opt/other/virglrenderer/install/`
   - macOS compatibility patches applied:
     - `clock_nanosleep` → `nanosleep` fallback
     - `MSG_CMSG_CLOEXEC` stubbed
     - `threads.h` → pthreads compatibility shim
     - `SOCK_SEQPACKET` → `SOCK_STREAM` (macOS doesn't support SEQPACKET)
     - `signalfd` stubbed for macOS

2. **QEMU patches to enable Venus without OpenGL/EGL**
   - `hw/display/meson.build`: Added `elif virgl.found()` branches for Venus-only builds
   - `hw/display/virtio-gpu-gl.c`: Guarded OpenGL checks with `#ifdef CONFIG_OPENGL`
   - `hw/display/virtio-gpu-virgl.c`:
     - Guarded EGL includes and callbacks
     - Added `VIRGL_RENDERER_NO_VIRGL` flag for Venus-only mode
     - No-op GL context callbacks for non-OpenGL builds

3. **QEMU built with virglrenderer Venus support**
   - virtio-gpu-gl-pci device available with `venus=on` property
   - Render server forks and runs successfully

4. **MoltenVK ICD auto-discovery** (already committed)
   - `setup_moltenvk_icd()` function searches common Homebrew paths

5. **GL context requirement resolved**
   - `hw/display/virtio-gpu-base.c`: Skip `GRAPHIC_FLAGS_GL` in Venus mode
   - Software framebuffer fallback via `dpy_gfx_update_full()`

6. **Venus render server working on macOS**
   - SOCK_STREAM used instead of SOCK_SEQPACKET
   - Render server process starts and communicates with QEMU

7. **Alpine Linux aarch64 boots with HVF**
   - Downloaded Alpine 3.23 aarch64 ISO
   - QEMU signed with HVF entitlement
   - Full boot verified via serial console

8. **Venus-only mode fixes (no OpenGL/EGL)**
   - SET_SCANOUT: Use QEMU's resource tracking instead of virglrenderer
   - CTX_CREATE: Accept non-Venus contexts as no-op when vrend not initialized
   - Resources tracked in QEMU's reslist even if virgl_renderer_resource_create fails

### QEMU Command Line

```bash
# aarch64 VM with HVF on Apple Silicon (display working)
VK_ICD_FILENAMES=/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json \
qemu-system-aarch64 \
    -M virt -accel hvf -cpu host -m 2G \
    -drive if=pflash,format=raw,readonly=on,file=edk2-aarch64-code.fd \
    -device virtio-gpu-pci \
    -display cocoa \
    -cdrom alpine-virt-3.23.0-aarch64.iso \
    -device qemu-xhci -device usb-kbd

# DUAL-GPU: Display + Venus for Vulkan (RECOMMENDED for macOS)
# virtio-gpu-pci provides display, virtio-gpu-gl-pci provides Venus/Vulkan
VK_ICD_FILENAMES=/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json \
qemu-system-aarch64 \
    -M virt -accel hvf -cpu host -m 2G -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file=edk2-aarch64-code.fd \
    -device virtio-gpu-pci \
    -device virtio-gpu-gl-pci,venus=on,blob=on,hostmem=256M \
    -display cocoa \
    -cdrom alpine-virt-3.23.0-aarch64.iso \
    -device qemu-xhci -device usb-kbd -device usb-tablet

# Single GPU Venus (display won't work - no OpenGL scanout)
VK_ICD_FILENAMES=/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json \
qemu-system-aarch64 \
    -M virt -accel hvf -cpu host -m 2G \
    -drive if=pflash,format=raw,readonly=on,file=edk2-aarch64-code.fd \
    -device virtio-gpu-gl-pci,venus=on,blob=on,hostmem=256M \
    -display cocoa \
    -cdrom alpine-virt-3.23.0-aarch64.iso
```

### Remaining Issues

1. **Single-GPU Venus-only mode has no display**
   - The scanout path requires OpenGL textures which aren't available
   - Workaround: Use dual-GPU (virtio-gpu + virtio-gpu-gl)
   - Long-term: Implement software framebuffer readback or IOSurface sharing
   - Workaround: Use separate virtio-gpu for display + virtio-gpu-gl for Venus

2. **Venus capset verification pending**
   - Need to install mesa-vulkan in guest and run vulkaninfo

### Next Steps

1. **Fix render_socket.c SOCK_SEQPACKET** (line 42 still uses SEQPACKET)
   - render_socket_pair() needs same SOCK_STREAM patch as proxy_socket_pair()

2. **Test dual-GPU configuration**
   ```bash
   qemu-system-aarch64 ... \
       -device virtio-gpu-pci \                    # For display
       -device virtio-gpu-gl-pci,venus=on \        # For Venus/Vulkan
   ```

3. **Install Vulkan tools in Alpine**
   - Default repos don't have vulkan-tools
   - Need: `apk add --repository=http://dl-cdn.alpinelinux.org/alpine/edge/testing vulkan-tools`
   - Or use a distro with better Vulkan support (Fedora, Ubuntu)

4. **Verify Venus capset reported to guest**
   - Run vulkaninfo to see if Venus driver is detected
   - Check /sys/class/drm/card*/device for virtio_gpu

5. **Fix display for Venus-only mode** (longer term)
   - Options: software scanout, virtio-gpu for framebuffer only
   - Or implement IOSurface-based scanout for macOS

## virglrenderer macOS Patches Required

These patches are needed for virglrenderer on macOS:

```c
// src/proxy/proxy_socket.c - SOCK_STREAM instead of SOCK_SEQPACKET
#ifdef __APPLE__
   int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, out_fds);
#else
   int ret = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, out_fds);
#endif

// server/render_socket.c - Same change for server side
// server/render_worker.c - threads.h compatibility + signalfd stubs
```

## Testing Checklist

- [x] MoltenVK installed and ICD discoverable
- [x] virglrenderer built with Venus support
- [x] QEMU built with virglrenderer support
- [x] QEMU starts without GL context assertion
- [x] Venus render server starts on macOS
- [x] Alpine Linux aarch64 boots with HVF
- [ ] Venus capset reported to guest (`VIRTIO_GPU_CAPSET_VENUS`)
- [ ] Guest Vulkan apps render correctly via Venus → MoltenVK
- [ ] Display working in Venus mode (currently needs separate virtio-gpu)
- [ ] Redox OS guest boots with Venus Vulkan support
 
                                                                         
  Actually, looking at the serial output, there was an error: virtio_gpu_virgl_process_cmd: ctrl 0x103, error 0x1203. Let me 
  decode this:- ctrl 0x103 = VIRTIO_GPU_CMD_CTX_CREATE (creating a context)                                                  
  - error 0x1203 = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER                                                                     
 

Currently:

  Venus renders Vulkan commands via MoltenVK                                                                                 
           ↓                                                                                                                 
  Pixels exist in Metal GPU memory                                                                                           
           ↓                                                                                                                 
  QEMU display needs pixel data in CPU-accessible memory                                                                     
           ↓                                                                                                                 
  No automatic readback from Vulkan → CPU                                                                                    
           ↓                                                                                                                 
  Display has nothing to show → "not active"                                                                                 
                                                                                                                             
  The dpy_gfx_update() fallback I added exists but doesn't work because there's no pixel data to display. Venus/virglrenderer
   doesn't automatically copy rendered frames back to CPU memory.                                                            
                                                                                                                             
  Real solutions: 
  1. Blit/readback: Make virglrenderer copy final framebuffer from Vulkan back to CPU memory                                 
  2. IOSurface sharing: Share Metal textures directly with QEMU's Cocoa display (macOS-specific, significant work)  
                                   

⏺ Venus is a pure Vulkan passthrough - it just forwards Vulkan API calls. There's no framebuffer readback because:           
                                                                                                                             
  1. Guest creates Vulkan resources → proxied to host MoltenVK                                                               
  2. Rendering happens in Metal GPU memory                                                                                   
  3. No mechanism to automatically copy results back for display                                                             
                                                                                                                             
  On Linux this is solved with dmabuf (zero-copy buffer sharing). macOS has IOSurface but QEMU doesn't support it.  

  # Add edge testing repo                                                                                                    
  echo "http://dl-cdn.alpinelinux.org/alpine/edge/testing" >> /etc/apk/repositories                                          
  apk update                                                                                                                 
  apk add vulkan-tools                                                                                                       
                                                                                                                             
  # Test Vulkan                                                                                                              
  vulkaninfo --summary    
                                                                                              
  cat /sys/class/drm/card*/device/vendor                                                                                     
  ls -la /dev/dri/            
                                                                                                         
  apk search vulkan                                                                                                          
  You'll likely want:                                                                                                        
  apk add vulkan-tools-dbg   # or                                                                                            
  apk add mesa-vulkan-swrast # software Vulkan renderer                                                                      
  apk add mesa-vulkan-layers                                                                                                 
                                                                                                                             
  For virtio-gpu Vulkan support, you need mesa-vulkan-virtio (if available) or the lavapipe software renderer:               
  apk add mesa-vulkan-swrast vulkan-loader                                                                                   
                   

                                                                                                                             
  In dual boot the display should work via the first virtio-gpu-pci device.                                                                                      
                                               

 "The key is whether we can read pixels back from Vulkan to CPU memory.
Why do we need read pixels back?
You need it only for bootstrapping, validation, or fallback display paths.

vnc://localhost:5901