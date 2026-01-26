# HVF WFI Sleep - Adaptive Idle CPU Reduction

## Problem

HVF on macOS has spurious WFI (Wait For Interrupt) wakeups causing:
- **300-350% idle CPU usage** (tight spinning in WFI loop)
- Thermal throttling, battery drain, fan noise
- Standard QEMU halt mechanisms don't work on macOS HVF

## Solution

Adaptive WFI sleep controlled by `HVF_WFI_SLEEP` environment variable:
- Disabled by default (safe, no behavior change)
- Activates **after 15 seconds** (preserves fast boot)
- Adds microsecond sleep per WFI call when idle
- System remains fully responsive (CPU scales up during activity)

## Usage

### Quick Start (Recommended)

```bash
export HVF_WFI_SLEEP=100  # Best balance
./scripts/run-alpine.sh
```

### Configuration Options

| Value | Sleep | Idle CPU | Use Case |
|-------|-------|----------|----------|
| `0` or unset | None | ~300% | Default (current behavior) |
| `10` | 10μs | ~30-40% | Mild reduction, very responsive |
| `100` | 100μs | ~6-7% | **Recommended** - best balance |
| `1000` | 1ms | ~5% | Max reduction (may affect responsiveness) |

### Performance Results (HVF_WFI_SLEEP=100)

- ✅ Boot time: **2s** (unchanged from baseline)
- ✅ Idle CPU: **6-7%** (down from 300-350%)
- ✅ Responsiveness: **Normal** (341% during SSH activity)
- ✅ Thermal: Significantly reduced heat/fan noise
- ✅ Battery: Much better power efficiency

## How It Works

1. **Boot Phase (0-15s)**: No sleep applied
   - Fast boot preserved (~2s for Alpine)
   - Full CPU available for boot workload

2. **Post-Boot (15s+)**: Sleep activated
   - Small delay added to each WFI call
   - Reduces tight spinning, lowers CPU usage
   - Sleep duration configurable via env var

3. **Activity Detection**: Automatic
   - When guest does work, CPU naturally scales up
   - Sleep still applied but dominated by actual work
   - No manual activation/deactivation needed

## Making It Permanent

### For Your User Profile

Add to `~/.zshrc` or `~/.bashrc`:

```bash
# Reduce QEMU idle CPU on macOS
export HVF_WFI_SLEEP=100
```

### For Specific Scripts

Edit launch scripts to set before starting QEMU:

```bash
export HVF_WFI_SLEEP=100
$QEMU -M virt -accel hvf ...
```

## Testing

Verify it's working:

```bash
export HVF_WFI_SLEEP=100
./scripts/run-alpine.sh &
QEMU_PID=$!

# Check CPU after 20s (should be <10%)
sleep 20
ps -p $QEMU_PID -o pid,pcpu,comm
```

## Technical Details

- Implementation: `target/arm/hvf/hvf.c::hvf_wfi()`
- Timer: `QEMU_CLOCK_REALTIME` (host monotonic time)
- Sleep: `g_usleep()` (GLib microsecond sleep)
- No impact on: interrupts, timers, or guest scheduling

## Troubleshooting

**Sleep not activating?**
- Wait at least 20 seconds after boot
- Check env var is set: `echo $HVF_WFI_SLEEP`
- Verify signed binary: `./scripts/rebuild-qemu.sh quick`

**System feels sluggish?**
- Try lower value: `export HVF_WFI_SLEEP=10`
- Or disable: `unset HVF_WFI_SLEEP`

**Boot time increased?**
- Should not happen (15s delay before activation)
- If boot >5s, something else is wrong

## Related Issues

- macOS HVF spurious wakeups (Apple Hypervisor framework limitation)
- QEMU halt mechanisms ineffective on macOS
- Previous attempts: g_usleep delays during boot (slowed boot to 200s)
- Solution: Time-based activation avoids boot penalty
