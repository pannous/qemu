~/.claude/plans/quiet-inventing-lantern.md 

Pick up the plan when we have more tests done 

 Venus-Only Driver Architecture Plan

Executive Summary

Create a clean Venus-only virtio-gpu driver for macOS while preserving the existing GL+Venus driver as a  │
dormant reference. This provides a safety net during migration while allowing first-principles Vulkan     │
optimization.

Recommended Path: Phased approach - stabilize current implementation first, then create new driver.       │

---
Strategic Decision: Refined Hybrid Approach

User's Proposal (Refined):
- Create new Venus-only driver (hw/display/virtio-gpu-venus/)
- Move old GL+Venus driver to archive location (hw/display/legacy/)
- Prevent accidental modifications to old code
- Keep old driver buildable but not default

Refinement Recommendations:

1. Archive Location Structure

hw/display/
├── virtio-gpu-venus/          # NEW: Venus-only driver (production)
│   ├── virtio-gpu-venus.c
│   ├── virtio-gpu-venus-base.c
│   ├── virtio-gpu-vk-swapchain.m
│   └── meson.build
│
├── legacy-virtio-gpu/         # OLD: Archived GL+Venus (reference only)
│   ├── README.md              # "DO NOT MODIFY - archived for reference"
│   ├── virtio-gpu-virgl.c
│   ├── virtio-gpu-gl.c
│   └── meson.build.disabled
│
└── virtio-gpu*.c              # Current working code (until migration done)

2. Build System Protection

Prevent accidental builds of legacy code:
# legacy-virtio-gpu/meson.build.disabled
warning('Legacy virtio-gpu driver - DO NOT BUILD')
warning('Use hw/display/virtio-gpu-venus/ for new development')

Make new driver opt-in initially:
# hw/display/meson.build
option('experimental-venus-driver', type: 'boolean', value: false,
       description: 'Use experimental Venus-only driver (macOS)')

---
Phased Implementation Strategy

Phase 0: Stabilize Current Implementation (RECOMMENDED FIRST)

Timeline: 1-2 weeks
Goal: Build confidence with more working examples before big refactor

Tasks:
1. Fix remaining technical debt in current driver:
  - Replace last_venus_ctx_id global with per-resource tracking
  - Add multi-format support (beyond XRGB8888)
  - Improve resource→hostptr binding
2. Validate with additional examples:
  - ✅ vkcube (working at 47 FPS)
  - 🔲 Vulkan triangle (test_tri) stability
  - 🔲 Different resolutions (test resize)
  - 🔲 Multiple Vulkan contexts (ensure tracking works)
  - 🔲 Redox OS boot attempt (ultimate target)
3. Document working patterns:
  - IOSurface zero-copy path
  - Host swapchain presentation
  - Blob resource lifecycle

Exit Criteria:
- At least 2 different Vulkan apps working reliably
- Zero-copy performance validated (maintain 47 FPS)
- Technical debt reduced
- Clear patterns documented for new driver

Why stabilize first?
- Reduces risk: proven working code before big change
- Clear requirements: know what the new driver must implement
- Confidence: team understands zero-copy architecture
- Reference: old driver is known-good baseline

---
Phase 1: Create New Venus-Only Driver

Timeline: 2-3 weeks (after Phase 0 complete)
Goal: Clean Vulkan implementation from first principles

1.1 File Structure

New driver files:
hw/display/virtio-gpu-venus/
├── virtio-gpu-venus.c           (~1200 lines - main device logic)
├── virtio-gpu-venus-base.c      (~400 lines - resource management)
├── virtio-gpu-venus-pci.c       (~100 lines - PCI device wrapper)
├── virtio-gpu-vk-swapchain.m    (COPY from current, 669 lines)
└── virtio-gpu-venus-common.h    (~200 lines - internal API)

include/hw/virtio/
└── virtio-gpu-venus.h           (~150 lines - public API)

1.2 Code to Copy from Current Driver

Copy and adapt (proven working):

From hw/display/virtio-gpu-virgl.c:
- IOSurface zero-copy logic (lines 127-175, 186-338)
- Venus blob resource management (lines 1567-1632)
- Host swapchain integration (lines 968-1140)
- Fence polling (lines 2048-2088)
- Venus context lifecycle (lines 760-815)
- Present timer mechanism (lines 295-368)

What to remove completely:
- All #ifdef CONFIG_OPENGL blocks (23 instances)
- EGL/GL context management (lines 2092-2184)
- GL scanout paths (dpy_gl_* calls)
- 2D transfer via vrend (lines 568-580)
- GL resource creation fallbacks

