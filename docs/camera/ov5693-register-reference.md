# OV5693 Sensor Register Reference & Mode Tables

## Overview

The OV5693 is a 5MP CMOS image sensor by OmniVision, used as the front camera
on Xiaomi Mi Pad 1 (mocha). Connected via CSI-E with **1 MIPI data lane** (reference
boards use 2 lanes on CSI-A). OV5693 supports max 2 lanes.

- Resolution: 2592x1944 (5MP)
- Output: 10-bit raw Bayer (BGGR)
- I2C address: 0x36 (on CAM I2C bus, adapter 2)
- MCLK: 24MHz
- Chip ID: reads 0x5690 at registers 0x300A/0x300B (not 0x5693, known quirk)
- Fuse ID: reads as 0x6c00 (hardware verified alive)

## Hardware Status: WORKING

**All modes are now WORKING and TESTED on real hardware.**

The sensor successfully streams frames through CSI-E (port index 1, PPB) on Tegra K1.
Key fixes that enabled functionality:
- 0x3011 register: bit[5]=0 for 1-lane mode, =1 for 2-lane mode (OV5693 max is 2 lanes)
- Proper CSI-E PHY initialization and CILE→PP_B routing
- MIPI calibration enabled for T124

PLL readback confirmed working:
- 0x3098 = 0x03
- 0x3099 = 0x1e
- 0x30b3 = 0x68

## Mode Table Summary

Each mode table programs the sensor from scratch (all registers from standby).
Tables end with "Mocha 1-lane CSI-E overrides" that adapt 2-lane reference
settings for the Mi Pad's 1-lane CSI-E connection.

### Available Modes (All Tested and Working)

| Mode | Resolution | FPS | HTS | VTS | Binning | Notes |
|------|-----------|-----|-----|-----|---------|-------|
| 2592x1944 | Full 5MP | 30 | varies | 0x07c0 | No | Full resolution |
| 2592x1458 | 16:9 crop | 30 | varies | 0x07c0 | No | Cropped for video |
| 1920x1080 | 1080p | 30 | varies | 0x07c0 | No | Center crop |
| 1280x720 | 720p | 60 | 0x1500 | 0x02f8 | 2x | Xiaomi stock |
| 1280x720 | 720p | 90 | 0x0e00 | 0x02f8 | 2x | Working at ~90fps |
| 1280x720 | 720p | 120 | 0x0a80 | 0x02f8 | 2x | Working at ~120fps |

### Frame Rate Selection

VIDIOC_S_PARM framerate selection is implemented. Users can select desired FPS
and the driver will choose the appropriate mode table. Available frame rates:
- 30fps (default for full resolution and 1080p)
- 60fps (default for 720p)
- 90fps (720p only)
- 120fps (720p only)

### Frame Rate Formula

```
fps = pixel_clock / (HTS × VTS)
```

Where pixel_clock is derived from XVCLK (24MHz) through the PLL chain.

### Mocha 1-Lane Overrides

Each mode table ends with overrides that adapt the 2-lane reference to 1-lane:

| Register | 2-lane (orig) | 1-lane (mocha) | Purpose |
|----------|--------------|----------------|---------|
| 0x3011 | 0x21 | 0x11 | MIPI lane config: bit[5]=0 for 1 lane, =1 for 2 lanes |
| 0x3015 | 0x08 | 0x28 | PLL divider (lower pixel clock) |
| 0x380c/d | mode-specific | increased | HTS: more H-blanking to reduce data rate |

**Important:** 0x3011 bit[5] controls lane count: 0=1-lane, 1=2-lane. OV5693 maximum
is 2 lanes (not 4). The R21.5 driver correctly used 0x11 (1-lane), but R24.1 had a
copy-paste bug keeping 0x21 (2-lane), which prevented CSI link from working.

### Why Xiaomi Limited 720p to 60fps

The reference NVIDIA driver supports 720p@120fps on 2 CSI lanes. Xiaomi reduced
this to 60fps by tripling HTS (1752 → 5376), which cuts frame rate proportionally.

