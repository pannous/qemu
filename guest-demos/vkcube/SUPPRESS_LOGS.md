# Suppressing Virglrenderer Debug Logs

The swapchain debug logs are coming from virglrenderer/QEMU, not from the vkcube program.

## Quick Fix: Redirect to /dev/null

When running vkcube in the guest, redirect stderr:

```bash
./vkcube_anim 2>/dev/null
```

## Better Fix: Disable Debug Logs in QEMU

The logs come from debug code in virglrenderer. To disable them:

### Option 1: Environment Variable (Guest)
```bash
export VIRGL_DEBUG=0
./vkcube_anim
```

### Option 2: Rebuild virglrenderer without debug (Host)
In `/opt/other/virglrenderer`, edit the code to remove the swapchain debug printf statements.

### Option 3: QEMU Launch Flag (Host)
When launching QEMU, redirect virgl logs:
```bash
qemu-system-aarch64 ... 2>&1 | grep -v "swapchain debug" | grep -v "virgl_render_server"
```

Or simply:
```bash
qemu-system-aarch64 ... 2>/dev/null
```

## Recommended: Guest-side Redirection

The easiest solution is to run in the guest with stderr redirected:
```bash
./vkcube_anim 2>/dev/null
```

This keeps QEMU logs visible on the host while silencing the virglrenderer spam.
