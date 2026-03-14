# HWC2 Implementation Plan for Tegra K1

## Executive Summary

This document outlines a comprehensive plan for implementing HWC2 (Hardware Composer HAL version 2) for the NVIDIA Tegra K1 platform. The migration from HWC1 to HWC2 is necessitated by Android 9+ dropping support for HWC1, and critical fence handling issues in the hwc2on1 adapter that cause system instability.

## Why HWC2 is Required

### Android Version Requirements

Android 9 (API level 28) and later versions have removed support for HWC1. While a compatibility layer (`hwc2on1` adapter) exists to bridge HWC1 implementations to the HWC2 interface, this adapter has proven problematic with the NVIDIA HWC1 driver.

### Root Cause of Fence FD Leak

The hwc2on1 adapter exhibits a critical fence file descriptor leak when paired with the NVIDIA HWC1 implementation:

1. **FD "Theft" Pattern**: The NVIDIA HWC1 driver "steals" the file descriptor from the layer structure by zeroing `acquireFenceFd` after taking ownership. The hwc2on1 adapter does not consistently track this ownership transfer.

2. **Framebuffer Cache Reuse**: When framebuffer layers are recycled, the NVIDIA HWC1 closes fences internally via `nvsync_close()`. The adapter may attempt to close the same fd again, causing double-close or use-after-close errors.

3. **Retire Fence Duplication**: The retire fence is one per display, but the adapter must duplicate it for per-layer release fences. Each `nvsync_dup()` creates a new fd. If the adapter does not track all duplicated fences, they leak.

4. **Accumulation**: Over time, accumulated fd leaks hit the process file descriptor limit, causing the SurfaceFlinger process to crash and the system to become unstable.

### Solution: Native HWC2 Implementation

A native HWC2 implementation eliminates these issues by:
- Implementing proper fence ownership semantics as defined by HWC2 specification
- Eliminating the adapter abstraction layer
- Providing direct control over fence lifecycle

## R24.1 Kernel API Adaptations

The Tegra K1 uses the R24.1 (Rel-24) kernel branch, which has specific API differences from mainline or newer Tegra kernels. Three specific adaptations are required:

### 1. Timestamp Structure

**Change**: `struct timespec` → `struct tegra_timespec`

```c
// Old (mainline Linux)
struct timespec {
    long tv_sec;
    long tv_nsec;
};

// R24.1
struct tegra_timespec {
    __u32 tv_sec;
    __u32 tv_nsec;
};
```

**Impact**: Fixed-size 32-bit fields instead of architecture-dependent `long` types.

### 2. Flip3 Structure Flags Field

**Change**: `reserved1` → `flags` field, `dirty_rect` added

```c
struct tegra_dc_ext_flip_3 {
    __u64 win;                      // Pointer to windowattr array
    __u8 win_num;                   // Number of windows
    __u8 flags;                     // R24.1: was reserved1
    __s32 post_syncpt_fd;           // Output: present/retire fence fd
    __u16 dirty_rect[4];            // R24.1: partial update rect
};
```

**Impact**: Enables partial display updates and flip flags.

### 3. Event Type Encoding

**Change**: Sequential values → Bit masks

```c
// Old kernel
#define TEGRA_DC_EXT_EVENT_HOTPLUG       0x1
#define TEGRA_DC_EXT_EVENT_BANDWIDTH_INC 0x3
#define TEGRA_DC_EXT_EVENT_BANDWIDTH_DEC 0x4

// R24.1
#define TEGRA_DC_EXT_EVENT_HOTPLUG       (1 << 0)  // 0x1
#define TEGRA_DC_EXT_EVENT_VBLANK        (1 << 1)  // 0x2 (new)
#define TEGRA_DC_EXT_EVENT_BANDWIDTH_INC (1 << 2)  // 0x4
#define TEGRA_DC_EXT_EVENT_BANDWIDTH_DEC (1 << 3)  // 0x8
```

**Impact**: Event handling code must use bit tests instead of equality checks.

## Proposed File Structure

```
hwc2_tegra/
├── HwcDevice.cpp           # getFunction/getCapabilities, device lifecycle
├── HwcDevice.h
├── HwcDisplay.cpp          # Per-display state, validate/present
├── HwcDisplay.h
├── HwcLayer.cpp            # Per-layer state (buffer, blend, transform, etc.)
├── HwcLayer.h
├── TegraDisplayBackend.cpp # Wrapper over /dev/tegra_dc_*, ioctls
├── TegraDisplayBackend.h
├── FenceManager.cpp        # STRICT lifecycle: acquire→wait→close, create release
├── FenceManager.h
├── CompositionEngine.cpp   # Window assignment (port logic from nvhwc_assign_windows)
├── CompositionEngine.h
└── Android.bp
```

