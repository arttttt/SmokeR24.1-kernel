# Tegra K1 Composition Engines — 2D, VIC, GL, and What's Lost Without Them

## Overview

This document describes the composition engines available on the Tegra K1 platform, their capabilities, selection logic, and the consequences of operating without the proprietary 2D and VIC engines.

## Compositor Priority Order

The HWC implementation selects compositors in the following priority order (from `nvhwc.c` lines 3263-3266):

```c
dev->compositors[0] = HWC_Compositor_GLComposer;      // EGL/GLES composition
dev->compositors[1] = HWC_Compositor_GLDrawTexture;   // GL texture draw path
dev->compositors[2] = HWC_Compositor_Gralloc;         // VIC/2D fallback
```

## Compositor Capabilities

Each compositor advertises capabilities via flags defined in `nvcomposer.h`:

```c
#define NVCOMPOSER_CAP_PROTECT      (1<<1)  // Can access VPR memory
#define NVCOMPOSER_CAP_DECOMPRESS   (1<<2)  // Can read compressed buffers
#define NVCOMPOSER_CAP_PITCH_LINEAR (1<<3)  // Can write to pitch linear layout
```

### GLComposer Capabilities
- **No VPR access**: Cannot read from protected memory
- **No decompression**: Cannot read compressed buffers directly
- **Pitch linear output**: Can write to standard linear buffers

### Gralloc (VIC/2D) Capabilities
- **VPR access**: Can read from Video Protected Region
- **Decompression**: Can read and decompress lossless compressed buffers
- **Pitch linear output**: Can write to standard linear buffers

## When Is Each Compositor Used

### GLComposer (Default)

**Used for:**
- Most UI composition
- Standard RGBA layers
- Non-protected content

**Characteristics:**
- Uses EGL/GLES for composition
- Full shader flexibility
- Cannot access VPR (protected content)
- Cannot read compressed buffers

### GLDrawTexture

**Used for:**
- Alternative GL path using texture draws
- Fallback when GLComposer fails

**Characteristics:**
- Simpler than full GLComposer
- Similar limitations (no VPR, no decompression)

### Gralloc (VIC/2D)

**Used for:**
- Protected content (VPR memory)
- Buffer decompression
- Format conversion
- Fallback when GL composers fail

**Selection Logic** (from `nvhwc_composite.c` lines 290-333):

```c
// Try preferred compositor first
compositor = dev->compositors[0];
result = compositor->composite(layers);

// If it fails, iterate through fallback chain
if (result != SUCCESS) {
    for (i = 1; i < num_compositors; i++) {
        compositor = dev->compositors[i];
        result = compositor->composite(layers);
        if (result == SUCCESS) break;
    }
}

// Special override for protected content
if (any_layer_has_protected_content &&
    !(compositor->caps & NVCOMPOSER_CAP_PROTECT)) {
    // Force Gralloc compositor
    compositor = HWC_Compositor_Gralloc;
}
```

## Scratch Buffer System

The scratch buffer system handles operations that cannot be performed directly by the display controller.

### When Scratch Blit Is Needed

- **Scaling beyond DC limits**: When scaling exceeds hardware window capabilities
- **90-degree rotation**: Display controller only supports 180-degree rotation
- **Format/layout conversion**: Converting between surface layouts

### VIC vs GR2D Alignment

```c
// From nvgr_scratch.c
if (sm->ctx->HaveVic) {
    // VIC: no alignment restrictions for rotation
    // Can rotate any buffer size efficiently
} else {
    // GR2D: requires 128-bit alignment
    // Extra padding required for YUV formats
}
```

### VIC (Video Image Composer)

- Advanced composition engine
- No alignment restrictions
- Supports rotation, scaling, format conversion
- Can handle YUV efficiently

### GR2D (2D Graphics Engine)

- Legacy 2D blit engine
- Requires 128-bit alignment
- Additional padding for YUV formats
- More limited than VIC

## What Is Lost Without 2D/VIC Engines

