# HWC1 vs HWC2 — Detailed API and Fence Semantics Comparison

## Overview

This document provides a comprehensive comparison between Hardware Composer HAL version 1 (HWC1) and version 2 (HWC2), focusing on API differences, composition flow changes, and critical fence semantics that affect implementation correctness.

## API Style Comparison

| Aspect | HWC1 (Existing) | HWC2 (Target) |
|--------|-----------------|---------------|
| **API Style** | C struct with function pointers (`hwc_composer_device_1_t`) | C function pointers via `getFunction()` or HIDL `IComposerClient` |
| **Composition Flow** | `prepare()` → `set()` | `validateDisplay()` → `getChangedCompositionTypes()` → `acceptDisplayChanges()` → `presentDisplay()` |
| **Layer Management** | Array of `hwc_layer_1_t` passed as batch | Each layer created/destroyed explicitly via `createLayer()`/`destroyLayer()`, properties set individually |
| **Display Management** | Implicit `HWC_DISPLAY_PRIMARY`/`EXTERNAL` | Explicit handles, hotplug callback |
| **Composition Types** | `HWC_OVERLAY` / `HWC_FRAMEBUFFER` / `HWC_FRAMEBUFFER_TARGET` | `CLIENT` / `DEVICE` / `SOLID_COLOR` / `CURSOR` / `SIDEBAND` |
| **Present Fence** | `retireFenceFd` in `hwc_display_contents_t` | `setPresentFence()` in output command queue |
| **Release Fence** | `releaseFenceFd` in each `hwc_layer_t` | `setReleaseFences()` as separate command |
| **Client Target** | `HWC_FRAMEBUFFER_TARGET` layer in list | Explicit `setClientTarget()` call |
| **Virtual Display** | Not well defined | `createVirtualDisplay()`/`destroyVirtualDisplay()` |
| **Color Transform** | Not in HWC1 | `SET_COLOR_TRANSFORM` command |

## Composition Flow Deep Dive

### HWC1 Flow

```
SurfaceFlinger
    │
    ▼
hwc_prepare(display, layers[], numLayers)
    │
    ├── Iterate through layers
    ├── Mark each as HWC_OVERLAY or HWC_FRAMEBUFFER
    └── Return: composition strategy decided
    │
    ▼
hwc_set(display, layers[], numLayers)
    │
    ├── Execute composition (if needed)
    ├── Post to display
    └── Return: retireFenceFd + releaseFenceFd per layer
```

**Key characteristics:**
- Single batch operation for all layers
- Composition type decided in `prepare()`, executed in `set()`
- Fence handling implicit in layer structure

### HWC2 Flow

```
SurfaceFlinger
    │
    ├── createLayer(display, layerHandle*)
    │   └── Allocate layer state
    │
    ├── setLayerBuffer(layerHandle, buffer, acquireFence)
    ├── setLayerBlendMode(layerHandle, mode)
    ├── setLayerCompositionType(layerHandle, type)
    ├── setLayerDisplayFrame(layerHandle, rect)
    ├── setLayerPlaneAlpha(layerHandle, alpha)
    ├── setLayerSourceCrop(layerHandle, rect)
    ├── setLayerTransform(layerHandle, transform)
    ├── setLayerVisibleRegion(layerHandle, region)
    └── setLayerZOrder(layerHandle, z)
    │
    ▼
validateDisplay(display, outNumTypes, outNumRequests)
    │
    ├── Validate all layer configurations
    ├── Determine composition strategy
    └── Return: validation result
    │
    ▼
getChangedCompositionTypes(display, numTypes, layers[], types[])
    └── Query which layers changed composition type
    │
    ▼
acceptDisplayChanges(display)
    └── Accept validation result
    │
    ▼
presentDisplay(display, outPresentFence)
    │
    ├── Execute composition
    ├── Post to display
    └── Return: present fence
```

**Key characteristics:**
- Layer lifecycle managed explicitly
- Properties set individually
- Validation phase separate from presentation
- Changes must be explicitly accepted

## Fence Semantics Deep Dive

### HWC1 Fence Handling ("Relaxed")

**Acquire Fence:**
```c
// In layer structure
hwc_layer_t layer;
layer.acquireFenceFd = fenceFd;  // Set by SurfaceFlinger

// In HWC1 driver
int fd = layer.acquireFenceFd;
layer.acquireFenceFd = -1;       // "Steal" the fd
// Pass fd to kernel as pre_syncpt_fd
```

**Release Fence:**
```c
// Single retire fence from kernel
int retireFd = args.post_syncpt_fd;

// Duplicated for each layer
for each layer:
    layer.releaseFenceFd = nvsync_dup("release", retireFd);
```

**Characteristics:**
- Ownership transfer is implicit
- FD is zeroed in layer structure after taking
- Single retire fence duplicated per layer
- If layer unchanged, fence simply closed via `nvsync_close()`