1.3 Architecture Simplifications

Single capset: Venus only
// Old driver supports multiple capsets
switch (capset) {
    case VIRGL: case VIRGL2: /* GL path */
    case VENUS: /* Vulkan path */
    case DRM: /* Native path */
}

// New driver: Venus only
if (capset != VIRTGPU_DRM_CAPSET_VENUS) {
    return -EINVAL;
}

No GL dependencies:
// Remove these includes entirely:
// #include "ui/egl-helpers.h"
// #include "ui/console.h" (GL portions)

// Add clean Vulkan-only:
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_metal.h>

Direct virglrenderer Venus API:
// Old: Generic dispatcher
virgl_renderer_context_create_with_flags(ctx_id, capset, ...);

// New: Direct Venus API (if available from virglrenderer)
vkr_context_create(ctx_id, ...);  // More explicit

1.4 Build Configuration

New meson.build:
virtio_gpu_venus_ss = ss.source_set()

# Venus driver only builds on macOS with Vulkan
if host_os == 'darwin' and virgl.found()
  virtio_gpu_venus_ss.add(
    files('virtio-gpu-venus.c',
          'virtio-gpu-venus-base.c',
          'virtio-gpu-venus-pci.c',
          'virtio-gpu-vk-swapchain.m'),
    dependencies: [virgl, vulkan, metal_framework, quartzcore_framework],
    install: true
  )

  hw_display_modules += {'virtio-gpu-venus': virtio_gpu_venus_ss}
endif

Command line configuration:
# Enable Venus-only driver
./configure --enable-virtio-gpu-venus
# or
meson configure -Dvirtio-gpu-venus=true

---
Phase 2: Archive Old Driver

Timeline: 1 week (after new driver proven)
Goal: Preserve reference code, prevent accidental edits

2.1 Archive Structure

Move files:
mkdir -p hw/display/legacy-virtio-gpu

# Move old implementation
git mv hw/display/virtio-gpu-virgl.c hw/display/legacy-virtio-gpu/
git mv hw/display/virtio-gpu-gl.c hw/display/legacy-virtio-gpu/

# Create protection README
cat > hw/display/legacy-virtio-gpu/README.md << 'EOF'
# Legacy Virtio-GPU Driver (ARCHIVED)

⚠️ **DO NOT MODIFY THIS CODE** ⚠️

This is the archived GL+Venus hybrid driver preserved for reference.
All new development should happen in `../virtio-gpu-venus/`.

## Purpose
- Historical reference
- Fallback if new driver has issues
- Code comparison during debugging

## To Build (emergency only)
Rename `meson.build.disabled` → `meson.build`

Last working commit: [commit hash]
EOF

2.2 Build System Protection

Disable by default:
# legacy-virtio-gpu/meson.build.disabled
if get_option('build-legacy-virtio-gpu')
  error('Legacy driver should not be built. Use for reference only.')
endif

Add option (default disabled):
# meson_options.txt
option('build-legacy-virtio-gpu', type: 'boolean', value: false,
       description: 'Build legacy GL+Venus driver (not recommended)')

---
Phase 3: Validation & Testing

Timeline: Ongoing during Phases 1-2

3.1 Functional Tests

Working examples to validate:
1. ✅ vkcube animation (47 FPS maintained)
2. 🔲 Vulkan triangle (test_tri)
3. 🔲 Multiple contexts simultaneously
4. 🔲 Window resize handling
5. 🔲 Resource destruction cleanup
6. 🔲 Redox OS boot (if applicable)

Test commands:
# Build new driver
cd /opt/other/qemu
meson configure -Dvirtio-gpu-venus=true
ninja

# Test in Alpine VM
./scripts/run-alpine.sh

# In guest:
cd /root/vkcube && ./vkcube_anim  # Should maintain 47 FPS
cd /root/triangle && ./test_tri   # Should render correctly

3.2 Performance Benchmarks

Maintain current performance:
- Zero-copy path: 47 FPS ✅
- Host swapchain present: <1ms latency
- Resource creation: <10ms
- Context switch: <5ms

Test scenarios:
# FPS benchmark
./vkcube_anim  # Target: ≥47 FPS

# Memory usage
while true; do ps aux | grep qemu; sleep 1; done

# Latency test (input to display)
# [Custom test - measure keyboard to pixel change]

3.3 Regression Testing

Ensure no breakage:
- IOSurface zero-copy still works
- Vulkan swapchain creation succeeds
- MoltenVK ICD found correctly
- Blob resource allocation
- Fence synchronization

---
Critical Files for Implementation

Files to Create (New Driver)

