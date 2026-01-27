# macOS Fork + XPC Problem - 2026-01-27

## Root Cause

The virglrenderer worker process architecture is fundamentally incompatible with macOS's XPC-based system services.

### The Problem Chain

1. **virglrenderer architecture**: Uses separate worker processes (via `fork()`) for security isolation
2. **MoltenVK requirement**: Needs to compile SPIR-V shaders to Metal at runtime
3. **Metal compiler**: Accessed via `MTLCompilerService` through XPC (inter-process communication)
4. **macOS XPC limitation**: **XPC connections do NOT survive fork()**

### Error Manifestation

```
[mvk-error] VK_ERROR_INITIALIZATION_FAILED: Shader library compile failed (Error code 3):
Unable to reach MTLCompilerService. The process is unavailable because the compiler is no longer active.
Latest invalidation reason: Connection init failed at lookup with error 3 - No such process.
```

### What Happens

1. Main `virgl_render_server` process starts ✅
2. QEMU requests a render context
3. virgl_render_server forks a worker process ✅
4. Worker process handles Vulkan commands ✅
5. Guest app creates graphics pipeline
6. Worker calls MoltenVK `vkCreateGraphicsPipelines`
7. MoltenVK converts SPIR-V to MSL successfully ✅
8. MoltenVK tries to compile MSL via `MTLCompilerService`
9. **XPC connection to MTLCompilerService fails** ❌ (lost during fork)
10. Pipeline creation returns `VK_ERROR_INITIALIZATION_FAILED`
11. Pipeline object not created, handle = NULL
12. Guest tries to bind NULL pipeline → lookup fails

## Failed Attempts

### 1. Thread-based workers
```bash
meson configure build -Drender-server-worker=thread
```
**Result**: Failed - macOS lacks C11 `<threads.h>`

### 2. Vulkan ICD preload
```bash
meson configure build -Dvulkan-preload=true
```
**Result**: No effect - XPC connections still lost on fork

### 3. Code signing with entitlements
**Status**: Not attempted yet, but unlikely to help (XPC connection loss is architectural)

## Possible Solutions

### Option 1: Disable worker process on macOS (RECOMMENDED)
Patch virglrenderer to run single-process on macOS:

```c
#ifdef __APPLE__
// Force single-process mode on macOS
#define ENABLE_RENDER_SERVER_WORKER_PROCESS 0
#endif
```

**Pros**:
- Simple patch
- Maintains functionality
- No security issues for local VM use

**Cons**:
- Loses process isolation
- All render contexts share address space

### Option 2: Pre-compile shader pipelines
Use MoltenVK's pipeline cache to pre-compile shaders before fork:

```c
// In parent process before fork:
vkCreatePipelineCache(...);  // Pre-warm all shaders
// Then fork workers
```

**Pros**:
- Keeps worker isolation
- May improve performance

**Cons**:
- Complex to implement
- Need to know all shaders ahead of time
- Still fails for dynamic shaders

### Option 3: Embedded Metal compiler
Bundle a Metal compiler that doesn't use XPC:

**Pros**:
- No XPC dependency

**Cons**:
- May violate Apple's licensing
- Extremely complex
- Possibly impossible

### Option 4: Run in QEMU process directly
Instead of separate render server, link virglrenderer into QEMU:

**Pros**:
- No fork, no XPC issues
- Simpler architecture

**Cons**:
- Major architectural change
- Loses process isolation entirely

## Solution ✅ RESOLVED

**Used thread-based workers with threads_compat.h** - This was already implemented on January 20, but accidentally reverted on January 27.

### What Happened

1. **Jan 20, 2026**: Commit `5c4f255a` added full macOS compatibility:
   - Created `threads_compat.h` (pthread wrapper for C11 threads API)
   - Modified `render_worker.c` to use threads on macOS
   - Implemented pipe-based SIGCHLD handling (macOS lacks signalfd)
   - **This was working perfectly!**

2. **Jan 27, 2026**: Commit `baf75ab` reverted a "codex wip" change
   - **Accidentally removed the `#ifdef __APPLE__` threads_compat.h include**
   - Broke thread-based worker support on macOS
   - Caused the XPC/fork issue to appear

3. **Jan 27, 2026 (later)**: Discovered the accidental revert via git archaeology
   - Restored the original working code from commit `5c4f255a`
   - Rebuilt with `-Drender-server-worker=thread`
   - **Everything works again!**

### Implementation (Restored from Jan 20 commit)

**File: `server/render_worker.c`**
```c
#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
#ifdef __APPLE__
#include "threads_compat.h"  // ← Key fix
#else
#include <threads.h>
#endif
#endif
```

**File: `server/threads_compat.h`** (pthread-based C11 threads shim)
```c
typedef pthread_t thrd_t;
typedef int (*thrd_start_t)(void *);
// ... implements thrd_create, thrd_join, etc using pthreads
```

**Build configuration:**
```bash
cd /opt/other/virglrenderer
meson configure build -Drender-server-worker=thread
meson compile -C build && meson install -C build
```

### Test Results

✅ **Pipeline creation**: SUCCESS (ret=0, valid handle `0x100f7f5b0`)
✅ **Triangle demo**: RENDERS CORRECTLY
✅ **Fence timeouts**: RESOLVED
✅ **Metal compiler access**: FUNCTIONAL (threads preserve XPC connections)

### Lesson Learned

The macOS compatibility was already solved! The XPC/fork issue only appeared after accidentally reverting the fix. Git history search (`git log -p -S"threads.h"`) was essential for discovering this.

## References

- virglrenderer worker code: `server/render_worker.c`
- MoltenVK shader compilation: Uses `MTLCompiler` XPC service
- Apple XPC documentation: XPC connections are process-local and don't survive fork
- virglrenderer build config: `build/config.h` (ENABLE_RENDER_SERVER_WORKER_PROCESS)
