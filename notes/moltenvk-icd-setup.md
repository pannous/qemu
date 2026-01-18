# MoltenVK ICD Setup for Venus on macOS

## Problem
Venus backend requires Vulkan, which on macOS uses MoltenVK. The Vulkan loader needs to find the MoltenVK ICD (Installable Client Driver) manifest JSON file.

## Solution
Added `setup_moltenvk_icd()` in `hw/display/virtio-gpu-virgl.c` that:
1. Checks if `VK_ICD_FILENAMES` or `VK_DRIVER_FILES` already set (respects user config)
2. Searches common MoltenVK installation paths
3. Sets `VK_ICD_FILENAMES` if found
4. Reports error with install instructions if not found

## Searched Paths
- `/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json` (Homebrew arm64)
- `/usr/local/share/vulkan/icd.d/MoltenVK_icd.json` (Homebrew x86_64)
- `/opt/homebrew/opt/molten-vk/share/vulkan/icd.d/MoltenVK_icd.json`
- `/usr/local/opt/molten-vk/share/vulkan/icd.d/MoltenVK_icd.json`

## Installation
```bash
brew install molten-vk
```

## Architecture
```
QEMU virtio-gpu-gl (venus=true)
  └─ virtio_gpu_virgl_init()
       └─ setup_moltenvk_icd()  # macOS only
            └─ Sets VK_ICD_FILENAMES
       └─ virgl_renderer_init(VIRGL_RENDERER_VENUS)
            └─ virglrenderer initializes Vulkan
                 └─ Vulkan loader uses VK_ICD_FILENAMES
                      └─ MoltenVK provides Vulkan->Metal
```

## Testing
Requires full QEMU build with virglrenderer and Venus support.
