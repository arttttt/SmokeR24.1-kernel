# Tegra K1 ISP — Deep Reverse Engineering Report

## Device: Xiaomi Mi Pad 1st Gen (mocha), Tegra K1 (T124)

---

## 1. ISP Hardware Architecture

### Memory Map
| Component | Physical Base | Size | IRQ |
|-----------|--------------|------|-----|
| ISP A | 0x54600000 | 256KB | 71 |
| ISP B | 0x54680000 | 256KB | 70 |
| VI | 0x54080000 | 256KB | 69 |
| Host1X | 0x50000000 | 208KB | — |

### Host1x Class IDs
| Module | Class ID | Used in command buffer SET_CLASS opcode |
|--------|----------|----------------------------------------|
| ISP A | 0x32 | `nvhost_opcode_setclass(0x32, offset, mask)` |
| ISP B | 0x34 | `nvhost_opcode_setclass(0x34, offset, mask)` |
| VI | 0x30 | `nvhost_opcode_setclass(0x30, offset, mask)` |

### Known ISP Register Offsets (from kernel)
| Offset | Name | Purpose |
|--------|------|---------|
| 0x74 | ISP_CG_CTRL | Clock gating (bit 0 = 2nd level CG enable) |
| 0xF8 | ISP_INT_STATUS | Interrupt status (bit 5 = MFI) |
| 0x14C | ISP_ENABLE | ISP enable (bit 0) |

### Power/Clock
- Power gate: TEGRA_POWERGATE_VENC (shared with VI)
- Clocks: isp, emc, sclk
- SMMU groups: TEGRA_SWGROUP_ISP (8), TEGRA_SWGROUP_ISP2B (29)

---

## 2. Complete Software Stack

### Call Chain: Camera App → ISP Hardware
```
Android CameraService
        ↓
camera.vendor.tegra.so          (Camera HAL3, 1.1MB)
        ↓
libnvmm_camera_v3.so           (Pipeline orchestrator, 1.4MB)
  - 3A algorithms (AE/AWB/AF)
  - Frame scheduling, buffer management
  - Post-processing: 3DPP, TNR, Scaler, FaceDetect, VStab, CUDA
  ┌─────────┼──────────────────────┐
  ↓         ↓                      ↓
libnvisp_v3.so   libnvvicsi_v3.so   libnvodm_imager.so
(50KB)           (17KB)              (2.4MB)
ISP programming  VI/CSI capture      Sensor drivers +
6 ISP blocks     MIPI setup          ISP calibration
  ↓              ↓                   ↓
libnvrm_graphics.so                  /dev/imx179
(21KB)                               /dev/ov5693
NvRmStream* →                        /dev/camera.pcl
NvRmChannelSubmit →
  ↓
ioctl(NVHOST_IOCTL_CHANNEL_SUBMIT)
  ↓
/dev/nvhost-isp    /dev/nvhost-ctrl-vi
  ↓                ↓
ISP Hardware       VI/CSI Hardware
(0x54600000)       (0x54080000)
```

---

## 3. ISP Register Programming Mechanism

### Two paths for register access:

#### Path 1: Command Buffer (main path for frame processing)
```c
// libnvisp_v3.so calls:
NvRmStreamBegin(stream);
NvRmStreamPushSetClass(ctx, stream, subclass, ISP_CLASS_0x32);
NvRmStreamPushIncr(ctx, stream, count, reg_offset, value, ...);
// ... more register writes ...
NvRmStreamEnd(stream, &syncpoint);
```

#### Path 2: Direct Register Write (for control registers)
```c
// libnvisp_v3.so calls:
NvRmHostModuleRegWr(ctx, channel, reg_offset, value_ptr);
// → builds ioctl struct → ioctl() → kernel writes register directly
```

### Command Buffer Opcode Encoding (from hardware_t124.h)
```
SET_CLASS: (0 << 28) | (offset << 16) | (class_id << 6) | mask
INCR:      (1 << 28) | (offset << 16) | count      → sequential regs
NONINCR:   (2 << 28) | (offset << 16) | count      → same reg repeated
MASK:      (3 << 28) | (offset << 16) | mask
IMM:       (4 << 28) | (offset << 16) | value       → 16-bit immediate
```

