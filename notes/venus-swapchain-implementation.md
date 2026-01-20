# Venus VK_KHR_swapchain Implementation for macOS

## Goal
Make vkcube work with Venus on macOS by exposing VK_KHR_swapchain to the guest.

## Current State
- Venus exposes Vulkan to guest via virtio-gpu
- Guest can render to HOST_VISIBLE memory (working)
- Guest can scanout via GBM → DRM → SET_SCANOUT_BLOB (working)
- Host presents via Vulkan swapchain → CAMetalLayer (working?? or is the following necessary: 
- **Missing**: VK_KHR_swapchain device extension not exposed to guest

## Problem
vkcube and most Vulkan apps use VK_KHR_swapchain directly:
```c
vkCreateSwapchainKHR()
vkAcquireNextImageKHR()
vkQueuePresentKHR()
```

Venus currently doesn't expose these because:
1. Venus is designed for headless/offscreen rendering
2. Swapchain requires WSI (Window System Integration) which is host-specific
3. No existing code path for swapchain in virglrenderer Venus backend

## Solution: Swapchain Proxy in virglrenderer

Important! We have full control over all components. 
If we encounter something blocking, we can change that and implement the necessary features. 

### Architecture
```
Guest                              Host (virglrenderer)
------                             -------------------
vkCreateSwapchainKHR()     →      Create IOSurface-backed images
vkGetSwapchainImagesKHR()  →      Return proxy VkImage handles
vkAcquireNextImageKHR()    →      Return available image index
vkQueuePresentKHR()        →      Copy to host swapchain, present
```

### Key Insight
Use the existing pattern from VK_EXT_external_memory_host fallback:
- Venus already "fakes" VK_KHR_external_memory_fd on macOS
- Uses host pointer import + SHM internally
- Advertises fd extension to guest for compatibility

Similarly for swapchain:
- Advertise VK_KHR_swapchain to guest
- Intercept swapchain commands in virglrenderer
- Map to host swapchain via IOSurface

### IOSurface Integration
On macOS, use IOSurface for zero-copy between guest and host:
```
Guest VkImage (swapchain)
    ↓ backed by
IOSurface (shared memory)
    ↓ imported as
Host MTLTexture / VkImage
    ↓ presented via
CAMetalLayer
```

## Implementation Plan

### Phase 1: Extension Exposure (virglrenderer)

**File: `src/venus/vkr_common.c`**
Add to extension table:
```c
.KHR_surface = true,
.KHR_swapchain = true,
```

**File: `src/venus/vkr_physical_device.c`**
- Expose VK_KHR_swapchain in device extensions
- Add surface capabilities query handlers

### Phase 2: Swapchain Object (virglrenderer)

**New file: `src/venus/vkr_swapchain.c`**
```c
struct vkr_swapchain {
    struct vkr_object base;
    VkSwapchainKHR handle;      // Host swapchain (MoltenVK)
    uint32_t image_count;
    VkImage *images;            // Host images
    IOSurfaceRef *surfaces;     // macOS: IOSurface backing
    uint32_t width, height;
    VkFormat format;
};
```

Dispatch handlers:
- `vkr_dispatch_vkCreateSwapchainKHR()`
- `vkr_dispatch_vkDestroySwapchainKHR()`
- `vkr_dispatch_vkGetSwapchainImagesKHR()`
- `vkr_dispatch_vkAcquireNextImageKHR()`
- `vkr_dispatch_vkQueuePresentKHR()`

### Phase 3: Surface Object (virglrenderer)

**New file: `src/venus/vkr_surface.c`**
```c
struct vkr_surface {
    struct vkr_object base;
    VkSurfaceKHR handle;        // Host surface (VK_EXT_metal_surface)
    CAMetalLayer *metal_layer;  // macOS display layer
};
```

Surface is created implicitly or via display extension.

### Phase 4: Image Backing with IOSurface

**File: `src/venus/vkr_swapchain.c`**
```c
// Create IOSurface for each swapchain image
IOSurfaceRef surface = IOSurfaceCreate(@{
    (id)kIOSurfaceWidth: @(width),
    (id)kIOSurfaceHeight: @(height),
    (id)kIOSurfaceBytesPerElement: @(4),
    (id)kIOSurfacePixelFormat: @(kCVPixelFormatType_32BGRA)
});

// Import into Vulkan via VK_EXT_metal_surface or memory import
```

### Phase 5: Present Path

On `vkQueuePresentKHR`:
1. Get IOSurface for presented image
2. Import IOSurface as MTLTexture
3. Blit to CAMetalLayer's nextDrawable
4. Present

## Files to Create/Modify

### virglrenderer
| File | Change |
|------|--------|
| `src/venus/vkr_swapchain.c` | **NEW** - Swapchain proxy |
| `src/venus/vkr_swapchain.h` | **NEW** - Header |
| `src/venus/vkr_surface.c` | **NEW** - Surface proxy |
| `src/venus/vkr_surface.h` | **NEW** - Header |
| `src/venus/vkr_common.c` | Add swapchain/surface to extension table |
| `src/venus/vkr_physical_device.c` | Expose extensions, surface caps |
| `src/venus/vkr_context.c` | Register dispatch handlers |
| `src/venus/meson.build` | Add new sources |

### QEMU (minimal changes)
| File | Change |
|------|--------|
| `hw/display/virtio-gpu-vk-swapchain.m` | Expose CAMetalLayer to virglrenderer |

## Alternative: Display Extension Path

Instead of full VK_KHR_swapchain, could use VK_KHR_display:
- Already partially exposed by Mesa
- Maps DRM displays to Vulkan
- But vkcube --wsi display says "Cannot find any display!"

Would need to implement display enumeration in Venus.

## Risks
1. **Protocol changes**: May need Venus protocol updates for new commands
2. **Synchronization**: Guest/host image sync complexity
3. **Performance**: IOSurface copy overhead (but I thought zero-copy?)

## References
- virglrenderer Venus: `/opt/other/virglrenderer/src/venus/`
- QEMU host swapchain: `hw/display/virtio-gpu-vk-swapchain.m`
- Existing macOS fixes: `notes/old/macos-venus-compatibility.md`

  Option A: Passthrough Swapchain (Complex)                                                                                
  - Add swapchain commands to Venus protocol                                                                               
  - Mesa Venus driver encodes swapchain commands                                                                           
  - virglrenderer handles them and creates host swapchain                                                                  
  - Requires Mesa changes (weeks of work)                                                                                  
                                                                                                                           
  Option B: WSI Shim Layer (Medium)                                                                                        
  - Create a Vulkan layer that intercepts swapchain calls                                                                  
  - Maps guest swapchain to DRM framebuffers                                                                               
  - Uses existing blob scanout path                                                                                        
  - Guest unaware, works with any Vulkan app   
