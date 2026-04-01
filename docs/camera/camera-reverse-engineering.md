# Mocha Camera Stack — Architecture & Bring-Up Guide

## Device: Xiaomi Mi Pad 1st Gen (mocha), Tegra K1 (T124)
## Cameras: IMX179 8MP rear (Primax) + OV5693 5MP front (Sunny)

---

## Status (April 2026)

| Component | Status | Notes |
|-----------|--------|-------|
| **OV5693 front camera** | **WORKING** | V4L2 RAW capture via MC framework, all modes working |
| **IMX179 rear camera** | **WORKING** | V4L2 RAW capture via MC framework, all modes working |
| **AD5823 focuser** | **WORKING** | VCM lens control via V4L2 lens channel |
| **ISP (demosaic, etc.)** | Not started | RAW only, no ISP processing |

**Both cameras are fully functional** via the Media Controller (MC) V4L2 framework.
- OV5693: Captures RAW Bayer (SBGGR10) at 5MP, 1080p, 720p@60/90/120fps
- IMX179: Captures RAW Bayer (SRGGB10) at 8MP, 1080p, 720p@90fps
- AD5823: Focuser control via /dev/video2 (lens channel, no CSI)
No proprietary blobs required.

---

## 1. Hardware Overview

### Cameras
| | Rear | Front |
|---|---|---|
| Sensor | Sony IMX179 | OmnVision OV5693 |
| Resolution | 3280x2464 (8MP) | 2592x1944 (5MP) |
| Bayer Pattern | RGGB | BGGR |
| CSI Port | CSI-A+B, 4 lanes | CSI-E, 1 lane |
| MCLK | 24 MHz (MCLK1) | 24 MHz (MCLK2) |
| PCLK | 348 MHz | — |
| Line Length | 3440 px | — |
| Modes | 8MP@30, 1080p@30, 720p@90 | 5MP@30, 1080p@30, 720p@60/90/120 |
| I2C Bus | CAM I2C (i2c@7000c500, adapter 2), addr 0x10 | CAM I2C (i2c@7000c500, adapter 2), addr 0x36 |
| Focuser | AD5823 VCM (macro=640, inf=140) | Fixed |
| Module | Primax | Sunny |
| CCM | [2.084 -0.413 -0.079; -1.102 1.817 -0.789; 0.017 -0.404 1.868] | See ov5693_sunny_v2.13.isp |

### AD5823 Focuser
- I2C address: 0x0C (on CAM I2C bus, adapter 2)
- VCM (Voice Coil Motor) range: 140 (infinity) to 640 (macro)
- Paired with IMX179 rear camera
- Power managed by capture channel (lens_chan association via DT phandle)
- GPIO 223 (CAM_AF_PWDN) owned by IMX179, enables focuser power

### SoC Camera/ISP Components
| Component | Physical Base | Size | Host1x Class | IRQ |
|-----------|--------------|------|--------------|-----|
| VI | 0x54080000 | 256KB | 0x30 | 69 |
| ISP A | 0x54600000 | 256KB | 0x32 | 71 |
| ISP B | 0x54680000 | 256KB | 0x34 | 70 |
| Host1X | 0x50000000 | 208KB | — | — |

### Data Flow (hardware level)
```
Sensor → MIPI CSI → VI (class 0x30) → DMA → [DRAM: RAW Bayer]
                                                     ↓
                                       ISP (class 0x32) DMA read
                                                     ↓
                                       ISP processing (demosaic, LSC, gamma, etc.)
                                                     ↓
                                       ISP DMA write → [DRAM: YUV/RGB]
```

**VI and ISP are separate host1x clients.** Data passes through DRAM between them.
They share the VENC power gate but have independent channels, syncpoints, and clocks.

