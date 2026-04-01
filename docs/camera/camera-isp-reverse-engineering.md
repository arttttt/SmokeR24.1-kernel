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

### Host1x Module IDs
| Module | Module ID | Used in NvRmHostModuleRegWr |
|--------|-----------|----------------------------|
| ISP A | 0x18 | `NVRM_MODULE_ID(0x18, 0)` |
| ISP B | 0x1A | `NVRM_MODULE_ID(0x1A, 0)` |

### Known ISP Register Offsets (from kernel source)
| Offset | Name | Purpose |
|--------|------|---------|
| 0x74 | ISP_CG_CTRL | Clock gating (bit 0 = 2nd level CG enable) |
| 0xF8 | ISP_INT_STATUS | Interrupt status (bit 5 = MFI) |
| 0x14C | ISP_ENABLE | ISP enable (bit 0) |

### Power/Clock
- Power gate: TEGRA_POWERGATE_VENC (shared with VI)
- Clocks: isp, emc, sclk
- SMMU groups: TEGRA_SWGROUP_ISP (8), TEGRA_SWGROUP_ISP2B (29)

### ISP and VI — Separate Host1x Clients

**VI and ISP are completely independent host1x devices.** They share the VENC power gate
but otherwise have separate:
- Register apertures (VI: 0x54080000, ISP-A: 0x54600000, ISP-B: 0x54680000)
- Host1x channels and syncpoints
- Bandwidth management (ISO clients)
- Clock domains

**Data flow: VI → Memory → ISP → Memory** (not inline).

VI captures raw Bayer from CSI into a DMA buffer. ISP reads that buffer, processes it,
and writes the result to a separate output buffer. There is no hardware-level pipeline
between VI and ISP — they are synchronized via syncpoints.

```
Sensor → MIPI CSI → VI (class 0x30) → DMA write → [DRAM buffer: RAW Bayer]
                                                          ↓
                                          ISP (class 0x32) DMA read
                                                          ↓
                                          ISP processing (demosaic, etc.)
                                                          ↓
                                          ISP DMA write → [DRAM buffer: YUV/RGB]
```

---

## 2. Proprietary Software Stack (Xiaomi stock, Android 4.4)

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

This is the proprietary (blob) stack. It is NOT used in our V4L2 approach.
Documented here for reference and reverse engineering context.

---

## 3. ISP Register Programming Mechanism

### Two paths for register access:

#### Path 1: Command Buffer (main path for frame processing)
```c
// libnvisp_v3.so builds host1x command buffers:
NvRmStreamBegin(stream);
NvRmStreamPushSetClass(stream, subclass, ISP_CLASS_0x32);
// ... register writes via NVRM_STREAM_PUSH_U ...
NvRmStreamEnd(stream, &syncpoint);
```

#### Path 2: Direct Register Write (for control registers)
```c
NvRmHostModuleRegWr(hDevice, Module, Offset, Value);
// → ioctl(NVHOST_IOCTL_CTRL_MODULE_REGRDWR) → kernel writes register directly
```

### Command Buffer Opcode Encoding
```
SET_CLASS: (0 << 28) | (offset << 16) | (class_id << 6) | mask
INCR:      (1 << 28) | (offset << 16) | count      → sequential regs
NONINCR:   (2 << 28) | (offset << 16) | count      → same reg repeated
MASK:      (3 << 28) | (offset << 16) | mask
IMM:       (4 << 28) | (offset << 16) | value       → 16-bit immediate
```

### Opcode Macros (from nvrm_channel.h, JXD vendor source)
```c
#define NVRM_CH_OPCODE_SET_CLASS(ClassID, Offset, Mask) \
    ((0 << 28) | ((Offset) << 16) | ((ClassID) << 6) | (Mask))
#define NVRM_CH_OPCODE_INCR(Offset, Count) \
    ((1 << 28) | ((Offset) << 16) | (Count))
#define NVRM_CH_OPCODE_NONINCR(Offset, Count) \
    ((2 << 28) | ((Offset) << 16) | (Count))
#define NVRM_CH_OPCODE_MASK(Offset, Mask) \
    ((3 << 28) | ((Offset) << 16) | (Mask))
#define NVRM_CH_OPCODE_IMM(Offset, Value) \
    ((4 << 28) | ((Offset) << 16) | (Value))
#define NVRM_STREAM_PUSH_U(pCurrent, Data) \
    (*(pCurrent)++ = (NvU32)(Data))
```

