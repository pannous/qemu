# Rust Vulkan Guest Demos

Rust implementations of the Vulkan guest demos for Venus/MoltenVK testing.

## Demos

- **triangle** - Simple RGB triangle on blue background
- **vkcube** - Animated spinning rainbow cube (10 seconds)

## Building

### On the Guest (Alpine VM)

```sh
cd /root/guest-demos-rs
./build.sh
```

This installs Rust, Vulkan dependencies, compiles shaders, and builds the demos.

### Cross-compiling on Host

For faster builds, cross-compile on macOS:

```sh
# Install cross-compilation target
rustup target add x86_64-unknown-linux-musl

# Build
cargo build --release --target x86_64-unknown-linux-musl

# Copy to guest
scp -P 2222 target/x86_64-unknown-linux-musl/release/test_tri root@localhost:/root/
scp -P 2222 target/x86_64-unknown-linux-musl/release/vkcube_anim root@localhost:/root/
```

## Running

Make sure shaders are in `/root/`:
```sh
# Triangle
./test_tri        # Shows RGB triangle for 5s

# Cube
./vkcube_anim     # Spins rainbow cube for 10s
```

## Architecture

```
guest-demos-rs/
├── common/           # Shared library
│   └── src/
│       ├── drm.rs    # DRM/KMS dumb buffer display
│       └── vulkan.rs # Vulkan context, render targets
├── triangle/         # Simple triangle demo
└── vkcube/           # Animated cube demo
```

Both demos:
1. Open DRM display via `/dev/dri/card0`
2. Create dumb buffer for framebuffer
3. Initialize Vulkan (no extensions needed)
4. Render to LINEAR image (CPU-visible)
5. Copy rendered pixels to DRM framebuffer
6. Set CRTC to display

## Dependencies

- `ash` - Vulkan bindings for Rust
- `libc` - Raw DRM ioctls (no drm-rs crate needed)

## Shaders

Uses the same GLSL shaders as the C demos:
- `tri.vert/frag` - Hardcoded triangle positions/colors
- `cube.vert/frag` - MVP transform with vertex colors
