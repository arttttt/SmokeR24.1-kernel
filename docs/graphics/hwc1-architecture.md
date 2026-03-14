# Tegra K1 HWC1 Architecture Analysis

## Overview

This document provides a comprehensive technical analysis of the NVIDIA Tegra K1 Hardware Composer HAL version 1.1 implementation. The HWC (Hardware Composer) HAL is a critical Android subsystem that determines how graphical layers are composed for display, balancing between hardware overlay planes and GPU framebuffer composition.

## HAL Version

**HWC 1.1** (`hwc_composer_device_1_t`)

From `nvhwc.c` line 3199:
```c
dev->device.common.version = HWC_DEVICE_API_VERSION_1_1;
```

## Source Location

```
/vendor_nvidia_jxd_src/tegra/graphics-partner/android/hwcomposer/
```

## Key Source Files

| File | Purpose | Lines |
|------|---------|-------|
| `nvhwc.c` | Main HWC implementation, prepare/set flow | ~3500 |
| `nvhwc.h` | Core structures (nvhwc_context, nvhwc_display, nvhwc_window_mapping) | - |
| `nvfb.c` | Framebuffer/display interface, kernel ioctl wrapper | - |
| `nvfb.h` | Display types, window capabilities, config structures | - |
| `nvfb_display.h` | Display mode structures | - |
| `nvhwc_composite.c/h` | Compositor selection and fallback logic | - |
| `nvhwc_external.c/h` | HDMI/external display handling | - |
| `nvhwc_debug.c/h` | Debug utilities | - |
| `nvhwc_props.c/h` | Property management | - |
| `nvfb_cursor.c/h` | Hardware cursor support | - |
| `nvfb_didim.c/h` | Dynamic Intelligent Display Illumination Management | - |
| `nvfb_hdcp.c/h` | HDCP for protected content | - |
| `nvfl.c/h` | Framebuffer layer management | - |

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                    HWC1 HAL Module                          │
│  ┌──────────────┐  ┌──────────────┐  ┌─────────────────┐   │
│  │   Primary    │  │   External   │  │   Framebuffer   │   │
│  │   Display    │  │   Display    │  │     Cache       │   │
│  └──────────────┘  └──────────────┘  └─────────────────┘   │
├─────────────────────────────────────────────────────────────┤
│                    Composition Engine                       │
│  ┌────────────┐ ┌────────────┐ ┌────────────┐ ┌─────────┐  │
│  │ SurfaceF   │ │  Gralloc   │ │ GLComposer │ │  GLDraw │  │
│  │ linger     │ │  (VIC/2D)  │ │  (EGL/GLES)│ │ Texture │  │
│  └────────────┘ └────────────┘ └────────────┘ └─────────┘  │
├─────────────────────────────────────────────────────────────┤
│                    Display Controller                       │
│         ┌─────────────────┐    ┌─────────────────┐          │
│         │   Panel (DSI)   │    │   HDMI (Tegra)  │          │
│         └─────────────────┘    └─────────────────┘          │
└─────────────────────────────────────────────────────────────┘
```

## Composition Flow

### Prepare Phase

The prepare phase analyzes layers and assigns them to hardware windows or framebuffer composition.

```
hwc_prepare()
    └── hwc_prepare_display()
            └── hwc_assign_windows()
```

**Key operations:**
1. Iterates layers back to front (highest Z-order first)
2. Assigns hardware windows based on window capabilities
3. Marks layers as `HWC_OVERLAY` (hardware) or `HWC_FRAMEBUFFER` (GPU)
4. Handles protected content priority
5. Detects cursor layers for hardware cursor optimization
6. Falls back to framebuffer composition when hardware constraints are exceeded

### Set Phase

The set phase executes the composition and posts to display.

```
hwc_set()
    └── hwc_set_display()
            └── nvfb_post()
                └── TEGRA_DC_EXT_FLIP3 ioctl
```

**Key operations:**
1. Decompresses buffers if needed (via VIC/2D engines)
2. Performs scratch buffer blit for transformations/scaling beyond DC limits
3. Generates acquire fences for buffer synchronization
4. Posts to display via `nvfb_post()` using `TEGRA_DC_EXT_FLIP3` ioctl
5. Returns retire fence from kernel via `args.post_syncpt_fd`

## Fence FD Management

The nvsync abstraction layer (`nvsync.h`) provides fence management primitives:

```c
// Core fence operations
int nvsync_wait(int fd, int timeout);
void nvsync_close(int fd);
int nvsync_dup(const char* name, int fd);
int nvsync_merge(const char* name, int fd1, int fd2);

