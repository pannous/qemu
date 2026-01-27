# Upstream Merge Summary - 2026-01-27

## QEMU venus-stable

✅ **Merged 173 commits** from upstream/master

**Merge commit**: eed6e60802
**Date range**: 5 days of upstream changes (Jan 22 → Jan 27)
**Files changed**: 302 files (+5998, -2911)
**Status**: Success, no conflicts

### Key upstream changes included:
- VFIO updates and fixes
- Linux user improvements (termios2, futex, etc.)
- Migration improvements
- Architecture fixes (ARM, x86, RISC-V, etc.)
- Documentation updates
- Various bug fixes and cleanups

### What was NOT merged:
- ❌ Origin/main (contains broken zero-copy code)
- ❌ Breaking commits from Jan 23 (37f2c7c205, etc.)

The venus-stable branch now has:
- All upstream QEMU improvements through Jan 27
- Working Venus rendering (273 FPS)
- None of the broken zero-copy code

---

## virglrenderer venus-stable

⏭️ **No merge needed** - already ahead of upstream!

**Status**: venus-stable is 16 commits AHEAD of upstream/main
**Upstream latest**: Jan 1, 2026
**venus-stable latest**: Jan 26, 2026

### Why ahead?
The macOS Venus work (Jan 20-26) hasn't been upstreamed yet:
- VK_EXT_external_memory_host support for macOS
- SHM-based blob export
- SOCK_STREAM message framing for macOS
- MoltenVK portability extensions
- DRM format modifier extensions
- Build scripts and documentation

### What was NOT merged:
- ❌ Origin/main (contains broken zero-copy code from f48b5b19)

The venus-stable branch has all the working macOS Venus code without the breaking zero-copy changes.

---

## Summary

✅ **QEMU**: Merged 173 upstream commits, no conflicts
✅ **virglrenderer**: Already ahead of upstream, no merge needed
✅ **Both branches**: Clean, working, and protected from broken code

Both venus-stable branches are now:
1. Up-to-date with official upstream repositories
2. Protected from broken zero-copy code
3. Ready for development
4. Maintaining 273 FPS performance
