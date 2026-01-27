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

## Recommended Path Forward

**Implement Option 1**: Patch virglrenderer to disable worker processes on macOS.

### Implementation Steps

1. Modify `server/render_worker.c` to detect macOS:
   ```c
   #ifdef __APPLE__
   // Run render context directly in main process
   render_context_main(context_data);
   #else
   // Fork worker as normal
   fork_worker(...);
   #endif
   ```

2. Test with same workload

3. If successful, submit patch upstream with explanation

### Alternative: In-process rendering
If worker patch is too complex, use virglrenderer as a library linked directly into QEMU (no separate render server).

## References

- virglrenderer worker code: `server/render_worker.c`
- MoltenVK shader compilation: Uses `MTLCompiler` XPC service
- Apple XPC documentation: XPC connections are process-local and don't survive fork
- virglrenderer build config: `build/config.h` (ENABLE_RENDER_SERVER_WORKER_PROCESS)
