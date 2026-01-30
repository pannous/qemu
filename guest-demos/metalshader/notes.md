
## 2026-01-30: Alpine API Compatibility Breakthrough 🎉

Successfully updated metalshader to work with modern Rust crates on Alpine Linux!

**Problem Solved:** drm 0.12→0.14 and gbm 0.15→0.18 had breaking API changes
**Solution:** Implemented DrmCard wrapper with AsFd trait, updated get_connector() calls, switched to map_mut()
**Result:** Clean build on Alpine with Rust 1.93.0, binary 617.8K

**Key Learning:** Crates are source-universal but FFI bindings are platform-specific. Alpine has everything needed (libdrm 2.4.131, mesa 25.2.7), just needed code updates for new APIs.

**Next:** Test actual rendering on Alpine VM with Vulkan/Venus

## 2026-01-30: Keyboard Input Detection Fixed ✓

**Problem:** All input devices returned "Unknown", keyboard navigation didn't work
**Root Cause:** EVIOCGNAME ioctl number (0x4506) was incorrect for aarch64 architecture
**Solution:** Properly construct ioctl using _IOC macro with direction, type, nr, and size bits
**Result:** Now correctly detects "QEMU QEMU USB Keyboard" and "gpio-keys"

Correct ioctl value for aarch64: `(_IOC_READ << 30) | (0x45 << 8) | (0x06 << 0) | (256 << 16) = 0x81004506`

Keyboard navigation (arrow keys, ESC, F) should now work in metalshader.
