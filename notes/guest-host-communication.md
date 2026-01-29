# Guest-to-Host Display Control

## Overview
Enables guest applications to control QEMU host window (e.g., trigger fullscreen) via virtio-serial communication channel.

## Implementation (2026-01-29)

### Architecture
```
Guest App (metalshader) 
  → /dev/vport0p1 (virtio-serial port)
    → Unix socket /tmp/qemu-display-ctl.sock
      → QEMU Cocoa UI (GCD timer polling)
        → Host window control (toggleFullScreen)
```

### Components

**1. Guest Side** (guest-demos/metalshader/metalshader.c:184)
```c
case KEY_F:
    FILE *f = fopen("/dev/vport0p1", "w");
    if (f) {
        fprintf(f, "FULLSCREEN\n");
        fflush(f);
        fclose(f);
    }
```

**2. QEMU Command Line** (scripts/run-alpine.sh:106)
```bash
-device virtio-serial-pci
-chardev socket,path=/tmp/qemu-display-ctl.sock,server=on,wait=off,id=display_ctl
-device virtserialport,chardev=display_ctl,name=org.qemu.display.0
```

**3. Host Side** (ui/cocoa.m:2282)
- GCD timer polls socket every 100ms
- Non-blocking read from Unix socket
- On "FULLSCREEN" command: `dispatch_async` to main queue → `doToggleFullScreen`

### Usage

**In Guest:**
```bash
# Load virtio-serial module (if not auto-loaded)
modprobe virtio_console

# Check device exists
ls -l /dev/vport0p1  # → crw------- 1 root root 251, 1

# In metalshader or any guest app
echo "FULLSCREEN" > /dev/vport0p1  # Toggles host fullscreen
```

**Commands:**
- `FULLSCREEN` - Toggle fullscreen on/off

### Benefits
✅ No host keyboard needed  
✅ Seamless guest→host integration  
✅ Extensible for future commands  
✅ Clean separation via virtio-serial  
✅ Negligible performance overhead (100ms polling)

### Limitations
- Requires virtio-serial device in QEMU
- Guest needs `/dev/vport0p1` device (requires virtio_console kernel module)
- Socket cleanup needed on QEMU crash (`rm /tmp/qemu-display-ctl.sock`)

### Future Extensions
Possible commands via same channel:
- `RESIZE:800x600` - Change window size
- `MINIMIZE` - Minimize window
- `MAXIMIZE` - Maximize window
- `SCREENSHOT` - Trigger host screenshot

### Testing
```bash
# Start QEMU
./scripts/run-alpine.sh

# In guest (after boot)
metalshader example
# Press F key → Host window goes fullscreen
# Press F again → Exit fullscreen

# Or test directly:
echo "FULLSCREEN" > /dev/vport0p1
```

### Troubleshooting

**Socket already exists:**
```bash
rm /tmp/qemu-display-ctl.sock
```

**/dev/vport0p1 missing in guest:**
```bash
# Check if virtio-serial detected
dmesg | grep virtio
# Load module manually
modprobe virtio_console
```

**No response from host:**
```bash
# Check socket is listening
lsof | grep qemu-display-ctl.sock
# Check QEMU debug output
# (Enable COCOA_DEBUG in ui/cocoa.m)
```

## Socket Direction Fix (2026-01-29)

### Problem
Original implementation had client/server roles backwards:
- ❌ QEMU chardev was server (`server=on`)
- ❌ Cocoa UI tried to connect as client
- Result: Commands never reached the host

### Solution
Corrected the roles:
- ✅ Cocoa UI is now server (bind/listen/accept)
- ✅ QEMU chardev is client (`server=off,reconnect=1`)
- Socket created by Cocoa *before* QEMU starts
- QEMU connects to existing socket on startup

### Updated Flow
```
Guest writes /dev/vport0p1
  → virtio-serial device
    → chardev (client, connects to Cocoa)
      → Unix socket
        → Cocoa UI (server, accepts & reads)
          → Window control (fullscreen/resize)
```

## Resolution Control (2026-01-29)

### New Command: RESIZE
Format: `RESIZE:WIDTHxHEIGHT`

**Example:**
```bash
echo "RESIZE:1920x1080" > /dev/vport0p1
```

**Constraints:**
- Min: 640x480
- Max: 3840x2160  
- Only works when NOT in fullscreen
- Window auto-centers after resize

### Helper Utility: qemu-resize

**Location:** `guest-demos/qemu-resize`

**Usage:**
```bash
# Copy to guest
scp -P 2222 guest-demos/qemu-resize root@localhost:/usr/local/bin/

# In guest
qemu-resize 800 600          # SVGA
qemu-resize 1920 1080        # Full HD
qemu-resize 2560 1440        # QHD
```

**Features:**
- Input validation
- Common resolution presets
- Clear error messages
- Checks for /dev/vport0p1

## Complete Testing Guide

### 1. Start QEMU
```bash
./scripts/run-alpine.sh
# Watch for: "Display control server listening on /tmp/qemu-display-ctl.sock"
```

### 2. Check Guest Setup
```bash
# SSH into guest
ssh -p 2222 root@localhost

# Load virtio_console module (if needed)
modprobe virtio_console

# Verify device exists
ls -l /dev/vport0p1
# Should show: crw------- 1 root root 251, 1 ...

# Check kernel messages
dmesg | grep virtio
# Should see: virtio_console virtio5: port0: guest port connected
```

### 3. Test Commands
```bash
# Test fullscreen
echo "FULLSCREEN" > /dev/vport0p1
# Mac window should enter fullscreen

# Test again to exit
echo "FULLSCREEN" > /dev/vport0p1

# Test resize
echo "RESIZE:1920x1080" > /dev/vport0p1
# Mac window should resize to 1920x1080

# Or use helper
qemu-resize 1280 720
```

### 4. Debug Issues

**Socket not found:**
```bash
# On Mac host
ls -la /tmp/qemu-display-ctl.sock
# If missing, check QEMU output for errors
```

**/dev/vport0p1 not found:**
```bash
# In guest
lsmod | grep virtio_console
# If not loaded:
modprobe virtio_console

# Check dmesg
dmesg | tail -20
```

**Commands have no effect:**
```bash
# Check if chardev connected
# In QEMU monitor (Ctrl+A C in serial console):
info chardev
# Should show display_ctl connected

# Check socket on Mac
lsof /tmp/qemu-display-ctl.sock
# Should show qemu-system-aarch64 connected
```

## Implementation Details

### Cocoa Server Socket (ui/cocoa.m:2328)
```objc
// Create server socket BEFORE QEMU starts
display_ctl_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
bind(display_ctl_server_fd, ...);
listen(display_ctl_server_fd, 1);

// Poll loop (100ms)
display_ctl_fd = accept(display_ctl_server_fd, ...);  // Non-blocking
read(display_ctl_fd, buf, ...);                       // Read commands
```

### QEMU Chardev (scripts/run-alpine.sh:108)
```bash
-chardev socket,path=/tmp/qemu-display-ctl.sock,\
         server=off,\     # Act as CLIENT
         wait=off,\       # Don't wait if server not ready
         reconnect=1,\    # Reconnect if disconnected
         id=display_ctl
```

### Data Flow Timing
1. **QEMU startup**: Cocoa creates socket first
2. **Chardev init**: QEMU connects to Cocoa's socket
3. **Guest boot**: virtio_console module loads
4. **Guest write**: Data flows instantly to Cocoa

### Performance
- Polling interval: 100ms
- Socket: Unix domain (no network overhead)
- Non-blocking I/O (no stalls)
- Overhead: < 0.1% CPU