### GPIOs
| GPIO | Pin | Function | Owner |
|------|-----|----------|-------|
| CAM_RSTN | PBB3 = 219 | IMX179 reset | IMX179 |
| CAM1_PWDN | PBB5 = 221 | IMX179 power down | IMX179 (not used in power seq) |
| CAM2_PWDN | PBB6 = 222 | OV5693 power down | OV5693 |
| CAM_AF_PWDN | PBB7 = 223 | AD5823 / IMX179 AF enable | IMX179 (focuser power) |
| CAM2_RSTN | PCC1 = 225 | OV5693 reset | OV5693 |

---

## 2. Kernel Driver Stack (SmokeR24.1)

### Active Framework: Media Controller + V4L2

```
drivers/media/platform/tegra/
├── camera/channel.c     ← V4L2 video device, vb2 buffer queues (T124 direct path)
├── camera/graph.c       ← Media controller graph setup, lens channel support
├── camera/mc_common.c   ← Media controller helpers
├── camera/core.c        ← Format definitions
├── camera/camera_common.c ← camera_common framework
├── vi/vi.c              ← VI driver with V4L2 integration
├── csi/csi.c            ← CSI transceiver (T124 CIL PHY init)

drivers/media/i2c/
├── imx179_mocha.c       ← IMX179 V4L2 subdev (mocha-specific, WORKING)
├── ov5693_mocha.c       ← OV5693 V4L2 subdev (mocha-specific, WORKING)
├── ad5823_mocha.c       ← AD5823 focuser V4L2 subdev (WORKING)
```

**This is the active framework for SmokeR24.1.** Both cameras working via MC V4L2.
First ever T124 platform using MC framework (previously only T210+).

### Legacy Frameworks (Historical Reference Only)

#### soc_camera + V4L2 (LEGACY)
```
drivers/media/platform/soc_camera/tegra_camera/
├── vi2.c           ← soc_camera host, compatible "nvidia,tegra124-vi"
├── common.c        ← Shared tegra_camera utilities

drivers/media/i2c/soc_camera/
├── ov5693_v4l2.c   ← OV5693 as V4L2 subdev (camera_common framework)
├── imx135_v4l2.c   ← IMX135 V4L2 subdev
```

**Historical only.** soc_camera was deprecated upstream. MC framework is used instead.

#### Legacy miscdevice Drivers (for blob stack)
```
drivers/media/platform/tegra/
├── imx179.c     ← IMX179 miscdevice (/dev/imx179), custom IOCTLs
├── ov5693.c     ← OV5693 miscdevice (/dev/ov5693), custom IOCTLs
├── ad5823.c     ← AD5823 focuser miscdevice
```

These legacy drivers are used by the proprietary blob stack (libnvodm_imager.so).
They are NOT V4L2 subdevs.

### ISP Kernel Driver
```
drivers/video/tegra/host/isp/
├── isp.c          ← ISP host1x client: probe, power on/off, bandwidth management
├── isp.h          ← ISP struct definition
├── isp_isr_v1.c   ← ISP interrupt handler (MFI callback)
├── isp_isr_v1.h
```

**Current state:** Only power management and host1x client registration.
No ISP register programming — that was always done from userspace (libnvisp_v3.so).

### Key Kernel Headers
| Header | Purpose |
|--------|---------|
| `include/media/tegra_v4l2_camera.h` | V4L2 camera platform data, CSI port enums |
| `include/media/camera_common.h` | camera_common framework (shared by V4L2 sensor drivers) |
| `include/media/imx179.h` | IMX179 IOCTL definitions (legacy, custom) |
| `include/media/ov5693.h` | OV5693 IOCTL definitions (legacy, custom) |
| `include/media/ad5823.h` | AD5823 IOCTL definitions (legacy) |
| `include/linux/nvhost_ioctl.h` | nvhost channel submit API |
| `include/linux/nvmap.h` | nvmap ↔ dma_buf memory management |

---

## 3. Proprietary Blob Stack (Xiaomi stock, reference only)