### ISP Register Writes — How They Actually Work
```c
// ISP registers are written using NVRM_STREAM_PUSH_U macro directly.
// Example: write 4 sequential registers starting at 0x100:
NVRM_STREAM_PUSH_U(pCurrent, NVRM_CH_OPCODE_INCR(0x100, 4));
NVRM_STREAM_PUSH_U(pCurrent, value0);  // reg 0x100
NVRM_STREAM_PUSH_U(pCurrent, value1);  // reg 0x101
NVRM_STREAM_PUSH_U(pCurrent, value2);  // reg 0x102
NVRM_STREAM_PUSH_U(pCurrent, value3);  // reg 0x103

// Example: write same register 224 times (tone curve LUT):
NVRM_STREAM_PUSH_U(pCurrent, NVRM_CH_OPCODE_NONINCR(0x101, 224));
for (i = 0; i < 224; i++)
    NVRM_STREAM_PUSH_U(pCurrent, lut[i]);  // all write to reg 0x101
```

### CORRECTION: NvRmStreamPushIncr is NOT a Register Write

Previous reverse engineering (from binary analysis alone) incorrectly identified
`NvRmStreamPushIncr` as an ISP register write function. JXD source reveals it is
a **syncpoint increment**:

```c
// Source: nvrm_stream.c from JXD vendor source
void NvRmStreamPushIncr(NvRmStreamRec *pStream, NvU32 Cond,
                        NvU32 SyncPointID, NvU32 Reg) {
    NvU32 *pCurrent = pStream->pCurrent;
    NVRM_STREAM_PUSH_U(pCurrent, NVRM_CH_OPCODE_NONINCR(Reg, 1));
    NVRM_STREAM_PUSH_U(pCurrent,
        NV_DRF_NUM(NV_CLASS_HOST, INCR_SYNCPT, COND, Cond) |
        NV_DRF_NUM(NV_CLASS_HOST, INCR_SYNCPT, INDX, SyncPointID));
    pStream->pCurrent = pCurrent;
    pStream->NumSyncPointIncrs++;
}
// Conditions: IMMEDIATE=0, OP_DONE=1, RD_DONE=2, REG_WR_SAFE=3
```

### NvRmStreamPushSetClass (source: nvrm_stream.c)
```c
void NvRmStreamPushSetClass(NvRmStreamRec *pStream, NvU32 SubClass,
                            NvU32 ClassID) {
    NvU32 *pCurrent = pStream->pCurrent;
    NVRM_STREAM_PUSH_U(pCurrent, NVRM_CH_OPCODE_SET_CLASS(ClassID, 0, 0));
    pStream->pCurrent = pCurrent;
    pStream->CurrentSubClass = SubClass;
    pStream->CurrentClass = ClassID;
}
// For ISP A: ClassID = 0x32, For ISP B: ClassID = 0x34
```

### NvRmHostModuleRegWr (source: nvrm_channel_linux.c)
```c
// Direct register write (bypasses command buffer, used for control regs)
NvError NvRmHostModuleRegWr(NvRmDeviceHandle hDevice,
    NvRmModuleID Module, NvU32 Offset, NvU32 Value) {
    struct nvhost_ctrl_module_regrdwr args;
    args.id = NVRM_MODULE_ID_MODULE(Module);
    args.num_offsets = 1;
    args.block_size = 4;
    args.offsets = (uintptr_t)&Offset;
    args.values = (uintptr_t)&Value;
    args.write = 1;
    ioctl(fd, NVHOST_IOCTL_CTRL_MODULE_REGRDWR, &args);
}
```

### Command Buffer Architecture (source: nvrm_stream.c)
```
Ping-pong dual command buffers:
  - 2 NvRmCommandBuffer structs per stream
  - Each buffer: 256 gather entries, 1024 relocations, 256 waits
  - NVRM_GATHER_TABLE_SIZE = 256
  - NVRM_RELOC_TABLE_SIZE = 1024
  - NVRM_WAIT_TABLE_SIZE = 256
  - NvRmStreamFlush → NvRmPrivFlush → NvRmChannelSubmit → ioctl
```

---

## 4. ISP Register Map (extracted from libnvisp_v3.so binary)

### Extraction Method
Host1x opcodes (INCR/NONINCR) are embedded as literal pool constants in the
Ardbeg prebuilt `libnvisp_v3.so` binary (50KB, ARM Thumb-2).
Opcodes were decoded: `INCR = (1<<28) | (offset<<16) | count`,
`NONINCR = (2<<28) | (offset<<16) | count`.