### NvRmStreamPushIncr Pseudocode (reversed from libnvrm_graphics.so @ 0x2a16):
```c
// Despite the name "Incr", this generates NONINCR opcode (0x20000000)
void NvRmStreamPushIncr(ctx, stream, count, offset, ...) {
    uint32_t *buf = stream->write_ptr;   // [stream + 0x4C]
    buf[0] = (offset << 16) | 0x20000000 | 1;  // NONINCR, count=1
    buf[1] = value;                              // register value
    stream->write_ptr = buf + 2;                 // advance 8 bytes
    // Optional: tracking/profiling path at ctx+0x6E00
}
```

### NvRmStreamPushSetClass Pseudocode (@ 0x2a02):
```c
void NvRmStreamPushSetClass(ctx, stream, subclass, class_id) {
    uint32_t *buf = stream->write_ptr;
    *buf = class_id << 6;           // SET_CLASS opcode (bits [31:28] = 0)
    stream->write_ptr = buf + 1;
    ctx->current_subclass = subclass;  // [ctx + 0x14]
    ctx->current_class = class_id;     // [ctx + 0x18]
}
```

### NvRmHostModuleRegWr Pseudocode (@ 0x4296):
```c
void NvRmHostModuleRegWr(ctx, channel, offset, value_ptr) {
    // Builds ioctl struct:
    struct {
        uint32_t handle, offset, size=4, is_write=1;
        uint32_t reserved, zero, value, zero2;
    } req;
    ioctl(fd, IOCTL_XXX, &req);
}
```

---

## 4. ISP Processing Blocks and Register Offsets

### 6 ISP Hardware Blocks (from libnvisp_v3.so)

| Block | Source File | Function | Size | Register Offsets |
|-------|-----------|----------|------|-----------------|
| GPP | nvisp_hw_at_tf_gpp.cpp | NvIspHwSettingsCopyGpp | 0x228 | 0x070, 0x088, 0x101 |
| Lens Shading | nvisp_hw_ls.cpp | NvIspHwSettingsCopyLensShading | 0x364 | 0x068, 0x0EE |
| Demosaic | nvisp_hw_dm.cpp | NvIspHwSettingsCopyDemosaic | 0x884 | 0x0E6, 0x0EC, 0x0EF |
| Luma Enhancement | nvisp_hw_le.cpp | NvIspHwSettingsCopyLumaEnhancement | 0xD9C | 0x060, 0x16B |
| Output Downscaler | nvisp_hw_ds.cpp | NvIspHwSettingsCopyOutputDownScaler | 0xC48 | 0x0B0, 0x0B2, 0x0B4 |
| Bitwise Op | nvisp_hw_bitop.cpp | NvIspHwSettingsCopyBitwiseOperation | 0x440 | 0x09B |

### Statistics Blocks
| Block | Function | Offsets |
|-------|----------|---------|
| Stats Read | NvIspGetStats | 0x024, 0x034, 0x09C |
| Stats Config | NvIspSetStats | 0x020, 0x024, 0x048, 0x068 |

### Statistics Types
- FB_STATS — Frame Buffer statistics
- FM_STATS — Focus Metric statistics
- LAC — Local Area Contrast statistics

### Register Offset Notes
- Values like 0x024, 0x040 may be structure offsets (buffer access), not ISP registers
- Values 0x0E6, 0x0EC, 0x0EF, 0x0EE, 0x101, 0x16B are likely ISP register offsets
- These are word-aligned offsets within the ISP class address space
- Full register space is 256KB (0x00000-0x3FFFF), only ~20 offsets identified

---

## 5. Frame Processing Data Flow

