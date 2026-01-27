# Alpine Boot Files Missing - Fixed

## Issue
QEMU failed to start with error:
```
qemu-system-aarch64: could not load initrd '/opt/other/qemu/alpine-boot/boot/initramfs-virt'
```

## Root Cause
During refactoring of `scripts/run-alpine.sh`, the `ISO` variable was accidentally removed while changing paths from `/tmp` to `${QEMU_DIR}`. The script still referenced `$ISO` at line 116 to extract kernel/initrd:

```bash
(cd ${QEMU_DIR}/alpine-boot && bsdtar -xf "$ISO" boot 2>/dev/null)
```

Since `$ISO` was undefined, the extraction silently failed (`2>/dev/null`), and the `alpine-boot/boot/` directory was never populated.

## Resolution
1. Downloaded Alpine ISO: `alpine-virt-3.21.0-aarch64.iso`
2. Extracted kernel and initrd to `alpine-boot/boot/`:
   - `vmlinuz-virt` (9.1M)
   - `initramfs-virt` (9.0M)
3. Re-added `ISO` variable to `scripts/run-alpine.sh`
4. Added ISO and boot files to `.gitignore`
5. Fixed `RENDER_SERVER_EXEC_PATH` from `builddir` to `build`

## Files Created
- `/opt/other/qemu/alpine-virt-3.21.0-aarch64.iso` (75M)
- `/opt/other/qemu/alpine-boot/boot/vmlinuz-virt` (9.1M)
- `/opt/other/qemu/alpine-boot/boot/initramfs-virt` (9.0M)

## Commit
`95fdc7e970` - fix(minor): Add missing ISO variable and fix render_server path

## Prevention
The ISO and boot files are now in `.gitignore` to prevent accidental tracking of large binaries. The `ISO` variable is properly defined for future use.
