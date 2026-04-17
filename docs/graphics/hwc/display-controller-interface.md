# Tegra K1 Display Controller Kernel Interface Reference

## Overview

This document provides a comprehensive reference for the Tegra K1 Display Controller kernel interface, as defined in the R24.1 kernel headers. This interface is used by the HWC HAL to communicate with the display hardware.

## Device Nodes

| Device Node | Purpose |
|-------------|---------|
| `/dev/tegra_dc_ctrl` | Control device for display enumeration and global operations |
| `/dev/tegra_dc_0` | Primary display controller (panel/DSI) |
| `/dev/tegra_dc_1` | Secondary display controller (HDMI) |
| `/dev/graphics/fb0` or `/dev/fb0` | Framebuffer device (primary) |
| `/dev/graphics/fb1` or `/dev/fb1` | Framebuffer device (secondary) |

## IOCTL Reference

### Display Device IOCTLs ('D' Magic: 0xD3)

| IOCTL | Code | Direction | Structure | Purpose |
|-------|------|-----------|-----------|---------|
| `TEGRA_DC_EXT_SET_NVMAP_FD` | 0x00 | IOW | `__s32` | Set nvmap fd for display device |
| `TEGRA_DC_EXT_GET_WINDOW` | 0x01 | IOW | `__u32` | Acquire display window |
| `TEGRA_DC_EXT_PUT_WINDOW` | 0x02 | IOW | `__u32` | Release display window |
| `TEGRA_DC_EXT_FLIP` | 0x03 | IOWR | `tegra_dc_ext_flip` | Legacy flip (3 windows embedded) |
| `TEGRA_DC_EXT_GET_CURSOR` | 0x04 | IO | — | Acquire cursor |
| `TEGRA_DC_EXT_PUT_CURSOR` | 0x05 | IO | — | Release cursor |
| `TEGRA_DC_EXT_SET_CURSOR_IMAGE` | 0x06 | IOW | `tegra_dc_ext_cursor_image` | Set cursor bitmap |
| `TEGRA_DC_EXT_SET_CURSOR` | 0x07 | IOW | `tegra_dc_ext_cursor` | Set cursor position/visibility |
| `TEGRA_DC_EXT_SET_CSC` | 0x08 | IOW | `tegra_dc_ext_csc` | Color space conversion matrix |
| `TEGRA_DC_EXT_GET_VBLANK_SYNCPT` | 0x09 | IOR | `__u32` | Get vblank syncpoint ID |
| `TEGRA_DC_EXT_SET_LUT` | 0x0A | IOW | `tegra_dc_ext_lut` | Set lookup table (gamma) |
| `TEGRA_DC_EXT_GET_FEATURES` | 0x0B | IOW | `tegra_dc_ext_feature` | Query display features |
| `TEGRA_DC_EXT_CURSOR_CLIP` | 0x0C | IOW | `__s32` | Set cursor clipping |
| `TEGRA_DC_EXT_SET_CMU` | 0x0D | IOW | `tegra_dc_ext_cmu` | Set color management unit |
| `TEGRA_DC_EXT_FLIP2` | 0x0E | IOWR | `tegra_dc_ext_flip_2` | Extended flip (variable windows) |
| `TEGRA_DC_EXT_SET_PROPOSED_BW` | 0x13 | IOR | `tegra_dc_ext_flip_2` | Bandwidth reservation probe |
| `TEGRA_DC_EXT_FLIP3` | 0x14 | IOWR | `tegra_dc_ext_flip_3` | Modern flip with sync fence fd |

### R24.1-Specific Additions

| IOCTL | Code | Direction | Structure | Purpose |
|-------|------|-----------|-----------|---------|
| `TEGRA_DC_EXT_SET_VBLANK` | 0x15 | IOW | `tegra_dc_ext_set_vblank` | Vblank control |
| `TEGRA_DC_EXT_SET_CSC_V2` | 0x17 | IOW | `tegra_dc_ext_csc_v2` | Extended CSC (4x3 matrix) |
| `TEGRA_DC_EXT_SET_CMU_V2` | 0x18 | IOW | `tegra_dc_ext_cmu_v2` | Extended CMU |
| `TEGRA_DC_EXT_FLIP4` | 0x1D | IOW | `tegra_dc_ext_flip_4` | Flip with HDR/user data |
| `TEGRA_DC_EXT_GET_CAP_INFO` | 0x1E | IOW | `tegra_dc_ext_get_cap_info` | Capability query |

### Control Device IOCTLs ('C' Magic: 0xC3)