Primary implementation:
- hw/display/virtio-gpu-venus/virtio-gpu-venus.c - Main device logic
- hw/display/virtio-gpu-venus/virtio-gpu-venus-base.c - Resource management
- hw/display/virtio-gpu-venus/meson.build - Build configuration
- include/hw/virtio/virtio-gpu-venus.h - Public API

Copy with modifications:
- hw/display/virtio-gpu-venus/virtio-gpu-vk-swapchain.m - From current driver (working)

Files to Archive (Old Driver)

Move to legacy/:
- hw/display/virtio-gpu-virgl.c (2407 lines)
- hw/display/virtio-gpu-gl.c (262 lines)
- hw/display/virtio-gpu-base.c (if GL-specific portions)

Preserve for reference:
- hw/display/legacy-virtio-gpu/README.md - Archive documentation
- hw/display/legacy-virtio-gpu/meson.build.disabled - Disabled build

Files to Modify

Build system:
- hw/display/meson.build - Add Venus driver option
- meson_options.txt - Add configuration options
- hw/display/Kconfig - Add Venus driver config

Device registration:
- hw/virtio/meson.build - Register new device
- qemu-options.hx - Add Venus driver CLI options

---
Dependencies & Prerequisites

Build Dependencies

Required:
- virglrenderer ≥1.0 with Venus support
- Vulkan SDK (MoltenVK on macOS)
- Metal framework (macOS)
- QuartzCore framework (macOS)

Not needed (removed from old driver):
- OpenGL/EGL libraries
- GBM (Linux GL buffer management)
- X11/Wayland GL extensions

Verify virglrenderer Venus:
cd /opt/other/virglrenderer
grep -r "VIRGL_RENDERER_VENUS" install/include/

# Should show:
# install/include/virgl/virglrenderer.h:#define VIRGL_RENDERER_VENUS ...

Runtime Dependencies

macOS host:
- MoltenVK ICD discoverable (/usr/local/share/vulkan/icd.d/MoltenVK_icd.json)
- Vulkan loader (libvulkan.1.dylib)
- Metal support (built into macOS)

Guest:
- Mesa Venus driver (/root/mesa-25.0.2 on Alpine)
- Vulkan applications (vkcube, test_tri, etc.)

---
Verification Plan

Phase 0 Verification (Before New Driver)

Success criteria:
# Test 1: vkcube FPS
./vkcube_anim | grep FPS  # Should show ~47 FPS

# Test 2: Multiple contexts
./test_multi_context      # Create 2+ Venus contexts

# Test 3: Resize handling
# Resize window, check no crashes

# Test 4: Clean shutdown
# Ctrl+C, verify no memory leaks

Documentation:
- Working examples catalog
- Known limitations list
- IOSurface zero-copy flow diagram

Phase 1 Verification (New Driver)

Functional tests:
# Build with new driver
meson configure -Dvirtio-gpu-venus=true -Dvirtio-gpu-virgl=false
ninja

# Same tests as Phase 0
./scripts/run-alpine.sh
# Run vkcube, triangle, etc.

Performance comparison:
┌─────────┬────────────┬────────────┬───────────────────────┐
│ Metric  │ Old Driver │ New Driver │        Status
├─────────┼────────────┼────────────┼───────────────────────┤
│ FPS47    ≥47   Must match or improve │
├─────────┼────────────┼────────────┼───────────────────────┤
│ Memory  │ X MB  ≤X MB Must not increase
├─────────┼────────────┼────────────┼───────────────────────┤
│ Startup │ Y ms  ≤Y ms Must not regress
└─────────┴────────────┴────────────┴───────────────────────┘
Code quality:
# No CONFIG_OPENGL in new code
grep -r "CONFIG_OPENGL" hw/display/virtio-gpu-venus/
# Should return nothing

# No GL includes
grep -r "egl-helpers.h\|GL/gl.h" hw/display/virtio-gpu-venus/
# Should return nothing

Phase 2 Verification (Archive)

Protection checks:
# Verify old driver not in build
ninja -t targets | grep virtio-gpu-virgl
# Should return nothing (or legacy path only)

# Verify README protection
cat hw/display/legacy-virtio-gpu/README.md
# Should show "DO NOT MODIFY" warning

# Verify build disabled
test -f hw/display/legacy-virtio-gpu/meson.build.disabled
echo $?  # Should be 0 (exists)

---
Risk Mitigation

Risk: New Driver Breaks Something

Mitigation:
- Phase 0 validation before starting
- Keep old driver accessible (archived)
- Incremental testing during development
- Git branches for safe experimentation

