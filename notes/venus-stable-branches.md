# Venus Stable Branches - Working Configuration

## Overview

Created stable branches that exclude the breaking "zero-copy milestone" commits from Jan 23, 2026. These branches restore the last known working state where Venus rendering worked at **273 FPS**.

## Branch Information

### QEMU: venus-stable
- **Base commit**: `a485898e27` (Jan 22, 2026 12:26)
- **Title**: "chore: Update notes with working vkcube - 273 FPS!"
- **Cherry-picked commits**:
  - `fb425206c7` - docs: Root cause analysis
  - `0535925f6f` - docs: Regression investigation

**Location**: `/opt/other/qemu` (branch: `venus-stable`)

### virglrenderer: venus-stable
- **Base commit**: `3243a2f8` (Jan 22, 2026 11:38)
- **Title**: "fix: Filter faked DMA-BUF extensions for MoltenVK/macOS"
- **Cherry-picked commits**:
  - `ae022b68` - chore: Add build script and build directory documentation
  - `88ec9526` - build.sh
  - `be2c094f` - rm .cache

**Location**: `/opt/other/virglrenderer` (branch: `venus-stable`)

## What Was Excluded

### Breaking Commits (SKIPPED):

#### QEMU
- `37f2c7c205` - milestone zero-copy triangle (Jan 23 13:43)
  - Modified: hw/display/virtio-gpu-virgl.c (+322 lines)
  - Added broken zero-copy scaffolding

- `e3601ea0d0` - codex wip (Jan 22 23:37)
  - Immediately before zero-copy

- `ad7ec23a30` - vkcube double buffering (Jan 24)
  - Modified test program after zero-copy broke it

- `1352959d65` - Clean up Venus zero-copy scaffolding
  - Attempted cleanup of broken code

#### virglrenderer
- `f48b5b19` - milestone zero-copy triangle (Jan 23 13:45)
  - Modified: 17 files, 301 insertions
  - Broke Venus memory allocation and rendering

- `9b0a9ab2` - SHM fallback fix (Jan 25)
  - Attempted to fix zero-copy boot issues
  - Didn't fix rendering problems

- `0018e310` - v2 (after zero-copy)
- `19cf9e77` - codex wip (before zero-copy)

## How to Use

### 1. Switch to Stable Branches

```bash
# QEMU
cd /opt/other/qemu
git checkout venus-stable
cd build && ninja

# virglrenderer
cd /opt/other/virglrenderer
git checkout venus-stable
./build.sh --clean --venus --release
```

### 2. Run Alpine VM

```bash
cd /opt/other/qemu
./scripts/run-alpine.sh
```

### 3. Test Venus Rendering

```bash
ssh -p 2222 root@localhost
cd /root

# Test basic functionality
./test_minimal

# Test rendering (should work at ~273 FPS)
./vkcube_anim
```

## Expected Results

With the stable branches:
- ✅ `test_minimal` - Creates/destroys fences successfully
- ✅ `vkcube_anim` - Animated spinning cube at **273 FPS**
- ✅ `test_tri` - RGB triangle rendered correctly
- ✅ All Venus protocol communication works
- ✅ Vulkan → MoltenVK → Metal pipeline functional

## Switching Back to Master

If you need to return to master branch (with zero-copy broken state):

```bash
# QEMU
cd /opt/other/qemu
git checkout master

# virglrenderer
cd /opt/other/virglrenderer
git checkout main
```

## Next Steps

### For Development
1. Keep working on `venus-stable` branches for reliability
2. Create feature branches from stable for new work
3. Test thoroughly before merging to master

### For Zero-Copy (Future)
If you want to re-attempt zero-copy optimization:
1. Create a new branch from `venus-stable`
2. Implement zero-copy **incrementally** with tests at each step
3. Keep the working copy-based path as fallback
4. Don't break basic rendering while optimizing

## Performance Notes

The copy-based path (stable branches) achieves:
- **273 FPS** for vkcube (1280x800)
- All rendering done on host GPU (Apple M2 Pro)
- CPU copy overhead is minimal at this performance level
- Zero-copy is premature optimization - current performance is excellent

## Files Modified in Stable Branches

### QEMU venus-stable
- `notes/root-cause-found.md` (added)
- `notes/venus-regression-2026-01-27.md` (added)

### virglrenderer venus-stable
- `build.sh` (added)
- `notes/build-directories.md` (removed later)
- `.cache/` (removed - build artifacts)

## Verification

To verify you're on the correct commits:

```bash
# QEMU
cd /opt/other/qemu
git log --oneline -1
# Should show: 0535925f6f docs: Document Venus regression investigation

# virglrenderer
cd /opt/other/virglrenderer
git log --oneline -1
# Should show: be2c094f rm .cache
```

## Build Status

Both stable branches build successfully:
- ✅ QEMU: 1567 targets compiled
- ✅ virglrenderer: 99 targets compiled
- ✅ No compilation errors or warnings (except minor vkr_transport.c warnings)
