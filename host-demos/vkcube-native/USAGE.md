# Usage Guide - Native MoltenVK Cube Demo

## Quick Start

```bash
cd /opt/other/qemu/host-demos/vkcube-native
make run
```

This will:
1. Compile GLSL shaders to SPIR-V
2. Build the native macOS application
3. Launch the rotating cube window

## Performance Testing

To measure baseline performance:

```bash
./vkcube_native 2>&1 | grep FPS
```

The FPS output appears every second showing current frame rate.

## Expected Output

```
Native MoltenVK initialized successfully!
FPS: 60
FPS: 60
FPS: 60
...
```

## Comparing with Guest

1. **Run this native demo** - Note the FPS
2. **Run guest demo** in Alpine VM:
   ```bash
   ./scripts/run-alpine.sh
   # In VM:
   cd /root/vkcube
   ./vkcube_anim
   ```
3. **Compare FPS** - The difference shows virtualization overhead

## Troubleshooting

### "VK err -9" (VK_ERROR_INCOMPATIBLE_DRIVER)
- MoltenVK ICD not found
- Solution: Ensure MoltenVK is installed via Homebrew

### Duplicate MoltenVK warning
- Two versions of MoltenVK are loaded
- This is a warning but shouldn't affect functionality
- Clean up `/usr/local/lib/libMoltenVK.dylib` if desired

### Window doesn't appear
- Check that you have GUI access (not in SSH session)
- Try running from Terminal.app directly

## Architecture

```
vkcube_native (Objective-C/C)
    ↓
Vulkan API (1.1)
    ↓
MoltenVK (ICD)
    ↓
Metal
    ↓
macOS WindowServer
    ↓
Display
```

## Performance Baseline

Typical performance on Apple Silicon:
- **M1/M2**: 60 FPS (VSync limited)
- **M1 Pro/Max**: 60 FPS (VSync limited)

With VSync disabled, expect 200-500+ FPS depending on hardware.

The guest VM will typically achieve 30-50% of this baseline due to:
- Vulkan command translation (Venus)
- virtio-gpu transport overhead
- Copy operations between guest/host