| IOCTL | Code | Direction | Structure | Purpose |
|-------|------|-----------|-----------|---------|
| `TEGRA_DC_EXT_CONTROL_GET_NUM_OUTPUTS` | 0x00 | IOR | `__u32` | Number of display outputs |
| `TEGRA_DC_EXT_CONTROL_GET_OUTPUT_PROPERTIES` | 0x01 | IOWR | `tegra_dc_ext_control_output_properties` | Output info |
| `TEGRA_DC_EXT_CONTROL_GET_OUTPUT_EDID` | 0x02 | IOWR | `tegra_dc_ext_control_output_edid` | EDID data |
| `TEGRA_DC_EXT_CONTROL_SET_EVENT_MASK` | 0x03 | IOW | `__u32` | Event notification mask |

## Key Structures

### struct tegra_dc_ext_flip_windowattr (R24.1 Version)

```c
struct tegra_dc_ext_flip_windowattr {
    __s32 index;                    // Window index
    __u32 buff_id;                  // Buffer handle (nvmap)
    __u32 blend;                    // TEGRA_DC_EXT_BLEND_* constant
    __u32 offset;                   // Surface offset in bytes
    __u32 offset_u;                 // U plane offset (YUV formats)
    __u32 offset_v;                 // V plane offset (YUV formats)
    __u32 stride;                   // Pitch in bytes
    __u32 stride_uv;                // UV plane pitch
    __u32 pixformat;                // TEGRA_FB_WIN_FMT_* format
    __u32 x;                        // Source X (20.12 fixed-point)
    __u32 y;                        // Source Y (20.12 fixed-point)
    __u32 w;                        // Source width (20.12 fixed-point)
    __u32 h;                        // Source height (20.12 fixed-point)
    __u32 out_x;                    // Destination X
    __u32 out_y;                    // Destination Y
    __u32 out_w;                    // Destination width
    __u32 out_h;                    // Destination height
    __u32 z;                        // Z-order
    __u32 swap_interval;            // Swap interval for vsync
    struct tegra_timespec timestamp; // R24.1: { __u32 tv_sec, tv_nsec }
    union {
        struct {
            __u32 pre_syncpt_id;
            __u32 pre_syncpt_val;
        };
        __s32 pre_syncpt_fd;        // Acquire fence fd (preferred)
    };
    __u32 flags;                    // TEGRA_DC_EXT_FLIP_FLAG_*
    __u8 global_alpha;              // Global alpha value
    __u8 block_height_log2;         // Block linear parameter
    // Union for interlace/CDE/CSC data follows
};
```

### struct tegra_dc_ext_flip_3 (R24.1 Version)

```c
struct tegra_dc_ext_flip_3 {
    __u64 win;                      // Pointer to windowattr array
    __u8 win_num;                   // Number of windows
    __u8 flags;                     // R24.1: was reserved1
    __s32 post_syncpt_fd;           // Output: present/retire fence fd
    __u16 dirty_rect[4];            // R24.1: partial update rect [x,y,w,h]
};
```

### struct tegra_timespec (R24.1)

```c
struct tegra_timespec {
    __u32 tv_sec;                   // Seconds
    __u32 tv_nsec;                  // Nanoseconds
};
```

Note: This replaces the standard `struct timespec` to ensure fixed 32-bit sizes.

## Pixel Formats

The following pixel formats are supported by the Tegra K1 display controller:

| Format | Description |
|--------|-------------|
| `TEGRA_FB_WIN_FMT_B8G8R8A8` | 32-bit BGRA (8888) |
| `TEGRA_FB_WIN_FMT_R8G8B8A8` | 32-bit RGBA (8888) |
| `TEGRA_FB_WIN_FMT_B5G6R5` | 16-bit RGB (565) |
| `TEGRA_FB_WIN_FMT_AB5G5R5` | 16-bit ABGR (1555) |
| `TEGRA_FB_WIN_FMT_B5G5R5A1` | 16-bit BGRA (5551) |
| `TEGRA_FB_WIN_FMT_R4G4B4A4` | 16-bit RGBA (4444) |
| `TEGRA_FB_WIN_FMT_A8R8G8B8` | 32-bit ARGB (8888) |
| `TEGRA_FB_WIN_FMT_B8G8R8X8` | 32-bit BGRX (8888, no alpha) |
| `TEGRA_FB_WIN_FMT_YCbCr420P` | YUV 4:2:0 planar (I420) |
| `TEGRA_FB_WIN_FMT_YCbCr422P` | YUV 4:2:2 planar |
| `TEGRA_FB_WIN_FMT_YCbCr422R` | YUV 4:2:2 reduced |
| `TEGRA_FB_WIN_FMT_YCrCb420SP` | YUV 4:2:0 semi-planar (NV21) |
| `TEGRA_FB_WIN_FMT_YCbCr420SP` | YUV 4:2:0 semi-planar (NV12) |
| `TEGRA_FB_WIN_FMT_YCbCr422SP` | YUV 4:2:2 semi-planar (NV16) |