Disassembly command used:
```
xcrun llvm-objdump -d --triple=thumbv7-linux-gnueabi libnvisp_v3.so
```
(macOS `objdump` cannot decode Thumb-2; `llvm-objdump` with explicit triple is required)

### 24 Unique ISP Register Base Addresses

| Offset | Type | Count | Block | Description |
|--------|------|-------|-------|-------------|
| 0x00C | NONINCR | 1 | Control | ISP control register |
| 0x015 | INCR | 1 | Control | ISP enable/mode |
| 0x100 | INCR | 4 | Color Proc | Color processing control (0x100-0x103) |
| 0x101 | NONINCR | 224 | Tone Curve | Tone curve / gamma LUT (224 entries written to FIFO) |
| 0x200 | NONINCR | 21 | Channel A | Input channel A config (0x200-0x214) |
| 0x300 | NONINCR | 12-137 | Channel B | Input channel B config (0x300-0x388) |
| 0x500 | INCR/NONINCR | 6-25 | Channel C | Processing channel (0x500-0x518) |
| 0x800 | NONINCR | 228-244 | Stats Config | Statistics configuration (0x800-0x8F3) |
| 0x87A | INCR | 169 | Stats/GPP | Histogram / tone mapping (0x87A-0x922) |
| 0x902 | INCR | 8 | Stats | Additional stats registers (0x902-0x909) |
| 0xC41 | INCR | 1-127 | Stats Output | AE/AWB stats output bank 1 (0xC41-0xCBF) |
| 0xC43 | INCR | 1-63 | Stats Output | AE/AWB stats output bank 2 (0xC43-0xC81) |
| 0xC45 | INCR | 127 | Stats Output | Focus stats output (0xC45-0xCC3) |
| 0xC47 | INCR | 15-127 | Stats Output | Histogram stats output (0xC47-0xCC5) |
| 0xC5A | INCR | 67 | Stats Output | Additional stats readback (0xC5A-0xC9C) |
| 0xD31 | INCR | 120 | Lens Shading | Lens shading correction table A (0xD31-0xDA8) |
| 0xDAF | INCR | 174 | Lens Shading | Lens shading correction table B (0xDAF-0xE5C) |
| 0xE00 | INCR | 1 | Output | Output control register 0 |
| 0xE01 | INCR | 1 | Output | Output control register 1 |
| 0xE02 | INCR | 1 | Output | Output control register 2 |
| 0xE30 | INCR | 1 | Output | Output format/size register 0 |
| 0xE31 | INCR | 1 | Output | Output format/size register 1 |
| 0xE32 | INCR | 1 | Output | Output format/size register 2 |
| 0xE33 | INCR | 1 | Output | Output format/size register 3 |

### ISP Register Block Map
```
0x000-0x015  Control / Enable / Mode
0x100-0x1E0  Color Processing + Tone Curve (224-entry LUT FIFO at 0x101)
0x200-0x214  Input Channel A (21 regs — input buffer address, dimensions, format)
0x300-0x388  Input Channel B (137 regs — second input or configuration)
0x500-0x518  Processing Channel (25 regs — demosaic, color conversion)
0x800-0x922  Statistics Configuration (244+ regs — AE/AWB/AF windows, thresholds)
0x87A-0x922  Histogram / Tone Mapping (169 regs)
0x902-0x909  Stats Control (8 regs)
0xC41-0xCC5  Statistics Output (multiple banks, ~500 read-only regs)
0xD31-0xE5C  Lens Shading Correction (294 regs, 2 tables: A=120 regs, B=174 regs)
0xE00-0xE33  Output Control (7 regs — output buffer address, format, dimensions)
```

### 6 ISP Hardware Blocks (from exported function analysis)

| Block | HwSettingsCopy Function | Binary Addr | Size | Register Range |
|-------|------------------------|-------------|------|----------------|
| GPP | NvIspHwSettingsCopyGpp | 0x7ABC | 0x90 | 0x100-0x1E0 (tone curve, gamma) |
| Lens Shading | NvIspHwSettingsCopyLensShading | 0x7CE4 | 0xD0 | 0xD31-0xE5C (2 tables) |
| Demosaic | NvIspHwSettingsCopyDemosaic | 0x8248 | 0xF0 | 0x500 range (Bayer→RGB) |
| Luma Enhancement | NvIspHwSettingsCopyLumaEnhancement | 0x8ACC | 0x114 | Edge/sharpness settings |
| Output Downscaler | NvIspHwSettingsCopyOutputDownScaler | 0x9868 | 0xF8 | Downscaler config |
| Bitwise Op | NvIspHwSettingsCopyBitwiseOperation | 0xA4B0 | 0x84 | Bitwise operations |

