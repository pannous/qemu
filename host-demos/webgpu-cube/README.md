# WebGPU Rotating Gradient Cube Demo

Browser-based performance baseline using WebGPU API.

## Features
- Pure WebGPU rendering (no WebGL fallback)
- Same rotating gradient cube geometry as native demos
- Real-time FPS counter
- Runs directly in browser

## Requirements
- Modern browser with WebGPU support:
  - Chrome/Edge 113+
  - Safari 18+
  - Firefox Nightly (with flags enabled)

## Run
```bash
# Start local web server
python3 -m http.server 8000
# or
./serve.sh
```

Then open: http://localhost:8000

## Performance Comparison
This provides a WebGPU baseline to compare against:
- Native MoltenVK (~250-420 FPS)
- QEMU + Venus + virglrenderer
- Shows browser GPU acceleration capabilities

## Architecture
```
JavaScript/WebGPU API
    ↓
Browser WebGPU Implementation
    ↓
Metal (macOS) / Vulkan (Linux) / D3D12 (Windows)
    ↓
GPU Driver
```