## Blend Modes

```c
#define TEGRA_DC_EXT_BLEND_NONE         0  // No blending (opaque)
#define TEGRA_DC_EXT_BLEND_PREMULT      1  // Premultiplied alpha
#define TEGRA_DC_EXT_BLEND_COVERAGE     2  // Coverage alpha
#define TEGRA_DC_EXT_BLEND_ADD          3  // Additive blending
```

## Event Types (R24.1 - Bit Mask Format)

```c
#define TEGRA_DC_EXT_EVENT_HOTPLUG       (1 << 0)  // 0x1 - Display hotplug
#define TEGRA_DC_EXT_EVENT_VBLANK        (1 << 1)  // 0x2 - Vblank (new in R24.1)
#define TEGRA_DC_EXT_EVENT_BANDWIDTH_INC (1 << 2)  // 0x4 - Bandwidth increase
#define TEGRA_DC_EXT_EVENT_BANDWIDTH_DEC (1 << 3)  // 0x8 - Bandwidth decrease
```

**Important**: In R24.1, event types use bit masks rather than sequential values. The `BANDWIDTH_INC` event changed from `0x3` to `(1 << 2)`.

## Output Types

```c
#define TEGRA_DC_EXT_DSI    0  // MIPI DSI panel
#define TEGRA_DC_EXT_LVDS   1  // LVDS panel
#define TEGRA_DC_EXT_VGA    2  // VGA output
#define TEGRA_DC_EXT_HDMI   3  // HDMI output
#define TEGRA_DC_EXT_DVI    4  // DVI output
#define TEGRA_DC_EXT_DP     5  // DisplayPort
#define TEGRA_DC_EXT_EDP    6  // eDP (R24.1)
#define TEGRA_DC_EXT_NULL   7  // Null/display-off (R24.1)
```

## Flip Flags

```c
#define TEGRA_DC_EXT_FLIP_FLAG_CURSOR         (1 << 0)
#define TEGRA_DC_EXT_FLIP_FLAG_VID            (1 << 1)
#define TEGRA_DC_EXT_FLIP_FLAG_INTERLACE      (1 << 2)
#define TEGRA_DC_EXT_FLIP_FLAG_COMPRESSED     (1 << 3)
#define TEGRA_DC_EXT_FLIP_FLAG_INVERT_H       (1 << 4)
#define TEGRA_DC_EXT_FLIP_FLAG_INVERT_V       (1 << 5)
#define TEGRA_DC_EXT_FLIP_FLAG_TILED          (1 << 6)
#define TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR    (1 << 7)
```

## Usage Examples

### Basic Flip Operation

```c
struct tegra_dc_ext_flip_windowattr win[2];
struct tegra_dc_ext_flip_3 flip;

// Configure window 0
memset(&win[0], 0, sizeof(win[0]));
win[0].index = 0;
win[0].buff_id = buffer_handle;
win[0].pixformat = TEGRA_FB_WIN_FMT_B8G8R8A8;
win[0].x = 0; win[0].y = 0;
win[0].w = width << 12;  // 20.12 fixed-point
win[0].h = height << 12;
win[0].out_x = 0; win[0].out_y = 0;
win[0].out_w = width; win[0].out_h = height;
win[0].z = 0;
win[0].blend = TEGRA_DC_EXT_BLEND_PREMULT;
win[0].pre_syncpt_fd = acquire_fence_fd;

// Configure flip structure
flip.win = (__u64)(uintptr_t)win;
flip.win_num = 1;
flip.flags = 0;
flip.post_syncpt_fd = -1;

// Execute flip
ioctl(dc_fd, TEGRA_DC_EXT_FLIP3, &flip);

// retire_fence_fd now in flip.post_syncpt_fd
```

### Event Mask Setup

```c
__u32 event_mask = TEGRA_DC_EXT_EVENT_HOTPLUG |
                   TEGRA_DC_EXT_EVENT_VBLANK;

ioctl(ctrl_fd, TEGRA_DC_EXT_CONTROL_SET_EVENT_MASK, &event_mask);
```

## Notes

- All ioctl structures must be packed/aligned appropriately for 32/64-bit compatibility
- The `tegra_timespec` structure uses fixed-size fields to avoid ABI issues
- Fence file descriptors use the Android sync framework
- YUV formats require proper stride and offset configuration for multi-plane layouts