### Architecture (Xiaomi stock, KitKat 4.4)
```
Android CameraService
        ↓
camera.vendor.tegra.so         ← Camera HAL3 (1.1MB, Xiaomi custom + CUDA)
  - NvCameraHal3Core               SkinBeautifier, MIPreview
  - NvCameraHal2 (compat)
        ↓
libnvmm_camera_v3.so          ← NvMM Camera Pipeline (1.4MB)
  - ISP tuning, AE/AWB/AF
  - Capture scheduling
   ┌────┼─────────────────┐
   ↓    ↓                 ↓
libnvodm_imager.so    libnvisp_v3.so      libnvvicsi_v3.so
(2.4MB)               (50KB)               (17KB)
Sensor drivers +      ISP HW driver        VI/CSI HW
embedded ISP cal.     ↓                    ↓
↓                     /dev/nvhost-isp      /dev/nvhost-ctrl-vi
/dev/imx179           /dev/nvhost-ctrl-isp /dev/nvhost-ctrl-vi.1
/dev/ov5693                                /dev/mipi-cal
```

**Key difference from TN8 (Shield Tablet):**
- **No libnvscf.so** — Mocha does NOT use SCF (Sensor Control Framework)
- TN8: HAL → SCF → NvMM → sensors
- Mocha: HAL → NvMM → sensors (direct, simpler)

### Library Inventory (Mocha stock)

| Library | Size | Purpose |
|---------|------|---------|
| camera.vendor.tegra.so | 1.1MB | Camera HAL3 + Xiaomi CUDA features |
| libnvmm_camera_v3.so | 1.4MB | Main camera pipeline orchestrator |
| libnvodm_imager.so | 2.4MB | Sensor drivers + ISP calibration |
| libnvisp_v3.so | 49KB | ISP HW register programming |
| libnvvicsi_v3.so | 17KB | VI/CSI HW interface |
| libnvrm_graphics.so | 21KB | Host1x command buffer API |
| libnvcam_imageencoder.so | 37KB | JPEG encoding |
| libnvcamerahdr_v3.so | 310KB | HDR processing |
| libnvcameratools.so | 85KB | Camera debug/tuning |
| libnvtnr.so | 33KB | Temporal noise reduction |
| libfcamdng.so | 446KB | DNG/RAW file support |

---

## 4. Blob Stack Incompatibility Analysis (Historical Reference Only)

**This section is preserved for historical reference.** The working V4L2 MC framework
approach eliminates all blob dependencies. The blob path was analyzed and determined
to be a dead end — no combination of available blobs could produce a working stack on
SmokeR24.1.

### Three Available Blob Sources — None Directly Usable

| | Mocha (Xiaomi stock) | TN8 (Shield) | JXD S192 (vendor source) |
|---|---|---|---|
| Android | 4.4 KitKat | 5.x-7.x | 4.4 KitKat |
| GCC | 4.4.3 | 4.9 | 4.9 |
| ABI | Old | New | Old |
| libnvisp_v3.so | 49KB, 39 exports | 63KB, 56 exports | Source available (not buildable) |
| libnvodm_imager.so | 2.4MB, IMX179+OV5693 mocha cal. | ~5MB, 15+ sensors, no mocha cal. | Source available (not buildable) |
| ISP calibration | Mocha-specific (Primax/Sunny) | TN8-specific | JXD-specific |

**SmokeR24.1 kernel is TN8-derived** (new ABI, GCC 4.9).

### Why Each Option Fails

**Mocha blobs on SmokeR24.1:**
- Old ABI (GCC 4.4.3 vs 4.9) — struct layout and calling convention differences
- libnvisp_v3.so has 39 exports; TN8's libnvmm_camera expects 56
- No libnvscf.so, but TN8 pipeline may require it

**TN8 blobs:**
- Compatible ABI with kernel, but libnvodm_imager has no mocha-specific sensor
  support (IMX179 Primax module calibration)
- Different badge/PCL strings (tn8_a00_rear vs mocha PCL matching)

