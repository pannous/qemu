
## macOS Signed Binaries Build Fix (2026-01-27)

**Problem:** The signed QEMU binaries (e.g., `qemu-system-aarch64`) were not being built by default. Only the `-unsigned` versions were compiled during a standard `ninja` or `make` build.

**Root Cause:** On macOS, QEMU uses a two-stage build:
1. Compile the executable as `qemu-system-*-unsigned`
2. Run `scripts/entitlement.sh` via custom_target to create signed version with:
   - Code signing with HVF entitlements
   - QEMU icon embedded via Rez/SetFile

The custom_target lacked `build_by_default: true`, so it only built on explicit request.

**Solution:** Added `build_by_default: true` to the custom_target in meson.build:4428

**Files Changed:** meson.build:4425-4429

**Commit:** eaee949dbe