**Estimated Implementation Size**: 3000-5000 lines of C/C++

## HWC2 API Flow vs HWC1

### HWC1 Flow (Legacy)

```
prepare()
    └── Analyze layers, mark as OVERLAY or FRAMEBUFFER
set()
    └── Execute composition, post to display
```

### HWC2 Flow (Modern)

```
createLayer() / destroyLayer()     # Per-layer lifecycle
setLayerBuffer()                   # Set buffer and acquire fence
setLayerBlendMode()                # Set blend mode
setLayerCompositionType()          # Set CLIENT or DEVICE
setLayerDisplayFrame()             # Set destination rect
setLayerPlaneAlpha()               # Set plane alpha
setLayerSourceCrop()               # Set source rect
setLayerTransform()                # Set transform
setLayerVisibleRegion()            # Set visible region
setLayerZOrder()                   # Set Z-order
validateDisplay()                  # Validate configuration
getChangedCompositionTypes()       # Query what changed
acceptDisplayChanges()             # Accept validation result
presentDisplay()                   # Execute and present
```

## Key HWC2 Functions to Implement

### Device Level
- `getCapabilities()` - Report supported capabilities
- `getFunction()` - Return function pointer table

### Display Level
- `createLayer()` / `destroyLayer()` - Layer lifecycle
- `validateDisplay()` - Validate display configuration
- `presentDisplay()` - Execute composition
- `acceptDisplayChanges()` - Accept validation changes
- `getChangedCompositionTypes()` - Query composition changes
- `getDisplayRequests()` - Query display requests
- `setClientTarget()` - Set client composition target
- `setActiveConfig()` - Set display configuration
- `setPowerMode()` - Set display power state
- `setVsyncEnabled()` - Enable/disable VSync
- `getDisplayAttribute()` - Query display attributes
- `getDisplayConfigs()` - Query available configs
- `getDisplayType()` - Query display type
- `getColorModes()` - Query color modes

### Layer Level
- `setLayerBuffer()` - Set layer buffer
- `setLayerBlendMode()` - Set blend mode
- `setLayerCompositionType()` - Set composition type
- `setLayerDisplayFrame()` - Set display frame
- `setLayerPlaneAlpha()` - Set plane alpha
- `setLayerSourceCrop()` - Set source crop
- `setLayerTransform()` - Set transform
- `setLayerVisibleRegion()` - Set visible region
- `setLayerZOrder()` - Set Z-order

### Callbacks
- `registerCallback()` - Register hotplug, vsync, refresh callbacks

## Virtual Displays

For virtual displays (screencast, screen recording), return 0 from `getMaxVirtualDisplayCount()` to indicate no hardware virtual display support. SurfaceFlinger will handle virtual displays via GPU composition.

## Color Transform

Color transform (HDR, color correction) should fall back to CLIENT composition. The Tegra K1 display controller has limited color transform capabilities compared to modern GPUs.

## HIDL Wrapper

Use the AOSP `android.hardware.graphics.composer@2.1-service` as a reference for the HIDL wrapper. The HWC2 implementation provides the backend, and the HIDL service wraps it for the Android framework.

## Incremental Implementation Approach

### Phase 1: Minimal Implementation
**Goal**: Validate fence lifecycle without complex composition

- All layers → CLIENT composition
- Only present fence handling
- Basic display enumeration

**Validation**: Monitor `/proc/<pid>/fd/` for SurfaceFlinger to ensure no fd leaks during basic operation.

### Phase 2: Add Overlay Support
**Goal**: Hardware composition for simple cases

- Port window assignment logic from `nvhwc_assign_windows()`
- Support basic RGBA layers without transformation
- Handle Z-order correctly

### Phase 3: Add Scratch Blit
**Goal**: Support transformations and scaling

- Integrate `libnvblit` for scratch buffer operations
- Support rotation and scaling beyond DC limits
- Handle format conversion

### Phase 4: Add Cursor Support
**Goal**: Hardware cursor optimization

- Dedicated cursor window handling
- Cursor position updates without full composition

### Phase 5: Add HDMI/External Display
**Goal**: Multi-display support

- Hotplug detection
- External display configuration
- Clone/extend modes

## Testing Strategy

1. **Fence Leak Test**: Monitor fd count during extended video playback
2. **Composition Test**: Verify all composition paths (CLIENT, DEVICE, CURSOR)
3. **Hotplug Test**: HDMI connect/disconnect cycles
4. **Stress Test**: Rapid orientation changes, app switching
5. **Power Test**: Verify proper power state transitions

## Conclusion

Implementing native HWC2 for Tegra K1 is essential for Android 9+ compatibility and system stability. The R24.1 kernel adaptations are well-defined, and the incremental approach allows for validation at each stage. The elimination of the hwc2on1 adapter removes the fence leak issue at its source.