**JXD sources:**
- KitKat-era API level, cannot be compiled against TN8 headers
- TN8 headers (NvMM camera internal API, NvOdm imager API) are not publicly available
- Useful as **documentation and reference**, NOT as buildable code

### Conclusion

The blob-based camera path is a dead end for SmokeR24.1. No combination of available
blobs produces a working stack. This is why we pursue the **V4L2 + kernel ISP driver**
approach, which eliminates all blob dependencies.

---

## 5. Kernel API Compatibility (Old Mocha blobs vs New kernel)

This section is preserved for historical reference. It documents what would need
to change if someone attempted the blob path on an older kernel.

### Summary: Mostly compatible, 3 breaking changes

| Interface | Status | Details |
|-----------|--------|---------|
| Camera PCL ioctls (camera.h) | **COMPATIBLE** | Same numbers, new ones added |
| IMX179 sensor ioctls | **BREAKING** | Missing OTP ioctls (8,9), missing ext_reg3 |
| OV5693 sensor ioctls | **COMPATIBLE** | New ioctls added (22,23) |
| NVC framework (nvc.h) | **COMPATIBLE** | p_value size same on ARM32 |
| nvhost submit | **COMPATIBLE** | Same submit structure |
| nvhost ISP ctrl | **COMPATIBLE** | New ioctls added (2,3,4) |
| nvhost VI ctrl | **COMPATIBLE** | New ioctls added (3,4,5) |
| nvmap | **COMPATIBLE** | Same IOC numbers |

### Required fixes (if attempting blob path):

1. **Add to include/media/imx179.h:**
   ```c
   #define IMX179_IOCTL_GET_OTPDATA  _IOR('o', 8, struct imx179_otp)
   #define IMX179_IOCTL_GET_OTPVEND  _IOR('o', 9, struct imx179_otp)

   struct imx179_otp {
       __u32 otp_size;
       __u8  otp_data[803];
   };
   ```

2. **Add ext_reg3 to imx179_power_rail:**
   ```c
   struct imx179_power_rail {
       struct regulator *dvdd, *avdd, *iovdd;
       struct regulator *ext_reg1, *ext_reg2, *ext_reg3;  // add ext_reg3
   };
   ```

3. **Implement OTP handlers in drivers/media/platform/tegra/imx179.c**

---

## 6. ISP Calibration Data (Mocha-specific)

ISP tuning profiles extracted from Xiaomi stock `libnvodm_imager.so` (2.4MB).
These are embedded as plain text inside the binary. **These are mocha-specific**
calibration data for the actual Primax (rear) and Sunny (front) modules.

### Extracted Profiles

| File | Sensor | Module | Version | Lines |
|------|--------|--------|---------|-------|
| imx179_primax_lfi_v3.09.isp | IMX179 | Primax LFI | v3.09 | 6272 |
| imx179_primax_v2.27.isp | IMX179 | Primax | v2.27 | 6571 |
| imx179_primax_v2.18.isp | IMX179 | Primax | v2.18 | 6234 |
| ov5693_sunny_v2.13.isp | OV5693 | Sunny | v2.13 | 6560 |

Location: `docs/camera-isp-profiles/`

### Parameter Categories (IMX179 v3.09)

| Category | Count | Purpose |
|----------|-------|---------|
| lensShading | 1632 | Vignetting correction tables per color channel |
| reference | 406 | Reference illuminant color data |
| demosaic | 258 | Bayer→RGB interpolation coefficients |
| noiseReduction | 167 | Per-ISO noise models and filter strengths |
| sharpness | 156 | Edge enhancement parameters |
| awb | 100 | Auto white balance gray line, CCT mapping |
| falloff_srfc | 100 | Lens falloff surface data |
| tc | 99 | Tone curve / gamma |
| ae | 54 | Auto exposure targets, algorithms |
| colorCorrection | 24 | 3x3 CCM per illuminant |
| colorEffects | 26 | Saturation, contrast |
| deadPixelCorrection | 60 | Bad pixel thresholds |
| colorArtifactReduction | 40+ | Purple fringing, chromatic aberration |

