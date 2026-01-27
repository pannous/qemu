# MTLCompilerService Crash - 2026-01-27

## Problem

Pipeline creation fails with `VK_ERROR_INITIALIZATION_FAILED` not due to shader issues, but because the macOS Metal compiler service is unavailable.

## Root Cause

From MoltenVK debug logs:
```
[mvk-error] VK_ERROR_INITIALIZATION_FAILED: Shader library compile failed (Error code 3):
Unable to reach MTLCompilerService. The process is unavailable because the compiler is no longer active.
Latest invalidation reason: Connection init failed at lookup with error 3 - No such process.
```

The shader conversion from SPIR-V to MSL succeeds perfectly. The problem occurs when MoltenVK tries to compile the MSL into a Metal library - the MTLCompilerService is not responding.

## Analysis

1. **SPIR-V to MSL conversion**: ✅ SUCCESS
   - MoltenVK successfully converted the vertex shader
   - Generated MSL code looks correct (lines 415-573 in log)

2. **MSL compilation**: ❌ FAILURE
   - MTLCompilerService not reachable
   - Error 3: No such process

## Possible Causes

1. **Compiler service crashed** - The Metal compiler service may have crashed during previous operations

2. **Process isolation** - The `virgl_render_server` worker process may not have access to the compiler service
   - virglrenderer uses separate worker processes for security
   - These processes may be sandboxed or lack necessary entitlements

3. **Resource exhaustion** - Too many processes, out of file descriptors, or memory issues

4. **XPC connection failure** - The Metal compiler is accessed via XPC services on macOS, and the connection is failing

## Solutions to Try

### Solution 1: Restart Compiler Service
```bash
# Kill and restart Metal-related services
sudo killall -9 MTLCompilerService
# It should auto-restart on next use
```

### Solution 2: Run virgl_render_server with proper entitlements
The worker process may need entitlements to access Metal compiler services. Check if QEMU/virglrenderer needs code signing with specific entitlements.

### Solution 3: Use single-threaded mode (avoid worker process)
If the issue is with worker process isolation, try disabling the worker process mode:
```bash
# Build virglrenderer without worker process
meson configure build -Drender-server-worker=thread
# Or disable render server entirely for testing
```

### Solution 4: Check system resources
```bash
# Check system limits
ulimit -a
# Check if Metal compiler is running
ps aux | grep MTLCompiler
# Check for crash reports
ls -l ~/Library/Logs/DiagnosticReports/*MTL*
```

## Next Steps

1. Try restarting QEMU and checking if the issue persists
2. If it persists, check for MTLCompilerService crash logs
3. Consider running virglrenderer in single-process mode to avoid XPC issues
4. Check if virgl_render_server needs code signing with Metal entitlements