### Capture Pipeline
```
1. NvOdmImagerSetSensorMode()    → Configure IMX179/OV5693 via I2C
2. NvViCsiCaptureFrame()         → VI captures RAW Bayer via CSI → DMA buffer
   Device: /dev/nvhost-ctrl-vi
   Syncpoints: VI_FRAME_START(9), VI_MW_REQ_DONE(4), VI_MW_ACK_DONE(6)

3. NvIspProcessFrame()           → ISP processes RAW → RGB/YUV
   Device: /dev/nvhost-isp
   Command buffer: SET_CLASS(0x32) → register writes → syncpoint incr
   ISP blocks applied in order:
     a. GPP (General Purpose Processing/Transform)
     b. Lens Shading correction
     c. Demosaic (Bayer → RGB)
     d. Luma Enhancement
     e. Output Downscaler
     f. Bitwise Operations

4. Post-ISP (in libnvmm_camera_v3.so):
     a. Pre-ISP HDR (NvPreIspHdrProcess)
     b. 3DPP tone mapping (pp3dProcess)
     c. TNR temporal noise reduction
     d. Video stabilization
     e. Face detection
     f. CUDA DCT denoising
     g. Scaler (output resize)

5. Output → Camera HAL → Android framework
```

### Buffer Flow
```
NvRmMemHandleCreate → NvRmMemHandleAllocAttr → NvRmMemPin
  ↓
NvRmSurfaceSetup (create surface descriptor)
  ↓
VI DMA capture → buffer (RAW Bayer)
  ↓
ISP input (same buffer or intermediate)
  ↓
ISP output → post-processing buffers
  ↓
Output surface → Camera HAL → gralloc
```

### Frame Queue Architecture
```
ViPendingQueue → CaptureQueue → RawCaptureCompleteQueue
                                         ↓
                              PreISPInputQueue → PreISPOutputQueue
                                                        ↓
                                                   ISPOutputQueue
                                                        ↓
                                                  HostCaptureQueue
```

### Synchronization
- Tegra syncpoints (NOT Linux dma-fence)
- NvRmChannelSyncPointRead/WaitTimeout
- NvCameraAddNewSyncPoint / NvCameraIspInsertSyncPtIncr

---

## 6. Mocha vs TN8 Comparison

### libnvisp_v3.so
| | Mocha | TN8 |
|---|---|---|
| Size | 50KB (39 exports) | 63KB (~55 exports) |
| Platform init | PopulateIspHwFunctions_T12x | PopulateIspHwFunctions_T12x (same!) |
| ISP blocks | 6 (GPP, LS, DM, LE, DS, BitOp) | Same 6 blocks |
| Extra in TN8 | — | NvIspProcessFrame3, NvIspGetCapabilities, NvCameraHwPacketReadRegs |
| Source paths | Same NVIDIA internal paths | Same paths |

**Key: Both target same T124 ISP hardware. TN8 is newer build with extra utility functions.**

### libnvvicsi_v3.so
| | Mocha | TN8 (T124) |
|---|---|---|
| Size | 17KB (49 exports) | 30KB (61 exports) |
| Mocha-only | NvViCsiSetFlash* (2 funcs) | — |
| TN8-only | — | I2C via VI, Watchdog, DPD mask (12 funcs) |

### libnvmm_camera_v3.so (Mocha only)
- 1.4MB, pipeline orchestrator
- Dependencies: libnvisp_v3.so, libnvvicsi_v3.so, libnvodm_imager.so, libcuda.so, libEGL.so
- Implements 3A (AE/AWB/AF), PNode processing, buffer management
- Device nodes accessed: /dev/nvhost-ctrl-vi, /dev/video0, /dev/video1

---

## 7. ISP Calibration Data

### Extracted Profiles (from libnvodm_imager.so)
| Profile | Sensor | Lines | Location |
|---------|--------|-------|----------|
| imx179_primax_lfi_v3.09.isp | IMX179 (rear) | 6272 | docs/camera-isp-profiles/ |
| imx179_primax_v2.27.isp | IMX179 (rear) | 6571 | docs/camera-isp-profiles/ |
| imx179_primax_v2.18.isp | IMX179 (rear) | 6234 | docs/camera-isp-profiles/ |
| ov5693_sunny_v2.13.isp | OV5693 (front) | 6560 | docs/camera-isp-profiles/ |