### HWC2 Fence Handling ("Strict")

**Acquire Fence:**
```c
// Passed via setLayerBuffer
setLayerBuffer(layerHandle, buffer, acquireFenceFd);

// HAL MUST consume this fd (wait + close)
// Cannot "steal" without tracking
```

**Release Fence:**
```c
// Must be unique per layer
setReleaseFences(numFences, layerHandles[], fenceFds[]);

// Each fenceFd must be a unique fd (even if same sync object)
```

**Present Fence:**
```c
// Returned after presentDisplay
presentDisplay(display, &presentFenceFd);

// Signaled when frame is on screen
```

**Characteristics:**
- Every `acquireFenceFd` MUST be consumed (waited on + closed)
- Every `releaseFenceFd` MUST be unique per layer
- `presentFence` MUST be returned after `presentDisplay()`
- If fence not consumed, fd leak accumulates
- Ownership transfer is explicit and documented

## Why hwc2on1 Adapter Breaks with NVIDIA HWC1

### Issue 1: FD Theft Tracking

**Problem:**
NVIDIA HWC1 "steals" the fd from layer struct (zeros `acquireFenceFd`). The hwc2on1 adapter doesn't always track this ownership transfer correctly.

**Result:**
Adapter may attempt to close an already-stolen fd, or leak the fd if it doesn't know it was taken.

### Issue 2: Framebuffer Cache Reuse

**Problem:**
When framebuffer layers are recycled, NVIDIA HWC1 closes fences internally via `nvsync_close()`. The adapter may not be aware of this internal closure.

**Result:**
Adapter tries to close the same fd again → double-close or use-after-close crash.

### Issue 3: Retire Fence Duplication

**Problem:**
Retire fence is one per display. The adapter must duplicate it for per-layer release fences. Each `nvsync_dup()` creates a new fd.

**Code path:**
```c
// In adapter
for (size_t i = 0; i < numLayers; i++) {
    releaseFences[i] = dup(retireFence);
    // Each dup creates new fd
}
```

**Result:**
If adapter doesn't track all duplicated fds for cleanup, they accumulate and leak.

### Issue 4: Accumulation

**Problem:**
Each composition frame may leak 1-3 file descriptors. With 60fps video or rapid UI updates, fd leaks accumulate quickly.

**Result:**
Process hits file descriptor limit (typically 1024). SurfaceFlinger crashes. System becomes unstable.

## Mapping HWC1 to HWC2 Composition Types

| HWC1 Type | HWC2 Type | Notes |
|-----------|-----------|-------|
| `HWC_OVERLAY` | `DEVICE` | Hardware composition |
| `HWC_FRAMEBUFFER` | `CLIENT` | GPU composition via SurfaceFlinger |
| `HWC_FRAMEBUFFER_TARGET` | `CLIENT_TARGET` | Explicit client target buffer |
| N/A | `SOLID_COLOR` | Solid color layer (no buffer) |
| N/A | `CURSOR` | Hardware cursor layer |
| N/A | `SIDEBAND` | Sideband stream (video) |

## Error Handling Differences

### HWC1
- Errors typically returned as negative values from `prepare()`/`set()`
- Limited error context
- SurfaceFlinger may retry or fall back to GPU composition

### HWC2
- Errors returned via `Error` enum
- Validation phase can reject configurations before presentation
- `BAD_LAYER` / `BAD_DISPLAY` / `BAD_PARAMETER` for specific issues
- SurfaceFlinger can query and adapt before presentation

## Thread Safety

### HWC1
- `prepare()` and `set()` called from SurfaceFlinger thread
- No explicit threading model defined
- Implementation typically single-threaded

### HWC2
- Explicit threading guarantees in specification
- `validateDisplay()` and `presentDisplay()` must be thread-safe
- Callbacks may be invoked from different threads
- Layer state must be protected

## Migration Checklist

- [ ] Implement `getFunction()` table
- [ ] Implement layer lifecycle (`createLayer`/`destroyLayer`)
- [ ] Implement property setters
- [ ] Implement `validateDisplay()` with proper error handling
- [ ] Implement `presentDisplay()` with fence handling
- [ ] Ensure strict fence semantics (consume acquire, unique release)
- [ ] Implement hotplug callback
- [ ] Implement vsync callback
- [ ] Test with `lsof -p <surfaceflinger_pid>` for fd leaks
- [ ] Test with `dumpsys SurfaceFlinger` for composition stats

## Conclusion

The transition from HWC1 to HWC2 represents a significant architectural shift from implicit batch processing to explicit state management. The strict fence semantics in HWC2, while more complex to implement, eliminate the ambiguity that causes the fd leak issues in the hwc2on1 adapter. A native HWC2 implementation provides the control necessary for correct fence lifecycle management on Tegra K1.