**NOTE**: HwSettingsCopy functions only copy data between HwSettings structures in memory.
Actual register programming happens in `NvIspSetConfiguration` (0x1D06-0x3044) and its
sub-functions (0x3044-0x7ABC) which build host1x command buffers.

### Statistics Functions
| Function | Binary Addr | Size | Purpose |
|----------|-------------|------|---------|
| NvIspGetStats | 0xA8F0 | 0x494 | Reads 3A stats from regs 0xC41-0xC5A |
| NvIspSetStats | 0xAD84 | 0x200 | Configures stats windows at regs 0x800-0x902 |

### NvIspSetConfiguration — Main Register Programming Function
Address: 0x1D06-0x3044 (0x133E bytes — largest function in the binary).
Programs all ISP registers via host1x command buffer. Methods used:
- Direct `stm` to pStream->pCurrent (offset 0x4C in stream struct)
- Literal pool opcodes: e.g. `0x1E310001` = INCR(0xE31, 1), `0x10150001` = INCR(0x015, 1)
- Dynamic opcode construction: `lsl r2, r2, #16` then `orr r0, r2, #0x10000000` (INCR)
- Sub-functions at: 0x3044, 0x4564, 0x5D28, 0x5E7A, 0x69CC, 0x7816, 0x7A50

### Platform Init
```
PopulateIspHwFunctions_T12x (0xAF84, 0x1B0 bytes)
  - Populates function pointer table for T124-specific ISP operations
  - Called once during ISP initialization
  - Both Mocha and TN8 binaries use the same function name → same T124 ISP hardware
```

---

## 5. Frame Processing Data Flow

### Stock (Blob) Capture Pipeline
```
1. NvOdmImagerSetSensorMode()    → Configure IMX179/OV5693 via I2C
2. NvViCsiCaptureFrame()         → VI captures RAW Bayer via CSI → DMA buffer
   Device: /dev/nvhost-ctrl-vi
   Syncpoints: VI_FRAME_START(9), VI_MW_REQ_DONE(4), VI_MW_ACK_DONE(6)

3. NvIspProcessFrame()           → ISP processes RAW → RGB/YUV
   Device: /dev/nvhost-isp
   Command buffer: SET_CLASS(0x32) → register writes → syncpoint incr
   ISP blocks applied in order:
     a. GPP (General Purpose Processing — tone curve, gamma)
     b. Lens Shading correction
     c. Demosaic (Bayer → RGB)
     d. Luma Enhancement (edge sharpening)
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

## 6. Mocha vs TN8 Binary Comparison

### libnvisp_v3.so
| | Mocha | TN8 |
|---|---|---|
| Size | 50KB (39 exports) | 63KB (~55 exports) |
| Platform init | PopulateIspHwFunctions_T12x | PopulateIspHwFunctions_T12x (same!) |
| ISP blocks | 6 (GPP, LS, DM, LE, DS, BitOp) | Same 6 blocks |
| Extra in TN8 | — | NvIspProcessFrame3, NvIspGetCapabilities, NvCameraHwPacketReadRegs |

**Both target same T124 ISP hardware. TN8 is newer build with extra utility functions.**

### libnvvicsi_v3.so
| | Mocha | TN8 (T124) |
|---|---|---|
| Size | 17KB (49 exports) | 30KB (61 exports) |
| Mocha-only | NvViCsiSetFlash* (2 funcs) | — |
| TN8-only | — | I2C via VI, Watchdog, DPD mask (12 funcs) |

---

## 7. ISP Calibration Data (from Mocha stock)

### Extracted Profiles
Source: `libnvodm_imager.so` from Xiaomi stock firmware (2.4MB, embedded as plain text).
These are **mocha-specific** calibration data for the actual Primax (rear) and Sunny (front)
camera modules in the Xiaomi Mi Pad.

| Profile | Sensor | Module | Lines | Location |
|---------|--------|--------|-------|----------|
| imx179_primax_lfi_v3.09.isp | IMX179 (rear) | Primax LFI | 6272 | docs/camera-isp-profiles/ |
| imx179_primax_v2.27.isp | IMX179 (rear) | Primax | 6571 | docs/camera-isp-profiles/ |
| imx179_primax_v2.18.isp | IMX179 (rear) | Primax | 6234 | docs/camera-isp-profiles/ |
| ov5693_sunny_v2.13.isp | OV5693 (front) | Sunny | 6560 | docs/camera-isp-profiles/ |

### Profile Parameter Categories (IMX179 v3.09)
| Category | Count | Purpose | Target ISP Registers |
|----------|-------|---------|---------------------|
| lensShading | 1632 | Vignetting correction per channel | 0xD31-0xDA8 (table A), 0xDAF-0xE5C (table B) |
| demosaic | 258 | Bayer→RGB interpolation | 0x500 range |
| noiseReduction | 167 | Per-ISO noise models, filter strengths | Processing channel |
| sharpness | 156 | Edge enhancement | Luma Enhancement block |
| reference | 406 | Reference illuminant color data | Color correction |
| tc (tone curve) | 99 | Gamma / tone mapping | 0x101 (224-entry LUT) |
| awb | 100 | Auto white balance gray line, CCT | Stats config 0x800+ |
| ae | 54 | Auto exposure targets | Stats config 0x800+ |
| colorCorrection | 24 | 3x3 CCM per illuminant | Color processing 0x100+ |
| colorEffects | 26 | Saturation, contrast | GPP block |
| deadPixelCorrection | 60 | Static/dynamic bad pixel thresholds | Input channel |
| colorArtifactReduction | 40+ | Purple fringing, chromatic aberration | Processing |
| falloff_srfc | 100 | Lens falloff surface | Lens shading |

### ISP Profile Search Paths (runtime override)
```
/data/camera_overrides.isp          (rear)
/data/camera_overrides_front.isp    (front)
/system/lib/camera_overrides.isp
/system/lib/camera_overrides_front.isp
/sdcard/camera_overrides.isp
```

### Sensor Hardware Parameters (from JXD vendor source, confirmed for mocha)
```
IMX179 (rear):  3280x2464 8MP, Bayer RGGB, MIPI CSI-A 4 lanes
                MCLK 24MHz, PCLK 348MHz, Line Length 3440px
                Focuser: AD5823 (macro=640, inf=140)
                CCM: [2.084, -0.413, -0.079; -1.102, 1.817, -0.789; 0.017, -0.404, 1.868]