### ISP Profile Search Paths (runtime override, stock behavior)
```
/data/camera_overrides.isp          (rear)
/data/camera_overrides_front.isp    (front)
/system/lib/camera_overrides.isp
/system/lib/camera_overrides_front.isp
/sdcard/camera_overrides.isp
```

---

## 7. ISP Register Map (Extracted)

Full ISP register map was extracted from `libnvisp_v3.so` binary via host1x
opcode decoding. See `docs/camera-isp-reverse-engineering.md` section 4 for
the complete 24-register-base table and block descriptions.

### Summary
```
0x000-0x015  Control / Enable / Mode
0x100-0x1E0  Color Processing + Tone Curve (224-entry LUT at 0x101)
0x200-0x214  Input Channel A (21 regs)
0x300-0x388  Input Channel B (137 regs)
0x500-0x518  Processing Channel (25 regs)
0x800-0x922  Statistics Configuration (244+ regs)
0xC41-0xCC5  Statistics Output (~500 read-only regs)
0xD31-0xE5C  Lens Shading Correction (294 regs, 2 tables)
0xE00-0xE33  Output Control (7 regs)
```

### 6 ISP Processing Blocks
| Block | Purpose | Key Registers |
|-------|---------|---------------|
| GPP | Tone curve, gamma | 0x101 (224-entry LUT) |
| Lens Shading | Vignetting correction | 0xD31-0xDA8, 0xDAF-0xE5C |
| Demosaic | Bayer → RGB | 0x500 range |
| Luma Enhancement | Edge sharpening | Processing channel |
| Output Downscaler | Resolution scaling | Output chain |
| Bitwise Operations | Pixel manipulation | Bitwise block |

---

## 8. Implementation Status

### Current State: V4L2 + Direct Register Path (no blobs)

```
Sensor (V4L2 subdev) → CSI → VI → [DRAM: RAW] → ISP kernel module → [DRAM: YUV]
                                                                            ↓
                                                                    V4L2 /dev/videoN
                                                                            ↓
                                                              libcamera / GStreamer / app
```

### Completed Work

#### IMX179 Rear Camera — WORKING
- [x] Write `imx179_mocha.c` using `camera_common` framework
  - Mocha-specific power sequence (GPIOs, regulators, MCLK1)
  - Mode tables: 8MP@30, 1080p@30, 720p@90fps
  - CSI-A 4-lane configuration
- [x] Configure MC framework for T124 CSI-A+B
  - T124-specific CIL PHY init in `csi.c`
  - MIPI calibration via `mipi_cal_t124.c`
- [x] **Test:** Capture RAW Bayer via `v4l2-ctl --stream-mmap`
  - Working: SRGGB10 format, /dev/video0 + /dev/media0

#### OV5693 Front Camera — WORKING
- [x] Write `ov5693_mocha.c` using `camera_common` framework
  - Mocha-specific power sequence (GPIOs, regulators, MCLK2)
  - Mode tables: 5MP@30, 1080p@30, 720p@60/90/120fps
  - CSI-E 1-lane configuration
- [x] Configure MC framework for T124 CSI-E
  - T124-specific CIL PHY init in `csi.c`
- [x] **Test:** Capture RAW Bayer via `v4l2-ctl --stream-mmap`
  - Working: SBGGR10 format, /dev/video1 + /dev/media0
  - ~6fps at 5MP (limited by single-shot overhead, not hardware)

#### AD5823 Focuser — WORKING
- [x] Write `ad5823_mocha.c` as V4L2 lens subdev
  - VCM position control via V4L2_CID_FOCUS_ABSOLUTE
  - Power managed by capture channel (lens_chan association)
