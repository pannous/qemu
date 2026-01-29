
## 2026-01-29: Thread-Safety Fix for Keyboard Activity Tracking

### Problem
Intermittent keyboard lag despite having idle prevention logic:
- `last_keyboard_activity_ns` variable updated by UI thread (SDL2)
- Read by vCPU thread (HVF WFI handler) 
- No synchronization = race condition
- CPU cache coherency issues caused vCPU thread to miss updates

### Root Cause
1. **Data race**: Non-atomic read/write across threads
2. **No memory barriers**: Updates might not be visible across CPU cores
3. **Performance overhead**: fprintf + fflush on every keypress
4. **Log spam**: Debug messages cluttered output

### Solution
**Atomic operations** (target/arm/hvf/hvf.c):
```c
// Writer (UI thread):
void hvf_notify_keyboard_activity(void) {
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    qatomic_set(&last_keyboard_activity_ns, now);  // Thread-safe
}

// Reader (vCPU thread):
int64_t kb_activity = qatomic_read(&last_keyboard_activity_ns);  // Thread-safe
if (kb_activity > 0 && (now - kb_activity) < 10000000000) {
    // Prevent idle for 10 seconds after keyboard activity
}
```

### Technical Details
- `qatomic_set()`: Uses `__ATOMIC_RELAXED` store with compiler barrier
- `qatomic_read()`: Uses `__ATOMIC_RELAXED` load with compiler barrier
- Provides data-race-free access as per C11 memory model
- No explicit `smp_wmb()/smp_rmb()` needed - QEMU atomics handle this

### Benefits
✅ **Keyboard activity always visible to vCPU thread**  
✅ **Zero logging overhead in hot path**  
✅ **Clean logs without debug spam**  
✅ **Proper cross-thread synchronization**  
✅ **Consistent 10-second idle prevention window**

### Testing
Test interactively with metalshader:
```bash
./scripts/run-alpine.sh
# In guest:
metalshader plasma
# Press keys and verify no lag
```