OV5693 (front): 2592x1944 5MP, Bayer BGGR, MIPI CSI-E 1 lane
                Modes: 5MP@30fps, 1080p@30fps, 720p@120fps
                Real gain mode, gain = raw_value / 16.0
```

---

## 8. Key Findings Summary

### What We Have (SOLVED)

| Item | Status | Source |
|------|--------|--------|
| **ISP Register Map** (24 base addresses, ~1600 regs) | **EXTRACTED** | Binary opcode extraction from libnvisp_v3.so |
| **Host1x command buffer API** (full source) | **OBTAINED** | JXD vendor: nvrm_stream.c, nvrm_channel.h |
| **ISP calibration data** (mocha-specific) | **EXTRACTED** | libnvodm_imager.so from Xiaomi stock |
| **Sensor drivers** (IMX179, OV5693, AD5823) | **OBTAINED** | JXD vendor source (full C source) |
| **Camera HAL3 source** | **OBTAINED** | JXD vendor: camera_v3/ |
| **ISP attribute API** (70+ attributes) | **OBTAINED** | JXD vendor: nvcamera_isp.h (49KB) |
| **Host1x class/module IDs** | **CONFIRMED** | JXD: class_ids.h, host1x_module_ids.h |
| **Opcode encoding format** | **CONFIRMED** | hardware_t124.h + nvrm_channel.h |

### What Is Still Unknown

| Item | Status | Impact |
|------|--------|--------|
| **ISP register field definitions** (bit-level) | Unknown | Need runtime tracing or more binary analysis |
| **Exact register-to-calibration mapping** | Partially known | Can be refined via tracing |
| **ISP input/output buffer descriptor format** | Unknown | Regs 0x200 (input) and 0xE00 (output) — need field decode |
| **arisp.h** (NVIDIA internal register header) | Not in any public/leaked source | Would have all field definitions |
| **nvisp_hw_*.cpp** (ISP programming layer source) | Not in JXD leak | Contains the logic we reverse-engineered |

### Corrections Made During Research

1. **NvRmStreamPushIncr**: Previously identified as register write → actually **syncpoint increment**
2. **NvIspHwSettingsCopy***: Previously thought to write registers directly → actually just **copy between structs** in memory
3. **Register offsets from orr instructions**: Early analysis found 0x0E6, 0x0EC etc. → these were **debug line numbers**, not register offsets
4. **vi_bypass.c**: Only for T210 (`nvidia,tegra210-vi-bypass`), NOT for T124

---

## 9. JXD S192 Vendor Source Reference

Repository: `github.com/Project-Google-Tango/vendor_nvidia_jxd_src`
Local clone: `/Users/artem/Projects/vendor_nvidia_jxd_src/`

### What was found:
| Component | Path | Notes |
|-----------|------|-------|
| NvRM Graphics (libnvrm_graphics.so) | `tegra/core/drivers/nvrm/graphics/` | Full source: nvrm_stream.c (37KB), nvrm_channel_linux.c (42KB) |
| NvRM Channel API | `tegra/core/include/nvrm_channel.h` | 75KB, all NVRM_CH_OPCODE_* macros, public API |
| Camera HAL3 | `tegra/camera-partner/android/libnvcamerategra/camera_v3/` | Full source |
| Sensor drivers | `tegra/camera-partner/imager/sensor_bayer_imx179.c` | Mode tables, register sequences |
| Sensor drivers | `tegra/camera-partner/imager/sensor_bayer_ov5693.c` | Mode tables, register sequences |
| Focuser driver | `tegra/camera-partner/imager/focuser_ad5823.c` | Full VCM control source |
| ISP calibration (C headers) | `tegra/camera-partner/imager/configs/` | sensor_bayer_imx179_camera_config.h (194KB) |
| ISP high-level API | `tegra/camera/core_v3/include/nvcamera_isp.h` | 49KB, 70+ ISP attributes |
| Host1x class IDs | `tegra/hwinc/t12x/class_ids.h` | ISP A=0x32, ISP B=0x34 |
| Host1x module IDs | `tegra/hwinc/t12x/host1x_module_ids.h` | ISP A=0x18, ISP B=0x1A |

### What was NOT found:
| Component | Expected Location | Notes |
|-----------|------------------|-------|
| ISP register headers (arisp.h) | `tegra/hwinc/t12x/` | NVIDIA does not ship these publicly |
| ISP HW programming (nvisp_hw_*.cpp) | `tegra/camera/` | The compiled version is libnvisp_v3.so |
| libnvisp_v3.so source | `tegra/camera/` | Only prebuilt binary available |

### ABI Compatibility Warning
JXD sources are **KitKat 4.4 era** (same as Mocha). They cannot be directly compiled
against the TN8 (Lollipop+) blob stack. The JXD sources are valuable as **documentation
and reference**, not as directly buildable code for SmokeR24.1.

| | Mocha blobs | JXD sources | TN8 blobs | SmokeR24.1 kernel |
|---|---|---|---|---|
| Android | 4.4 | 4.4 | 5.x-7.x | 5.x-7.x |
| GCC | 4.4.3 | 4.9 | 4.9 | 4.9 |
| ABI | Old | Old | New | New |

**None of the three blob sources (mocha/jxd/tn8) can be directly used:**
- Mocha blobs: old ABI, incompatible with SmokeR24.1 kernel
- JXD sources: old API level, no build headers for TN8 stack
- TN8 blobs: right ABI, but no IMX179/OV5693 mocha-specific support

This is why the **V4L2 + kernel ISP driver** approach is recommended (see section 10).

---

## 10. Implementation Plan: V4L2 + Kernel ISP Driver

### Current Status (April 2026)

| Step | Status | Notes |
|------|--------|-------|
| Step 1: Device Tree + Power | **COMPLETE** | Both cameras + focuser |
| Step 2: V4L2 RAW Capture | **COMPLETE** | IMX179 + OV5693 + AD5823 via MC framework |
| Step 3: Minimal ISP | **NOT STARTED** | Next step — kernel ISP entity |
| Step 4: Full ISP Calibration | **NOT STARTED** | Load mocha ISP profiles |
| Step 5: 3A Statistics | **NOT STARTED** | AE/AWB/AF |

**All cameras work without ISP**, producing RAW Bayer frames directly via V4L2:
- IMX179 rear: RGGB 10-bit, 3 modes (3280x2460, 1920x1080, 1280x720@90)
- OV5693 front: BGGR 10-bit, 6 capture + 2 HDR modes
- AD5823 focuser: VCM control via lens channel in MC graph

### Architecture: ISP as MC Entity

ISP integrates into the existing Media Controller graph as another entity
in the pipeline, similar to how sensor and focuser entities are connected:

**Current pipeline (RAW only):**
```
[sensor entity] → link → [VI channel entity] → link → [video0: RAW Bayer]
[focuser entity] → link → [lens channel: controls only]
```

**Target pipeline (with ISP):**
```
[sensor entity] → link → [VI channel entity] → link → [ISP entity] → link → [video0: YUV]
[focuser entity] → link → [lens channel: controls only]
```

ISP is a V4L2 subdev registered via `v4l2_async_register_subdev`, with:
- SINK pad: accepts RAW Bayer from VI channel
- SOURCE pad: outputs processed YUV/RGB
- Configured via V4L2 subdev controls and ISP profile data
- Same `media_entity_create_link()` as sensor/focuser links
- DTS node already exists: `isp@54600000` (ISP A), `isp@54680000` (ISP B)

The ISP entity receives RAW frames from VI, processes them through
the hardware pipeline (demosaic → WB → CCM → gamma → color space),
and outputs YUV. Userspace sees the same `/dev/video0` interface
but gets processed frames instead of RAW Bayer.

### Why Kernel ISP Driver (not userspace library)

| Factor | Kernel driver | Userspace .so |
|--------|--------------|---------------|
| Consumer | V4L2 — any Linux app | Only with matching blob stack |
| Blob dependency | None | Must match libnvmm_camera ABI |
| Host1x API | Kernel nvhost_* already available | Need custom NvRm reimplementation |
| Debugging | printk, ftrace, debugfs | strace, gdb |
| Integration | Fits existing MC framework | Need custom pipeline |
| Future | Standard Linux approach | Dead end |

### What Already Exists in Kernel

| Component | File | Status |
|-----------|------|--------|
| V4L2 video device + buffer queues | `drivers/media/platform/tegra/camera/channel.c` | **WORKING** |
| Media controller graph | `drivers/media/platform/tegra/camera/graph.c` | **WORKING** |
| VI capture driver (T124) | `drivers/media/platform/tegra/vi/vi.c` | **WORKING** |
| CSI transceiver | `drivers/media/platform/tegra/csi/csi.c` | **WORKING** |
| MIPI calibration (T124) | `drivers/media/platform/tegra/mipical/mipi_cal_t124.c` | **WORKING** |
| T124 register headers | `drivers/media/platform/tegra/camera/t124_registers.h` | **WORKING** |
| T210 register headers | `drivers/media/platform/tegra/camera/t210_registers.h` | **WORKING** |
| ISP host1x client (power/clock) | `drivers/video/tegra/host/isp/isp.c` | Ready (no register programming) |
| ISP interrupt handler | `drivers/video/tegra/host/isp/isp_isr_v1.c` | Ready |
| **IMX179 V4L2 subdev (mocha)** | **`drivers/media/i2c/imx179_mocha.c`** | **WORKING** |
| **OV5693 V4L2 subdev (mocha)** | **`drivers/media/i2c/ov5693_mocha.c`** | **WORKING** |
| **AD5823 V4L2 focuser (mocha)** | **`drivers/media/i2c/ad5823_mocha.c`** | **WORKING** |
| ISP calibration profiles | `docs/camera/isp-profiles/*.isp` | Extracted from stock |
| ISP register map | Section 4 of this document | Fully extracted |
| Diagnostic tool | `tools/camera/v4l2_diag.c` | **WORKING** (capture + focus + exposure) |

### What Needs To Be Written

1. **ISP V4L2 subdev entity** — `drivers/media/platform/tegra/isp/isp_t124_mc.c`
   - V4L2 subdev with SINK (RAW input) and SOURCE (YUV output) pads
   - Register in MC graph between VI channel and video output device
   - Program ISP registers via host1x command buffers
   - Load calibration data from ISP profile files

2. **ISP register programming** — using register map from section 4
   - Opens host1x channel for ISP-A (class 0x32)
   - Builds command buffers: SET_CLASS(0x32) → enable → input → processing → output
   - Loads calibration data (lens shading, tone curves, CCM) from mocha ISP profiles
   - Submits via nvhost API, waits for syncpoint

3. **DTS integration**
   - Add ISP entity endpoints to mocha camera DTS
   - Link VI channel → ISP → video output in MC graph
   - ISP profile path reference in DTS

### Step-by-Step Plan

```
Step 1: Device Tree + Power Sequence — COMPLETE
  - [x] OV5693 front camera with CSI-E, 1-lane
  - [x] IMX179 rear camera with CSI-A, 4-lane
  - [x] AD5823 focuser with lens channel
  - [x] DSI MIPI calibration fix

Step 2: V4L2 RAW Capture (no ISP) — COMPLETE
  - [x] ov5693_mocha.c — front camera driver
  - [x] imx179_mocha.c — rear camera driver
  - [x] ad5823_mocha.c — focuser driver
  - [x] channel.c PORT_A + PORT_B T124 bypass
  - [x] Syncpt fix (MWA=6, MWB=7)
  - [x] Register headers split (common/t124/t210)
  - [x] writel→tegra_channel_write cleanup
  - [x] OTP readout for both sensors

Step 3: Minimal ISP (demosaic + output) — NEXT
  - Write ISP V4L2 subdev entity for MC graph
  - Minimum register programming:
    SET_CLASS(0x32) → enable(0x015) → input(0x200) →
    tone_curve(0x101, linear) → output(0xE00, 0xE30-0xE33)
  - Test: ISP produces viewable YUV output from v4l2_diag

Step 4: Full ISP Calibration
  - Load mocha calibration profiles:
    lens_shading(0xD31-0xE5C) → tone_curve(0x101) → CCM
  - Test: good quality image comparable to stock

Step 5: 3A Statistics
  - Configure stats engine (0x800-0x922)
  - Read AE/AWB/AF stats (0xC41-0xCC5)
  - Implement basic auto-exposure/white-balance in userspace
```

---

## 11. File Index

### Documentation:
- `docs/camera-isp-reverse-engineering.md` — this file (ISP register map + reverse engineering)
- `docs/camera-reverse-engineering.md` — camera architecture overview
- `docs/camera-isp-profiles/` — extracted ISP calibration data from Xiaomi stock

### Kernel (existing):
- `drivers/video/tegra/host/isp/isp.c` — ISP host1x platform driver
- `drivers/video/tegra/host/isp/isp_isr_v1.c` — ISP interrupt handler
- `drivers/video/tegra/host/t124/hardware_t124.h` — host1x opcode definitions
- `drivers/video/tegra/host/class_ids.h` — ISP class IDs
- `drivers/media/platform/soc_camera/tegra_camera/vi2.c` — V4L2 VI host (T124)
- `drivers/media/platform/tegra/camera/channel.c` — V4L2 video device
- `drivers/media/platform/tegra/camera/graph.c` — media controller graph
- `drivers/media/platform/tegra/csi/csi.c` — CSI transceiver
- `drivers/media/i2c/soc_camera/ov5693_v4l2.c` — OV5693 V4L2 subdev
- `drivers/media/i2c/soc_camera/imx135_v4l2.c` — IMX135 V4L2 subdev (template for IMX179)
- `drivers/media/platform/tegra/imx179.c` — IMX179 legacy miscdevice driver
- `drivers/media/platform/tegra/ov5693.c` — OV5693 legacy miscdevice driver
- `drivers/media/platform/tegra/ad5823.c` — AD5823 focuser legacy driver
- `arch/arm/boot/dts/tegra124-soc-base.dtsi` — VI/ISP device tree nodes

### Stock blobs (reference only):
- `libnvisp_v3.so` (50KB) — ISP HW programming, 6 blocks, 39 exports
- `libnvvicsi_v3.so` (17KB) — VI/CSI HW programming
- `libnvmm_camera_v3.so` (1.4MB) — pipeline orchestrator, 3A
- `libnvrm_graphics.so` (21KB) — host1x command buffer — **SOURCE OBTAINED**
- `libnvodm_imager.so` (2.4MB) — sensor drivers + ISP calibration

### JXD S192 vendor source:
- `tegra/core/drivers/nvrm/graphics/nvrm_stream.c` — NvRmStream* full source (37KB)
- `tegra/core/drivers/nvrm/graphics/nvrm_channel_linux.c` — NvRmChannel* Linux impl (42KB)
- `tegra/core/include/nvrm_channel.h` — Public API + opcode macros (75KB)
- `tegra/camera/core_v3/include/nvcamera_isp.h` — ISP attribute API (49KB)
- `tegra/camera-partner/imager/sensor_bayer_imx179.c` — IMX179 sensor driver source
- `tegra/camera-partner/imager/sensor_bayer_ov5693.c` — OV5693 sensor driver source
- `tegra/camera-partner/imager/focuser_ad5823.c` — AD5823 focuser driver source
- `tegra/camera-partner/imager/configs/sensor_bayer_imx179_camera_config.h` — ISP cal (194KB)
- `tegra/camera-partner/android/libnvcamerategra/camera_v3/` — Camera HAL3 source
