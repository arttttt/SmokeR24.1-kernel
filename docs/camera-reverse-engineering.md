# Mocha Camera Stack — Reverse Engineering Report

## Device: Xiaomi Mi Pad 1st Gen (mocha), Tegra K1 (T124)
## Cameras: IMX179 8MP rear (Primax) + OV5693 5MP front (Sunny)

---

## 1. Architecture Overview

### Mocha (Xiaomi stock, KitKat 4.4) — SIMPLER than TN8
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
/dev/camera.pcl
```

### Key difference from TN8 (Shield Tablet):
- **No libnvscf.so** — Mocha does NOT use SCF (Sensor Control Framework)
- TN8: HAL → SCF → NvMM → sensors
- Mocha: HAL → NvMM → sensors (direct, simpler)

---

## 2. Library Inventory

### Mocha stock (from android_vendor_xiaomi_mocha)

| Library | Size | Location | Purpose |
|---------|------|----------|---------|
| camera.vendor.tegra.so | 1.1MB | system/lib/hw/ | Camera HAL3 + Xiaomi CUDA features |
| libnvmm_camera_v3.so | 1.4MB | system/vendor/lib/ | Main camera pipeline |
| libnvmm_camera.so | 625KB | system/vendor/lib/ | Legacy pipeline |
| libnvodm_imager.so | **2.4MB** | system/vendor/lib/ | Sensor drivers + ISP calibration |
| libnvisp_v3.so | 49KB | system/vendor/lib/ | ISP HW interface |
| libnvisp.so | 49KB | system/vendor/lib/ | ISP HW interface (legacy) |
| libnvvicsi_v3.so | 17KB | system/vendor/lib/ | VI/CSI HW interface |
| libnvvicsi.so | 17KB | system/vendor/lib/ | VI/CSI HW interface (legacy) |
| libnvcam_imageencoder.so | 37KB | system/vendor/lib/ | JPEG encoding |
| libnvcamerahdr_v3.so | 310KB | system/vendor/lib/ | HDR processing |
| libnvcamerahdr.so | 310KB | system/vendor/lib/ | HDR processing (legacy) |
| libnvcameratools.so | 85KB | system/vendor/lib/ | Camera debug/tuning |
| libnvstitching.so | 634KB | system/vendor/lib/ | Panorama stitching |
| libnvobjecttracking.so | 370KB | system/vendor/lib/ | Object tracking |
| libnvtnr.so | 33KB | system/vendor/lib/ | Temporal NR |
| libfcamdng.so | 446KB | system/vendor/lib/ | DNG/RAW file support |
| libopencv24_tegra.so | 7.3MB | system/lib/ | OpenCV (Tegra optimized) |
| libtbb.so | 181KB | system/lib/ | Intel TBB |
| libbeautify.so | 378KB | system/lib/ | Xiaomi skin beautification |
| libFaceProc.so | 3.1MB | system/lib/ | Face detection (Xiaomi) |

### NOT present in Mocha (TN8-only):
- libnvscf.so / libscf.so (SCF framework)
- libnvcamerautils.so
- libnvcamlog.so
- libnvcameranrr.so
- libnvfnet.so (NVIDIA face detection)
- libnvcudautils.so

---

## 3. TN8 vs Mocha Binary Comparison

### libnvodm_imager.so — COMPLETELY DIFFERENT

| Aspect | TN8 | Mocha |
|--------|-----|-------|
| Size | ~5MB | 2.4MB |
| Sensors | 15+ (IMX135, OV5693, AR0261, IMX179, IMX214...) | IMX179, OV5693 + legacy refs |
| ISP calibration | Generic/TN8-specific | **XIAOMI X6 specific** (Primax/Sunny) |
| Compiler | GCC 4.9 | GCC 4.4.3 |
| Dependencies | +libnvcamv4l2.so | +libnvodm_query.so |
| Badge strings | tn8_a00_rear, tn8_a03_rear | (none — uses PCL matching) |

### libnvisp_v3.so — DIFFERENT
| Aspect | TN8 | Mocha |
|--------|-----|-------|
| Size | 63KB | 49KB |
| Exports | 56 functions | 39 functions |
| Compiler | GCC 4.9 / Clang 3.8 | GCC 4.7 |

### libnvvicsi_v3.so — DIFFERENT
| Aspect | TN8 | Mocha |
|--------|-----|-------|
| Size | 30KB | 17KB |
| Exports | 61 functions | 49 functions |
| Device nodes | /dev/camera.pcl | /dev/nvhost-ctrl-vi.1 |

### Conclusion: TN8 blobs CANNOT substitute for Mocha blobs

---

## 4. ISP Calibration Data (Extracted from libnvodm_imager.so)

ISP tuning profiles are embedded as **plain text** inside the binary.
Extracted to: `docs/camera-isp-profiles/`

### Available profiles:

| File | Sensor | Integrator | Version | Date | Lines |
|------|--------|------------|---------|------|-------|
| imx179_primax_lfi_v3.09.isp | IMX179 | Primax Lens Flare Improved | v3.09 | Jul 7, 2014 | 6272 |
| imx179_primax_v2.27.isp | IMX179 | Primax | v2.27 | Jul 7, 2014 | 6571 |
| imx179_primax_v2.18.isp | IMX179 | Primax | v2.18 | Jun 6, 2014 | 6234 |
| ov5693_sunny_v2.13.isp | OV5693 | Sunny | v2.13 | Jul 4, 2014 | 6560 |

### Parameter categories in IMX179 v3.09:

| Category | Count | Purpose |
|----------|-------|---------|
| lensShading | 1632 | Vignetting correction tables per color channel |
| reference | 406 | Reference illuminant color data |
| demosaic | 258 | Bayer-to-RGB interpolation coefficients |
| noiseReduction | 167 | Per-ISO noise models and filter strengths |
| sharpness | 156 | Edge enhancement parameters |
| awb | 100 | Auto white balance gray line, CCT mapping |
| falloff_srfc | 100 | Lens falloff surface data |
| tc | 99 | Tone curve / gamma |
| ae | 54 | Auto exposure targets, algorithms |
| colorCorrection | 24 | 3x3 color correction matrices per illuminant |
| colorEffects | 26 | Saturation, contrast adjustments |
| deadPixelCorrection | 60 | Static/dynamic bad pixel thresholds |
| colorArtifactReduction | 40+ | Purple fringing, chromatic aberration |

### ISP file search paths (runtime override):
```
/data/camera_overrides.isp          (rear)
/data/camera_overrides_front.isp    (front)
/system/lib/camera_overrides.isp
/system/lib/camera_overrides_front.isp
/sdcard/camera_overrides.isp
```

### Calibration binary paths:
```
/data/calibration.bin, /data/calibration_front.bin
/data/factory.bin, /data/factory_front.bin
/mnt/factory/camera/factory.bin
```

---

## 5. Kernel API Compatibility (Old vs New)

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

### Required kernel fixes for old Mocha blobs:

1. **Enable in defconfig:**
   ```
   CONFIG_VIDEO_IMX179=y
   CONFIG_VIDEO_OV5693=y
   CONFIG_VIDEO_AD5823=y
   CONFIG_VIDEO_DW9718=y
   CONFIG_VIDEO_CAMERA=y  (already enabled)
   ```

2. **Add to include/media/imx179.h:**
   ```c
   #define IMX179_IOCTL_GET_OTPDATA  _IOR('o', 8, struct imx179_otp)
   #define IMX179_IOCTL_GET_OTPVEND  _IOR('o', 9, struct imx179_otp)

   struct imx179_otp {
       __u32 otp_size;
       __u8  otp_data[803];
   };
   ```

3. **Add ext_reg3 to imx179_power_rail:**
   ```c
   struct imx179_power_rail {
       struct regulator *dvdd, *avdd, *iovdd;
       struct regulator *ext_reg1, *ext_reg2, *ext_reg3;  // add ext_reg3
   };
   ```

4. **Implement OTP handlers in drivers/media/platform/tegra/imx179.c**

---

## 6. Hardware ISP + V4L2: Feasibility Analysis

### How Tegra K1 ISP works

The ISP is a hardware block on the host1x bus, programmed through **nvhost channel submit**:

```
Userspace (libnvisp_v3.so):
  1. Open /dev/nvhost-isp channel
  2. Allocate nvmap buffer for command stream
  3. Write ISP register commands into buffer:
     [REG_ADDR_1] [VALUE_1] [REG_ADDR_2] [VALUE_2] ...
  4. Submit via NVHOST_IOCTL_CHANNEL_SUBMIT
  5. Wait for syncpoint completion