### Profile Parameters → ISP Register Mapping
| .isp Parameter | ISP Block | Register Offset |
|----------------|-----------|-----------------|
| demosaic.* | Demosaic | 0x0E6, 0x0EC, 0x0EF |
| lensShading.* | Lens Shading | 0x068, 0x0EE |
| sharpness.* | Luma Enhancement | 0x060, 0x16B |
| noiseReduction.* | GPP? | 0x070, 0x088 |
| tc.* (tone curve) | GPP | 0x101? |
| colorCorrection.* | GPP | TBD |
| ae.* | Stats Config | 0x020-0x068 |
| awb.* | Stats Config | 0x020-0x068 |

---

## 8. Key Findings

### Public Documentation Status
- **NO public ISP register documentation exists**
- Searched: NVIDIA forums, GitHub, XDA, kernel mailing lists, L4T repos
- Only 3 register offsets documented in kernel (0x74, 0xF8, 0x14C)
- ISP is entirely programmed from userspace via proprietary blobs

### Critical Library: libnvrm_graphics.so (21KB)
- NOT graphics-only — shared host1x infrastructure for ALL nvhost devices
- Contains NvRmStream*, NvRmHostModuleRegWr/Rd, NvRmChannelSubmit
- This is the bridge between libnvisp_v3.so and kernel
- Small enough (21KB) to fully reverse engineer

### ISP Register Access is Abstracted
- libnvisp_v3.so does NOT contain hardcoded ISP register addresses
- Register offsets are passed as parameters to NvRmStreamPushIncr
- The actual offsets come from the HwSettingsCopy* functions
- ~20 unique register offsets identified across 6 ISP blocks

---

## 9. Next Steps for Full ISP Register Map

### Method 1: Runtime Tracing (RECOMMENDED)
1. Boot old kernel with stock Mocha blobs (KitKat)
2. Hook/intercept NvRmStreamPushIncr in libnvrm_graphics.so
3. Capture all (register_offset, value) pairs during camera operation
4. Map offsets to ISP blocks using known function call chain
5. Correlate with .isp profile parameters

### Method 2: Deep Static Analysis
1. Disassemble all 6 NvIspHwSettingsCopy* functions with Thumb2 support
2. Trace all values loaded into r3 (offset param) before NvRmStreamPushIncr calls
3. Map each offset to its ISP block and parameter meaning
4. Cross-reference with .isp profile parameter names

### Method 3: GPU ISP Alternative (bypass hardware ISP entirely)
1. Use V4L2 vi2.c to capture RAW Bayer
2. Write GPU compute shaders for each ISP stage:
   - Demosaic (using demosaic.* params from .isp profile)
   - Color correction (using colorCorrection.* 3x3 matrices)
   - Lens shading (using lensShading.* tables)
   - Noise reduction (using noiseReduction.* per-ISO models)
   - Tone curve / gamma (using tc.* params)
   - Edge sharpening (using sharpness.* params)
3. Tegra K1 GPU: 192 CUDA cores, ~15ms per 8MP frame = 30fps
4. Estimated effort: 2-3 weeks

---

## 10. File Index

### This analysis:
- `docs/camera-isp-reverse-engineering.md` — this file
- `docs/camera-reverse-engineering.md` — architecture overview
- `docs/camera-isp-profiles/` — extracted ISP calibration data

### Key kernel files:
- `drivers/video/tegra/host/isp/isp.c` — ISP platform driver
- `drivers/video/tegra/host/t124/hardware_t124.h` — host1x opcode definitions
- `drivers/video/tegra/host/class_ids.h` — ISP class IDs (0x32, 0x34)
- `drivers/media/platform/soc_camera/tegra_camera/vi2.c` — V4L2 VI host
- `arch/arm/mach-tegra/iomap.h` — ISP physical addresses

### Stock blobs analyzed:
- `libnvisp_v3.so` (50KB) — ISP HW programming, 6 blocks
- `libnvvicsi_v3.so` (17KB) — VI/CSI HW programming
- `libnvmm_camera_v3.so` (1.4MB) — pipeline orchestrator, 3A
- `libnvrm_graphics.so` (21KB) — host1x command buffer infrastructure
- `libnvodm_imager.so` (2.4MB) — sensor drivers + ISP calibration data
