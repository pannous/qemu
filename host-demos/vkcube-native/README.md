# Native macOS MoltenVK Cube Demo

Performance baseline for comparing QEMU+Venus+MoltenVK rendering.

## Features
- Native macOS Cocoa window
- Direct MoltenVK/Metal rendering
- Rotating gradient cube (same geometry as guest demo)
- FPS counter for performance measurement

## Requirements
- MoltenVK installed via Homebrew
- glslc shader compiler

## Build & Run
```bash
make run
```

## Performance Comparison
This demo provides the native performance baseline. Compare FPS against:
- QEMU + Venus + virglrenderer guest demos
- Direct guest rendering vs copyback approaches

## Architecture
```
Application (Objective-C/C)
    ↓
Vulkan API
    ↓
MoltenVK
    ↓
Metal
    ↓
macOS WindowServer
```