- [x] DTS integration with tegra-camera-platform modules
  - v4l2_focuser drivernode1 for lens channel
- [x] **Test:** Focuser responds to V4L2 controls
  - Position range 140-640 verified working

### T124-Specific Implementation Details

#### Architecture
- MC framework (first T124 MC usage ever, not soc_camera)
- T124-specific register programming via tegra_channel_write/read abstractions
- Register headers split: t124_registers.h, t210_registers.h, common registers.h
- DSI MIPI cal fix: skip MIPI calibration when DSI panel initialized by bootloader
- IMX179: CILA+CILB → PP_A → VI_CSI_0 (port 0)
- OV5693: CILE → PP_B → VI_CSI_1 (port 1)
- AD5823: lens channel (port 2, no CSI, non-capture)

#### Critical T124-Specific Fixes
1. **Init order**: CSI PHY init AFTER sensor s_stream (R21.5 order) — eliminated CIL errors
2. **Syncpt condition**: PPB_FRAME_START=10, PPA_FRAME_START=9 (not generic T210 formula)
3. **MW_ACK_DONE**: MWA_ACK_DONE=6, MWB_ACK_DONE=7 for T124
4. **ec_recover**: T124 path skips DPD toggle + MC CSI re-init (was killing MIPI pads)
5. **bus-width**: both VI and sensor endpoints must match (1 for OV5693, 4 for IMX179)

#### T124 Syncpt Event Mapping
| Event | T124 (R21.5) | T210 (MC generic) |
|-------|-------------|-------------------|
| PPA_FRAME_START | 9 | 5 |
| PPB_FRAME_START | 10 | 9 |
| MWA_ACK_DONE | 6 | 7 |
| MWB_ACK_DONE | 7 | 11 |

### Future Work

#### Step 3: Minimal ISP Kernel Driver
- Write `isp_t124.c` in `drivers/video/tegra/host/isp/`
- Program minimum ISP registers via host1x command buffer:
  ```
  SET_CLASS(0x32)          → select ISP A
  INCR(0x015, 1)           → enable ISP
  NONINCR(0x200, 21)       → input channel A (buffer address, dimensions, Bayer format)
  NONINCR(0x101, 224)      → tone curve LUT (linear 1:1 initially)
  INCR(0xE00, 1)           → output control (buffer address)
  INCR(0xE30-0xE33, 1 ea.) → output format/size
  syncpoint increment      → trigger processing
  ```
- **Test:** ISP produces viewable YUV from RAW input

#### Step 4: Load Mocha Calibration
- Parse ISP profiles from `docs/camera-isp-profiles/`
- Load lens shading tables → regs 0xD31-0xE5C
- Load tone curves → reg 0x101 (224 entries)
- Load CCM → color processing regs 0x100+
- **Test:** Image quality comparable to stock camera

#### Step 5: 3A Statistics (optional, for auto mode)
- Configure stats engine: regs 0x800-0x922
- Read AE/AWB/AF output: regs 0xC41-0xCC5
- Implement basic auto-exposure and white-balance in userspace
- **Test:** Camera adapts to lighting changes

#### Step 6: Continuous Capture Mode
- Implement continuous streaming (not single-shot)
- Target: real 60+ fps at 720p
- Overlap capture setup with sensor readout

---

## 9. File Reference

### Documentation
```
docs/camera-reverse-engineering.md         ← this file (architecture overview)
docs/camera-isp-reverse-engineering.md     ← ISP register map, reverse engineering details
docs/camera-isp-profiles/                  ← extracted mocha-specific ISP calibration data
docs/tegra124-mc-framework-analysis.md     ← MC framework bring-up analysis
docs/ov5693-register-reference.md          ← OV5693 sensor register reference
```

