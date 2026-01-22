We do not do anything with OpenGL, but it is provided by a compatibility layer:

GUEST:

OpenGL (Mesa)
   ↓
Zink (Gallium)
   ↓
Vulkan
   ↓
MoltenVK
   ↓
Metal



Better GUEST:
   •  only Mesa user-space:
   •  libvulkan + venus_icd.json
   •  optionally libGL / EGL for virgl
   •  kernel: virtio_gpu + virtio_gpu_venus

HOST:

Mesa (Gallium / virgl / venus)
   ↓
Vulkan API
   ↓
MoltenVK
   ↓
Metal
   ↓
GPU