Rollback plan:
# If new driver fails, re-enable old
git checkout hw/display/virtio-gpu-virgl.c
git checkout hw/display/virtio-gpu-gl.c
meson configure -Dvirtio-gpu-venus=false
ninja

Risk: Performance Regression

Mitigation:
- Benchmark old driver first (baseline)
- Profile new driver during development
- Compare IOSurface code paths (should be identical)
- Test on same hardware/VM

If regression found:
- Compare code differences
- Use old driver as reference
- Check for unnecessary synchronization
- Verify zero-copy path preserved

Risk: Incomplete Feature Coverage

Mitigation:
- Copy working code from old driver
- Test matrix: vkcube, triangle, resize, multi-context
- Document any intentional removals (GL features)
- Keep test suite from Phase 0

---
Alternative: Wait for More Examples

User's question: "Should we wait until we have more working examples?"

Recommendation: Yes - Do Phase 0 first

Rationale:
1. Current proof-of-concept: 1 app (vkcube) at 47 FPS
2. Unknown unknowns: What breaks with multiple contexts? Different formats?
3. Learning opportunity: Understand zero-copy deeply before rewriting
4. Lower risk: Proven patterns before architectural change

Additional examples to pursue:
# 1. Multiple Vulkan apps
./test_tri        # Simple triangle
./vkcube_anim     # Animated cube (current)
./vulkan_gears    # If available

# 2. Stress tests
./multi_context   # Multiple Venus contexts
./rapid_resize    # Resize stress test
./format_test     # ARGB, BGRA, etc.

# 3. Real workload
# Boot Redox OS (ultimate goal)

When to proceed to new driver:
- ✅ At least 3 different Vulkan apps working
- ✅ Multi-context tested
- ✅ Format handling validated
- ✅ Team comfortable with architecture
- ✅ Technical debt in current driver reduced

---
Timeline Estimate

Conservative (Recommended)

Phase 0: Stabilization (2-3 weeks)
- Week 1: Fix technical debt (resource tracking, formats)
- Week 2-3: Test additional examples, document patterns

Phase 1: New Driver (3-4 weeks)
- Week 1: File structure, basic device setup
- Week 2: Copy IOSurface/swapchain code, test
- Week 3: Resource management, fence handling
- Week 4: Polish, performance tuning

Phase 2: Archive (1 week)
- Move files, add protection, test builds

Total: 6-8 weeks (with proper validation)

Aggressive (Higher Risk)

Skip Phase 0, go straight to new driver: 4-5 weeks
- Not recommended unless very confident in current architecture

---
Recommendation Summary

Proposed Path

1. Now → 2-3 weeks: Phase 0 Stabilization
  - Fix last_venus_ctx_id global → per-resource tracking
  - Add format support beyond XRGB8888
  - Test 2-3 more Vulkan apps
  - Document zero-copy patterns
2. After validation: Phase 1 New Driver
  - Create clean Venus-only implementation
  - Copy proven working code
  - Remove all GL dependencies
  - Maintain 47 FPS performance
3. After new driver stable: Phase 2 Archive
  - Move old driver to legacy-virtio-gpu/
  - Add build protection
  - Keep as reference only

Why This Order?

✅ Reduces risk: Known-good baseline before big change
✅ Builds confidence: More examples prove architecture
✅ Clear requirements: Understand what new driver needs
✅ Safety net: Old driver available if problems arise
✅ Learning: Team understands zero-copy deeply

Exit Criteria for Each Phase

Phase 0 → Phase 1:
- 3+ Vulkan apps working reliably
- Multi-context tested
- Performance characterized (47 FPS baseline)
- Technical debt reduced

Phase 1 → Phase 2:
- New driver matches old driver functionality
- Performance ≥47 FPS maintained
- All tests passing
- No regressions found

Phase 2 → Production:
- New driver default for ≥2 weeks
- Zero issues reported
- Old driver not needed
- Documentation complete

---
Open Questions for User

1. Timeline urgency: Is 6-8 week timeline acceptable? Or need faster?
2. Redox OS priority: When do you need Redox booting? Critical for Phase 0?
3. Multi-format: Which formats beyond XRGB8888 are actually needed?
4. Linux support: Should new driver work on Linux too, or macOS-only?

---
Final Recommendation

START WITH PHASE 0 - Stabilize current implementation with more examples.

Why:
- Low risk way to build confidence
- Validates architecture before rewrite
- Identifies any gaps in current approach
- Provides clear baseline for new driver

Then proceed to Phases 1-2 when ready.

Your hybrid approach is excellent - creating new driver while keeping old as dormant reference. The       │
phased approach just adds safety and learning before the big change.   