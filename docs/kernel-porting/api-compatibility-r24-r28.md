# Kernel API Compatibility: R24.1 (3.10) vs R28 (4.4) — Breaking Changes

## Overview

Both kernels are based on Linux 3.10.x, with R24.1 at patchlevel 108. When comparing to R28 (4.4), we can observe the full scope of NVIDIA's own porting work between kernel versions.

This document analyzes the breaking changes between these BSP versions to assess compatibility risks for porting efforts.

---

## tegra_dc_ext.h Changes

Comparing R24.1 headers against older JXD headers reveals several API modifications:

### HIGH RISK Breaking Changes

#### 1. flip_windowattr timestamp
```c
// OLD (binary incompatible on 64-bit)
struct timespec timestamp;

// NEW
struct tegra_timespec {
    __s32 tv_sec;
    __s32 tv_nsec;
} timestamp;
```
**Impact:** Binary incompatible on 64-bit systems due to timespec size differences.

#### 2. flip_3 layout change
```c
// OLD
__u32 reserved1[2];

// NEW
__u32 flags;
struct tegra_dc_ext_rect dirty_rect;
```
**Impact:** Semantic change in window flip behavior.

#### 3. Event type values
```c
// OLD (sequential values)
TEGRA_DC_EXT_EVENT_MODESET = 0x1,
TEGRA_DC_EXT_EVENT_BANDWIDTH_INC = 0x3,
TEGRA_DC_EXT_EVENT_BANDWIDTH_DEC = 0x4,

// NEW (bit masks)
TEGRA_DC_EXT_EVENT_MODESET = 1 << 0,
TEGRA_DC_EXT_EVENT_BANDWIDTH_INC = 1 << 2,
TEGRA_DC_EXT_EVENT_BANDWIDTH_DEC = 1 << 3,
```
**Impact:** Event handling logic must be updated.

#### 4. nvhost submit_args field reordering
```c
// Fields reordered in structure
struct nvhost_submit_args {
    // Binary incompatible layout
};
```
**Impact:** Complete binary incompatibility for submit operations.

#### 5. Removed nvhost ioctls
The following 11 ioctls were removed (numbers 8, 100-110 range):
- GPFIFO operations
- WAIT operations
- ZCULL management
- OBJ_CTX (object context)

**Impact:** GPU-specific functionality removed from nvhost interface.

---

## MEDIUM RISK Changes

### 1. NVMAP_IOC_CLAIM Removed
```c
// IOCTL 1 removed
#define NVMAP_IOC_CLAIM  // NO LONGER AVAILABLE
```

### 2. New Required Flags Handling
Additional flag validation in allocation paths.

---

## LOW RISK (Additive) Changes

### 1. New DC ioctls (10 added)
- FLIP4 — Extended flip operation
- CSC_V2 — Color space conversion v2
- CMU_V2 — Color management unit v2
- And others

### 2. New nvmap alloc flags
Extended allocation attribute flags.

### 3. New nvhost ioctls
Client-managed syncpoint support added.

---

## Compatibility Assessment

### HWC2 (Hardware Composer 2) Compatibility

**Critical Finding:** The nvhost ioctls that were REMOVED (100-111 range) are **GPU-specific**. They are **NOT** used by the Hardware Composer or display path.

HWC uses only:
- DC (Display Controller) ioctls
- nvmap basic operations

Both of these remain compatible between versions.

### nvmap Compatibility Matrix

| IOCTL | R24.1 | R28 | Status |
|-------|-------|-----|--------|
| CREATE | Yes | Yes | Compatible |
| ALLOC | Yes | Yes | Compatible |
| FREE | Yes | Yes | Compatible |
| PIN | Yes | Yes | Compatible |
| UNPIN | Yes | Yes | Compatible |
| CACHE | Yes | Yes | Compatible |
| GET_FD | Yes | Yes | Compatible |
| FROM_FD | Yes | Yes | Compatible |
| MMAP | Yes | Yes | Compatible |

**Conclusion:** Core nvmap is stable across versions.

### nvhost (Non-GPU) Compatibility

| IOCTL Range | Purpose | Status |
|-------------|---------|--------|
| 1-27 | Channel operations | Largely stable |
| 100+ | GPU-specific (GPFIFO, OBJ_CTX, ZCULL) | **REMOVED** |

**Note:** 2D and VIC channels do not use the removed ioctls.

---

## GPU Driver Migration

### Path Changes
```
R24.1 and earlier:  drivers/video/tegra/host/gk20a/
R28 and later:      drivers/gpu/nvgpu/gk20a/
```

### Device Node Naming
```
OLD: nvhost-<module> pattern
NEW: nvhost-gpu unified pattern
```

**Important:** R24.1 userspace blobs are already aware of the new device path naming convention.

---

## Summary

| Component | Risk Level | Notes |
|-----------|------------|-------|
| Display (DC) | LOW | Core ioctls stable, new features additive |
| nvmap | LOW | Fully compatible |
| nvhost (non-GPU) | LOW | Channel ioctls 1-27 stable |
| nvhost (GPU) | HIGH | 100+ range ioctls removed |
| GPU driver | MEDIUM | Path changed, blobs compatible |

For a display-focused port (without GPU acceleration), the API surface is largely stable between R24.1 and R28. The primary concerns are:
1. 64-bit timestamp handling in flip operations
2. Event type value changes if using display events