Kernel (nvhost):
  - DMA command buffer to ISP hardware
  - ISP processes data in-place or to output buffer
  - Signal syncpoint when done
```

### Can we use hardware ISP from V4L2 path?

**Theoretically YES**, but requires significant work:

#### What exists:
- ISP kernel driver (isp.c) — manages power/clocks/bandwidth
- nvhost channel submit API — documented in nvhost_ioctl.h
- nvmap ↔ dma_buf conversion — NVMAP_IOC_GET_FD / NVMAP_IOC_FROM_FD
- ISP calibration data — extracted (25,000+ lines of tuning parameters)

#### What's missing:
- **ISP register map** — which registers control demosaic, AWB, NR, etc.
  The ~50KB libnvisp_v3.so encodes this knowledge
- **Command buffer format** — exact sequence of register writes for processing a frame
- **Input/output buffer layout** — how ISP expects source RAW and produces output

#### Approach to reverse ISP register programming:

1. **Trace libnvisp_v3.so on working system** (old kernel + old blobs):
   - Intercept NVHOST_IOCTL_CHANNEL_SUBMIT calls
   - Dump command buffers (register address + value pairs)
   - Map register addresses to ISP functions

2. **Use NVIDIA TRM** (Technical Reference Manual):
   - T124 TRM may document ISP registers
   - Available under NDA or leaked versions exist

3. **Compare with T210 open driver** (mainline Linux):
   - Mainline has tegra210 VI driver, no ISP
   - But NVIDIA L4T for Jetson has more open ISP code for T210

#### Hybrid V4L2 + Hardware ISP pipeline:

```
Step 1: V4L2 capture (vi2.c)
  IMX179 → CSI → VI → DMA buffer (RAW Bayer)
                         ↓ (dma_buf fd)
