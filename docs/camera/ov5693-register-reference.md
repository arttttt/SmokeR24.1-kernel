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

## Mode Table Summary

Each mode table programs the sensor from scratch (all registers from standby).
Tables end with "Mocha 1-lane CSI-E overrides" that adapt 2-lane reference
settings for the Mi Pad's 1-lane CSI-E connection.

### Available Modes

| Mode | Resolution | FPS | HTS | VTS | Binning | Notes |
|------|-----------|-----|-----|-----|---------|-------|
| 2592x1944 | Full 5MP | 30 | varies | 0x07c0 | No | Full resolution |
| 2592x1458 | 16:9 crop | 30 | varies | 0x07c0 | No | Cropped for video |
| 1920x1080 | 1080p | 30 | varies | 0x07c0 | No | Center crop |
| 1280x720 | 720p | 60 | 0x1500 | 0x02f8 | 2x | Xiaomi stock |
| 1280x720 | 720p | 90 | 0x0e00 | 0x02f8 | 2x | Experimental |
| 1280x720 | 720p | 120 | 0x0a80 | 0x02f8 | 2x | Experimental (orig PLL) |
| 1280x720 | 720p | 120 | 0x0a80 | 0x02f8 | 2x | Experimental (safe PLL) |
| 2592x1944 HDR | Full 5MP | 24 | varies | 0x07c0 | No | HDR mode |
| 1920x1080 HDR | 1080p | 30 | varies | varies | No | HDR mode |
| 1280x720 HDR | 720p | 60 | varies | varies | 2x | HDR mode |

### Frame Rate Formula

```
fps = pixel_clock / (HTS × VTS)
```

Where pixel_clock is derived from XVCLK (24MHz) through the PLL chain.

### Mocha 1-Lane Overrides

Each mode table ends with overrides that adapt the 2-lane reference to 1-lane:

| Register | 2-lane (orig) | 1-lane (mocha) | Purpose |
|----------|--------------|----------------|---------|
| 0x3011 | 0x21 | 0x11 | MIPI lane config: 1 lane |
| 0x3015 | 0x08 | 0x28 | PLL divider (lower pixel clock) |
| 0x380c/d | mode-specific | increased | HTS: more H-blanking to reduce data rate |

### Why Xiaomi Limited 720p to 60fps

The reference NVIDIA driver supports 720p@120fps on 2 CSI lanes. Xiaomi reduced
this to 60fps by tripling HTS (1752 → 5376), which cuts frame rate proportionally.

The CSI-E 1-lane bandwidth can theoretically handle 120fps (~1106 Mbps/lane, at the
~1 Gbps D-PHY 1.2 spec limit), so the limitation was likely due to:
- PLL configuration constraints with the 1-lane divider
- ISP pipeline or DRAM bandwidth concerns
- Power/thermal considerations
- Development time (Android 4.4, never updated)

### Experimental 90/120fps Modes

Added as experiments to test the hardware limits:

**90fps** (safe): Same mocha PLL, HTS reduced from 5376 to 3584.
~830 Mbps/lane — near spec limit.

**120fps Option B** (aggressive): Original 2-lane PLL (0x3015=0x08), 1-lane MIPI
(0x3011=0x11), HTS=2688. Uses original MIPI timing values (0x4826=0x32, 0x4831=0x6a).
Higher risk — may not work if PLL can't sustain data rate on 1 lane.

**120fps Option A** (safe PLL): Mocha PLL (0x3015=0x28), HTS=2688. Lower risk but
may not actually reach 120fps if pixel clock is insufficient.

**Safety note**: No risk of physical damage. Worst case: corrupted frames, MIPI
timeout, or no stream. Sensor has internal PLL lock protection.

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