### Kernel — V4L2 Camera Stack (Working Files)
```
drivers/media/platform/tegra/camera/
├── channel.c              ← V4L2 video device, vb2 buffer queues (T124 direct path)
├── graph.c                ← media controller graph setup, lens channel support
├── mc_common.c            ← media controller helpers
├── core.c                 ← format definitions
├── camera_common.c        ← camera_common framework
├── v4l2_diag.c            ← V4L2 diagnostics and debug
├── registers.h            ← VI/CSI register offsets

drivers/media/platform/tegra/
├── vi/vi.c                ← VI driver with V4L2 integration
├── csi/csi.c              ← CSI transceiver (T124 CIL PHY init)
├── csi/csi.h              ← CSI register definitions
├── mipical/mipi_cal.c     ← MIPI calibration common code
├── mipical/mipi_cal_t124.c ← T124-specific MIPI calibration

drivers/media/i2c/
├── imx179_mocha.c         ← IMX179 V4L2 subdev (mocha-specific, WORKING)
├── ov5693_mocha.c         ← OV5693 V4L2 subdev (mocha-specific, WORKING)
├── ad5823_mocha.c         ← AD5823 focuser V4L2 subdev (WORKING)
├── imx179_mocha_mode_tbls.h ← IMX179 mode tables
├── ov5693_mocha_mode_tbls.h ← OV5693 mode tables
```

### Kernel — ISP
```
drivers/video/tegra/host/isp/
├── isp.c                  ← ISP host1x client (power, clock, bandwidth)
├── isp.h                  ← ISP struct
├── isp_isr_v1.c           ← ISP interrupt handler
├── isp_isr_v1.h
```

### Kernel — Device Tree
```
arch/arm/boot/dts/tegra124-soc-base.dtsi           ← VI, ISP-A, ISP-B nodes
arch/arm/boot/dts/tegra124-platforms/               ← platform-specific camera configs
└── tegra124-mocha-camera-mc.dtsi                  ← mocha camera DT (IMX179, OV5693, AD5823)
```

### Tools
```
tools/camera/v4l2_diag.c   ← Capture diagnostic tool
```

### Stock Blobs (reference only, from android_vendor_xiaomi_mocha)
```
vendor/lib/libnvodm_imager.so     (2.4MB, sensor drivers + ISP calibration)
vendor/lib/libnvmm_camera_v3.so   (1.4MB, pipeline orchestrator)
vendor/lib/libnvisp_v3.so         (49KB, ISP register programming)
vendor/lib/libnvvicsi_v3.so       (17KB, VI/CSI programming)
vendor/lib/libnvrm_graphics.so    (21KB, host1x command buffer — SOURCE OBTAINED)
lib/hw/camera.vendor.tegra.so     (1.1MB, Camera HAL3)
```

### JXD S192 Vendor Source (reference, github.com/Project-Google-Tango/vendor_nvidia_jxd_src)
```
tegra/core/drivers/nvrm/graphics/
├── nvrm_stream.c              ← NvRmStream* full source (37KB)
├── nvrm_channel_linux.c       ← NvRmChannel* Linux ioctl impl (42KB)
├── nvrm_channel_priv.h        ← internal structures
tegra/core/include/
├── nvrm_channel.h             ← public API + NVRM_CH_OPCODE_* macros (75KB)
├── nvrm_stream.h              ← stream constants

tegra/camera/core_v3/include/
└── nvcamera_isp.h             ← ISP attribute API (49KB, 70+ attributes)

tegra/camera-partner/imager/
├── sensor_bayer_imx179.c      ← IMX179 driver source (mode tables, timing)
├── sensor_bayer_ov5693.c      ← OV5693 driver source
├── focuser_ad5823.c           ← AD5823 focuser source
├── imager_nvc.c               ← NVC imager framework (96KB)
└── configs/
    └── sensor_bayer_imx179_camera_config.h ← ISP calibration as C (194KB)

tegra/hwinc/t12x/
├── class_ids.h                ← ISP A=0x32, ISP B=0x34
└── host1x_module_ids.h        ← ISP A=0x18, ISP B=0x1A
```