| Feature | Without 2D/VIC | Severity |
|---------|---------------|----------|
| **Protected Content (DRM)** | Cannot access VPR memory | **FATAL** — no Netflix, no HDCP video |
| **Hardware Decompression** | Explicit decompress needed, which itself uses 2D/VIC internally | **CIRCULAR DEPENDENCY** |
| **YUV Video Processing** | GPU shader conversion required | **HIGH** — poor video perf, higher power |
| **Idle Composition** | GPU must wake up for cached frames | **MEDIUM** — 15-25% higher idle power |
| **DIDIM** | No video-aware backlight dimming | **MEDIUM** — higher backlight power |
| **Pitch Linear Output** | Cannot write to linear buffers | **MEDIUM** — camera/video compat issues |
| **Rotation Without Padding** | GPU has own alignment restrictions | **LOW** — small memory waste |

### Detailed Impact Analysis

#### Protected Content (FATAL)

DRM-protected video (Netflix, Amazon Prime, etc.) requires:
1. Decryption to VPR (Video Protected Region) memory
2. Composition directly from VPR
3. HDCP-encrypted output to display

**Without 2D/VIC:**
- GPU cannot access VPR (security restriction)
- No path from VPR to display
- DRM video simply cannot play

#### Hardware Decompression (CIRCULAR DEPENDENCY)

Lossless compression saves memory bandwidth:
- Blocklinear layout with compression
- Decompression required before display

**Without 2D/VIC:**
- Explicit decompression via `decompress_buffer()` API
- This API internally uses 2D/VIC engines
- Creates a circular dependency

#### YUV Video Processing (HIGH)

Video content typically arrives in YUV format:
- YUV 4:2:0 (NV12) most common
- Display controller supports YUV directly
- But composition may require conversion

**Without 2D/VIC:**
- GPU shader conversion required
- Higher power consumption
- Potential quality issues

#### Idle Composition (MEDIUM)

When screen content is static:
- HWC can cache composed frame
- No GPU wake needed for identical frames

**Without 2D/VIC:**
- GPU must wake for any composition
- 15-25% higher idle power consumption
- Reduced battery life

#### DIDIM (MEDIUM)

Dynamic Intelligent Display Illumination Management:
- Detects video layers (YUV covering >50% screen)
- Adjusts backlight based on content

**Versions:**
- **DIDIM1**: Adjusts backlight aggressiveness based on content type
- **DIDIM2**: Creates window around video for localized dimming

**Without 2D/VIC:**
- No video layer detection
- Backlight stays brighter than necessary
- Higher power consumption

## What Is NOT Lost

Despite losing 2D/VIC, the following capabilities remain:

### Basic Overlay Composition
- Display Controller handles 3-6 hardware windows independently
- Direct layer-to-window assignment
- No composition engine needed for simple overlays

### Regular UI Rendering
- GPU handles standard UI rendering
- GLES composition works normally
- Most apps unaffected

### Display Window Scaling
- T124 DC supports hardware scaling per window
- No external scaler needed

### Alpha Blending
- DC supports premultiplied and coverage blending
- Per-window alpha in hardware

## Bandwidth Negotiation

The composition engine participates in bandwidth management:

```c
// From nvfb.c
nvfb_reserve_bw(display, config);
    └── TEGRA_DC_EXT_SET_PROPOSED_BW ioctl
```

**Purpose:**
- Probes display configurations
- Reserves memory bandwidth
- Prevents display underflows

**Without hardware composition:**
- Bandwidth estimation may fail
- Potential frame drops
- Display underflows possible

## Proprietary Libraries

The following proprietary libraries provide 2D/VIC functionality:

| Library | Purpose |
|---------|---------|
| `libnvddk_2d_v2.so` | 2D engine driver |
| `libnvddk_vic.so` | VIC engine driver |
| `libnvblit.so` | Blit operations |

**Static libraries:**
- `libnvgr2dcomposer.a` — 2D composer implementation
- `libnvviccomposer.a` — VIC composer implementation

## Conclusion

The 2D/VIC composition engines are **NOT optional** for a fully functional Tegra K1 system. They provide:

1. **DRM content path** — Essential for protected video playback
2. **Hardware decompression** — Required for compressed buffer handling
3. **Power efficiency** — Significant idle power savings
4. **Video optimization** — YUV processing and DIDIM

While basic display functionality is possible without these engines (via GPU composition), the user experience is severely degraded:
- No DRM video playback (Netflix, etc.)
- Higher power consumption
- Potential performance issues with video content

Any HWC2 implementation for Tegra K1 must account for these dependencies or provide alternative implementations for the critical functionality.