// Fence <-> NvRmFence conversion
int nvsync_to_fence(int fd, NvRmFence* fences, int numFences);
int nvsync_from_fence(const char* name, NvRmFence* fences, int numFences);
```

### Fence Flow in HWC1

**In `hwc_set_buffer()`:**
- Extracts `acquireFenceFd` from layer structure
- Clears layer's fd (ownership transfer to kernel)
- Passes to kernel as `preFenceFd` in flip structure

**In `hwc_set_display()`:**
- Consumes all `acquireFenceFds`
- Closes unused fences for recycled framebuffer layers

**In `hwc_update_fences()`:**
- Tracks current and previous frame fences
- Duplicates retire fence for release fences per layer

## Key Data Structures

### nvhwc_context

The main HAL context structure:

```c
typedef struct nvhwc_context {
    hwc_composer_device_1_t device;    // HAL interface
    NvGrModule* gralloc;                // Gralloc module pointer
    nvfb_device* fb;                    // Framebuffer device
    nvhwc_display displays[HWC_NUM_DISPLAY_TYPES];
    // ... additional state
} nvhwc_context;
```

### nvhwc_display

Per-display state structure:

```c
typedef struct nvhwc_display {
    int type;                           // HWC_DISPLAY_PRIMARY/EXTERNAL
    nvfb_display_caps caps;             // Display capabilities
    nvfb_display_config config;         // Current configuration
    nvhwc_window_mapping window_mapping[MAX_WINDOWS];
    nvhwc_composite_state composite;    // Composition state
    nvhwc_fb_cache fb_cache[2];         // Framebuffer cache (double buffered)
    nvhwc_fences fences;                // Fence tracking
    // ... additional state
} nvhwc_display;
```

### nvfb_window

Window configuration for display controller:

```c
typedef struct nvfb_window {
    int window_index;                   // Hardware window index
    int blend;                          // Blend mode
    int transform;                      // Transform flags
    struct nvfb_rect src;               // Source rectangle
    struct nvfb_rect dst;               // Destination rectangle
    int planeAlpha;                     // Plane alpha value
    // ... additional fields
} nvfb_window;
```

## Display Capabilities by Chip

| Version | Chip | Windows | Sequential Blend | Rotation | Cursor |
|---------|------|---------|------------------|----------|--------|
| ver 2 | T114 | 3 | No | Yes | 256px |
| ver 3 | T148 | 5/4 | Yes | No | No |
| ver 4 | T124 | 4/3 | Yes | Yes | 256px |

Tegra K1 (T124) uses version 4 with 4 windows (3 when cursor enabled).

## Window Capabilities Flags

```c
#define NVFB_WINDOW_CAP_YUV_FORMATS         0x001
#define NVFB_WINDOW_CAP_SCALE               0x002
#define NVFB_WINDOW_CAP_SWAPXY_PITCH        0x004
#define NVFB_WINDOW_CAP_SWAPXY_TILED        0x008
#define NVFB_WINDOW_CAP_SWAPXY_TILED_PLANAR 0x010
#define NVFB_WINDOW_CAP_FLIPHV              0x020
#define NVFB_WINDOW_CAP_TILED               0x040
#define NVFB_WINDOW_CAP_SEQUENTIAL          0x080
```

## Linked Libraries

### Shared Libraries
- `libdl` - Dynamic linker
- `liblog` - Android logging
- `libcutils` - Android utilities
- `libsync` - Fence synchronization
- `libEGL` - EGL interface
- `libGLESv2` - OpenGL ES 2.0
- `libhardware` - Hardware abstraction
- `libui` - Android UI
- `libutils` - Android utilities
- `libnvblit` - NVIDIA blit operations
- `libnvddk_2d_v2` - 2D engine driver
- `libnvddk_vic` - VIC composition engine
- `libnvgr` - NVIDIA gralloc
- `libnvrm` - Resource manager
- `libnvrm_graphics` - Graphics resource manager
- `libnvos` - OS abstraction

### Static Libraries
- `libnvgr2dcomposer` - 2D composer implementation
- `libnvglcomposer` - GL composer implementation
- `libnvviccomposer` - VIC composer implementation
- `libnvfxmath` - Fixed-point math utilities

## Notes

The HWC1 implementation is tightly coupled with NVIDIA's proprietary gralloc and kernel display controller interface. Understanding this architecture is essential for porting to HWC2 or debugging composition issues.