Step 2: ISP processing (userspace via nvhost)
  Open /dev/nvhost-isp
  Import dma_buf fd → nvmap handle (NVMAP_IOC_FROM_FD)
  Allocate output nvmap buffer
  Build ISP command buffer (register writes based on .isp profile)
  Submit to ISP via NVHOST_IOCTL_CHANNEL_SUBMIT
  Wait for syncpoint
  Result: processed RGB/YUV buffer
                         ↓
Step 3: Camera HAL
  Export output as gralloc buffer → Android
```

#### Effort estimate:
- Reverse ISP registers: **weeks** (tracing + TRM analysis)
- Write ISP programming library: **weeks** (replace libnvisp_v3.so)
- Integrate with V4L2: **days** (buffer conversion is straightforward)
- Total: **1-2 months** of focused work

### Alternative: GPU ISP (much faster to implement)

```
V4L2 capture (RAW Bayer)
        ↓ (dma_buf fd → EGLImage)
GPU compute shader:
  - Demosaic (using demosaic params from .isp profile)
  - Color correction (using 3x3 matrices from .isp profile)
  - AWB (using gray line / CCT params from .isp profile)
  - Lens shading correction (using lensShading tables)
  - Noise reduction (using per-ISO noise models)
  - Tone curve / gamma (using tc params)
  - Edge sharpening (using sharpness params)
        ↓
Camera HAL → Android
```

Tegra K1 GPU: 192 CUDA cores, Kepler, OpenGL ES 3.1 + compute shaders.
Estimated performance: **8MP RAW → RGB at 30fps** (~15ms per frame).

#### Effort estimate:
- Write GLES compute demosaic shader: **days**
- Parse .isp profile and apply parameters: **days**
- Integrate with V4L2 + Camera HAL: **1 week**
- Total: **2-3 weeks**

---

## 7. Recommended Path Forward

### Option A: Proprietary path (fix userspace compatibility)
- Restore Mocha stock blobs (from android_vendor_xiaomi_mocha)
- Fix 3 kernel API breaks (OTP ioctls, ext_reg3)
- Debug Android framework compatibility (KK blobs on Nougat+)
- **Risk: High** — user already tried this and hit userspace issues
- **Reward: Full ISP** quality with hardware acceleration

### Option B: V4L2 + GPU ISP (recommended)
- Write imx179_v4l2.c sensor subdev driver
- Enable vi2.c soc_camera stack
- Write GPU compute shader ISP using extracted .isp profiles
- Modify Antmicro HAL or write minimal Camera HAL
- **Risk: Medium** — known-good architecture, no binary dependencies
- **Reward: Good quality** with GPU-accelerated ISP, fully open

### Option C: V4L2 + Hardware ISP (ambitious)
- Everything from Option B, plus:
- Reverse engineer ISP register map via tracing
- Write open ISP programming library
- **Risk: High** — ISP registers are undocumented
- **Reward: Best quality** + hardware ISP acceleration

### Option D: Incremental approach (B → C)
1. Start with V4L2 + GPU ISP (working camera in weeks)
2. In parallel, trace ISP registers on old system
3. Gradually replace GPU stages with hardware ISP
- **Risk: Low** — always have a working fallback
- **Reward: Progressively better** quality

---

## 8. File Locations

### Extracted ISP profiles:
```
docs/camera-isp-profiles/
├── imx179_primax_lfi_v3.09.isp    (6272 lines, BEST rear)
├── imx179_primax_v2.27.isp        (6571 lines, rear)
├── imx179_primax_v2.18.isp        (6234 lines, rear)
└── ov5693_sunny_v2.13.isp         (6560 lines, front)
```

### Key kernel files:
```
drivers/media/platform/soc_camera/tegra_camera/vi2.c     — V4L2 VI host
drivers/media/platform/soc_camera/tegra_camera/common.c  — format handling
drivers/media/i2c/soc_camera/imx135_v4l2.c               — sensor template
drivers/media/platform/tegra/imx179.c                     — NVC IMX179 (register tables)
drivers/video/tegra/host/isp/isp.c                        — ISP power/clock
include/linux/nvhost_ioctl.h                              — nvhost submit API
include/linux/nvmap.h                                     — nvmap ↔ dma_buf
```

### Stock blobs:
```
/Users/artem/Projects/android_vendor_xiaomi_mocha/proprietary/
├── vendor/lib/libnvodm_imager.so
├── vendor/lib/libnvmm_camera_v3.so
├── vendor/lib/libnvisp_v3.so
├── vendor/lib/libnvvicsi_v3.so
└── lib/hw/camera.vendor.tegra.so
```