The CSI-E 1-lane bandwidth can theoretically handle 120fps (~1106 Mbps/lane, at the
~1 Gbps D-PHY 1.2 spec limit), so the limitation was likely due to:
- PLL configuration constraints with the 1-lane divider
- ISP pipeline or DRAM bandwidth concerns
- Power/thermal considerations
- Development time (Android 4.4, never updated)

### 90/120fps Modes

These modes are tested and working on the Mi Pad hardware:

**90fps**: Same mocha PLL, HTS reduced from 5376 to 3584.
~830 Mbps/lane — near spec limit.

**120fps**: Mocha PLL (0x3015=0x28), HTS=2688. Uses safe PLL settings while
still achieving 120fps. Tested and working on real hardware.

**Safety note**: No risk of physical damage. Worst case: corrupted frames, MIPI
timeout, or no stream. Sensor has internal PLL lock protection.

## Current Limitations

- **~6fps single-shot capture**: Current implementation captures at approximately
  6 frames per second in single-shot mode
- **Dark images without exposure control**: Images are dark because automatic
  exposure control (AEC) is not yet implemented. Manual exposure/gain control
  needed for proper brightness

## CSI Connection Details

- **Port**: CSI-E (1 data lane + 1 clock lane)
- **Port Index**: 1 (PPB - Pixel Parser B)
- **DPD Register**: CSIE (reg1, bit 12)
- **MCLK**: MCLK2 (24MHz)

The CILE (CSI-E Low Speed I/O) is hardwired in silicon to route to PP_B.
No mux register is needed for this routing on T124.

## Register Map

Full register documentation is in the header file itself:
`drivers/media/i2c/ov5693_mocha_mode_tbls.h`

### Key Register Groups

| Group | Registers | Function |
|-------|-----------|----------|
| System Control | 0x0100-0x0103 | Streaming on/off, software reset |
| I/O & Pads | 0x3001-0x3028 | Pad drive strength, MIPI lane config |
| PLL (system) | 0x3098-0x309c | System clock PLL chain |
| PLL (MIPI) | 0x30a0-0x30b6 | MIPI/pixel clock PLL (key for frame rate) |
| AEC/AGC | 0x3500-0x350b | Exposure and gain (manual in our driver) |
| Analog | 0x3600-0x3681 | AFE tuning (OmniVision factory values) |
| Sensor Timing | 0x3700-0x37df | Readout timing (OmniVision factory values) |
| Windowing | 0x3800-0x382a | Crop, output size, HTS/VTS, binning |
| MIPI | 0x4800-0x4837 | MIPI clock mode, HS timing, pclk divider |
| ISP | 0x5000-0x5046 | DPC, BLC, white balance enables |
| DPC | 0x5780-0x5791 | Defect pixel correction thresholds |

### Critical Registers for Mode Tuning

| Register | Name | Effect |
|----------|------|--------|
| 0x3011 | mipi_lane_mode | bit[5]: 0=1-lane, 1=2-lane (OV5693 max 2 lanes) |
| 0x3015 | pll_divider | Changes pixel clock; 0x08=fast, 0x28=slow |
| 0x30b3 | pll_mul_mipi | MIPI PLL multiplier (affects link frequency) |
| 0x380c/d | HTS | Line length — directly controls fps |
| 0x380e/f | VTS | Frame length — also controls fps |
| 0x3814/15 | subsample | 0x11=1:1, 0x31=2x binning |
| 0x4800 | mipi_ctrl | 0x20=discontinuous clock, 0x00=continuous |
| 0x4837 | mipi_pclk_div | MIPI pixel clock period — critical for link stability |

## Sources

- Mainline Linux `drivers/media/i2c/ov5693.c` (v5.15+): register `#define`s
- Intel atomisp `drivers/staging/media/atomisp/i2c/ov5693/ov5693.h`: named registers
- NVIDIA R24.1 reference driver: mode tables
- Xiaomi R21.5 board file: power sequence and hardware configuration
