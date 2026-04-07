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
| Step 3: Minimal ISP | **IN PROGRESS** | ISP entity in MC graph, host1x submit works, DMA confirmed on stock |
| Step 4: Full ISP Calibration | **NOT STARTED** | Stock calibration captured (binary blobs ready) |
| Step 5: 3A Statistics | **NOT STARTED** | Stats readback mechanism decoded (DMA buffer at +0x20000) |

**ISP T124 MC driver** (`drivers/media/platform/tegra/camera/isp_t124.c`):
- V4L2 subdev registered in MC graph
- Host1x channel + syncpoint working (ping 438 µs)
- All ISP methods probed and accepted
- Separate from legacy `isp.c` (stock, untouched)
- Called from legacy `isp_probe()` via `tegra_isp_t124_mc_init()` hook

**ISP DMA confirmed** on stock kernel via userspace test (`tools/camera/isp_test.c`):
- ISP-A (IMX179): full BGRA output with trigger 0x05
- ISP-B (OV5693): output confirmed with trigger 0x05
- Requires: stock calibration block + stock dimensions + relocs for SMMU

**Gather filter**: SmokeR24.1 kernel has `t124_channel_init_gather_filter` enabled.
Stock kernel does not. ISP driver must not use SET_CLASS inside gathers —
use `class_id` parameter in `nvhost_job_add_client_gather_address()` instead.

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

## 11. Stock Command Buffer Capture (April 2026)

### Method

Modified Smoke kernel 1.2 (tag `1.2` from `Insei/Smoke-kernel-mocha`) with ISP
cmdbuf hex dump in `nvhost_cdma.c` `trace_write_gather` path. Built with
Linaro 4.9.4, flashed to device, launched stock camera app.

### Per-Frame ISP Command Buffer Structure

Each frame consists of 6 submits in order:

| Submit | Words | Content |
|--------|-------|---------|
| 1 | 2 | Syncpt increment (host1x class) |
| 2 | 45 | **Output config + surfaces + input + trigger** |
| 3 | 2 | Syncpt increment |
| 4 | 8 | Syncpt wait (host1x WAIT_SYNCPT for VI completion) |
| 5 | 2 | Syncpt increment |
| 6 | ~1545 | **Full ISP calibration (lens shading, tone curve, etc.)** |

### Decoded 45-Word Output Block (ISP-A, IMX179 3280x2460)

```
[000] 0x00000C80 = SET_CLASS(0x32)
[001] 0x1E000001 = INCR(0xE00, 1)
[002] 0x0CCF0000 = ((width-1) & 0x3FFF) << 16 = (3279 << 16)
[003] 0x1E010001 = INCR(0xE01, 1)
[004] 0x099B0000 = ((height-1) & 0x3FFF) << 16 = (2459 << 16)
[005] 0x1E020001 = INCR(0xE02, 1)
[006] 0x04FE00E6 = output format (0xE6) + flags (0x04FE00)
[007] 0x1E030001 = INCR(0xE03, 1)
[008] 0x00000000 = color space params

[009] 0x1E040003 = INCR(0xE04, 3) — output surface Y plane
[010] IOVA address (changes per frame)
[011] 0x00000000
[012] 0x00000D00 = stride 3328 (3280 aligned to 64)

[013] 0x1E070003 = INCR(0xE07, 3) — output surface U plane
[014] IOVA address
[015] 0x00000000
[016] 0x00000680 = stride 1664 (half Y stride)

[017] 0x1E0A0003 = INCR(0xE0A, 3) — output surface V plane
[018] IOVA address
[019] 0x00000000
[020] 0x00000680 = stride 1664

[021] 0x15000006 = INCR(0x500, 6) — processing/demosaic
[022-026] 0x00000000 (5 zeros)
[027] 0x099C0CD0 = (height << 16) | width = (2460 << 16) | 3280

[028] 0x00000C80 = SET_CLASS(0x32)
[029] 0x11000004 = INCR(0x100, 4) — input buffer
[030] IOVA address (changes per frame)
[031-033] 0x00000000

[034-035] SET_CLASS(0x32)
[036-041] 3x NONINCR(0x000, 1) — syncpt increments (host1x method)
         values: 0x420, 0x521, 0x623 (syncpt IDs + conditions)

[042] SET_CLASS(0x32)
[043] 0x200C0001 = NONINCR(0x00C, 1) — control trigger
[044] 0x00000005 = trigger value
```

### ISP-B Comparison (OV5693 2592x1944)

```
SET_CLASS: 0x0D00 (class 0x34 = ISP-B)
0xE00: 0x0A1F0000 = (2591 << 16)
0xE01: 0x07970000 = (1943 << 16)
0xE02: 0x04FE00E6 = same format
Y stride: 0x0A40 (2624, 2592 aligned to 64)
UV stride: 0x0540 (1344)
0x500 dims: 0x07980A20 = (1944 << 16) | 2592
0x00C trigger: 0x05 = same
Calibration: different values (per-sensor), same structure
```

### Surface Descriptor Format

```
INCR(0xE04 + 3*i, 3):
  Word 0: IOVA buffer address
  Word 1: 0 (always zero in stock)
  Word 2: stride in bytes (aligned to 64)
```

3 surfaces for YUV planar: Y at 0xE04, U at 0xE07, V at 0xE0A.
Input at 0xE34+3*i follows same format (confirmed from RE).

### Syncpt Wait Block (8 words)

```
[0] 0x00000040 = SET_CLASS(0x01) — host1x class
[1] 0x20080001 = NONINCR(0x008, 1) — WAIT_SYNCPT
[2] syncpt_id | (value << 8)
[3] SET_CLASS(ISP) — back to ISP class
[4-7] repeat for second syncpt wait
```

### Per-Frame Dynamic Values

Only buffer addresses change per frame (rotating pool):
- Output Y/U/V addresses ([010], [014], [018])
- Input address ([030])

All register values, format codes, strides, calibration — **constant** across frames.

### MMIO Register Values During Streaming

```
Method  MMIO    ISP-A (rear)    ISP-B (front)   Description
0x008   0x20    0xF000F800      0xF000F800      Input config (hw default)
0x00C   0x30    0x00000004      0x00000004      Control (streaming state)
0x00D   0x34    0x00000100      0x00000100      Status
0x015   0x54    0x04040007      0x04040007      ISP enable + mode
0x018   0x60    0x0A00500A      0x0A00500A      Processing params
0x019   0x64    0x00008089      0x00008089      Processing params
0x01A   0x68    0x013645CB      0x013645CB      Calibration
0x01B   0x6C    0x000001E7      0x000001E7      Calibration
0x01C   0x70    0x00000001      0x00000001      Clock gate
0x01D   0x74    0x00000001      0x00000001      ISP_CG_CTRL
0x01F   0x7C    0x00000003      0x00000003      Mode
```

Note: 0x00C shows 0x04 during streaming, not 0x05 (trigger value) or 0x0F
(post-apply). This suggests 0x04 is the "streaming active" state after
the trigger write completes.

### Calibration Block Structure (~1545 words for ISP-A, ~1538 for ISP-B)

Fully decoded in `docs/camera/isp-calibration-decoded.md`.

```
SET_CLASS(ISP)
INCR(0xD00, 10)      — Lens shading control (per-sensor coefficients)
INCR(0xD0A, 1)       — Lens shading enable
NONINCR(0xD0B, 480)  — Lens shading correction table (FIFO, 4ch x 120 points)
INCR(0xD20, 6)       — Lens shading extra (ISP-A only, absent in ISP-B)
4x {
  INCR(0x65N, 1)     — Tone curve channel N control
  NONINCR(0x65(N+1), 257) — Tone curve channel N LUT (FIFO)
}
INCR(0x053, 2)       — ISP enable + buffer address
```

Note: Tone curve methods (0x651-0x658) differ from original RE (0x101).
Lens shading methods (0xD00-0xD20) differ from original RE (0xD31/0xDAF).
This is because stock uses a different binary (Shield 63KB) vs original
RE (Mocha 50KB), or different code paths.

Stock tone curves are **all linear** (0x1000 = 1.0 for all 257 entries).
Real gamma/tone mapping would come from ISP calibration profiles in
`docs/camera-isp-profiles/`:
- `imx179_primax_lfi_v3.09.isp` — IMX179 rear (latest)
- `imx179_primax_v2.27.isp` — IMX179 rear
- `imx179_primax_v2.18.isp` — IMX179 rear
- `ov5693_sunny_v2.13.isp` — OV5693 front

### Key Corrections from Stock Capture

| What | Our assumption | Stock reality |
|------|---------------|---------------|
| SET_CLASS in gather | Blocked by filter | Works in stock kernel |
| Surface word order | [addr, stride, dims] | [addr, 0, stride] |
| Output format | Simple | YUV planar (3 surfaces Y/U/V) |
| 0x500 processing | All zeros | 5 zeros + (h<<16)\|w |
| 0x00C trigger | 0x0F or 0x01 | 0x05 |
| 0x015 enable | 0x01 | 0x04040007 |
| Input via 0x200 | DMA descriptor | Wrong — 0x200 is coefficients |
| Input via 0x100 | reloc + zeros | Confirmed: INCR(0x100,4) |

### Stock Firmware Build for ISP Tracing

Source: `Insei/Smoke-kernel-mocha` tag `1.2`
Modification: unconditional ISP cmdbuf hex dump in `nvhost_cdma.c`
Toolchain: Linaro 4.9.4 (same as SmokeR24.1)
Ramdisk: extracted from stock boot partition via `dd`
Built with: `/home/artem/Projects/Smoke-kernel-mocha/` on build server

---

## 13. ISP DMA Breakthrough — Dual Trigger Discovery (April 2026)

### Summary

ISP requires **two-phase trigger** to process a frame:
1. **Trigger 0x0F** (static config apply) — loads calibration into ISP pipeline
2. **Trigger 0x05** (runtime frame processing) — starts DMA read/write

Both must be sent via `NONINCR(0x00C, 1)` in sequential host1x submits.
Trigger 0x0F alone produces no output. Trigger 0x05 alone (without prior 0x0F)
produces no output. Only 0x0F followed by 0x05 produces ISP DMA output.

### Discovery Method

Running `isp_test tests` on stock Smoke kernel 1.2 (cold boot, no camera app):

```
Test 1: trigger=0x0F, format=0x04FE00E6 → output: 0/4096 non-zero (untouched)
Test 2: trigger=0x05, format=0x04FE00E6 → output: 4096/4096 non-zero (ISP WROTE DATA!)
  hex: 86 63 d9 ff 86 63 d9 ff 86 63 d9 ff ...  (BGRA pattern)
```

Test 1 (0x0F) initialized ISP but produced no output.
Test 2 (0x05) ran on already-initialized ISP and produced DMA output.

### Test Conditions

- **Kernel**: Stock Smoke 1.2, unmodified, freshly flashed MiuiSmoke_V8_MiPad_7.2.9_4.4.4
- **Boot state**: Cold boot, NO camera app launched, NO prior ISP init
- **Power**: ISP power-on via `open(/dev/nvhost-isp)` → `nvhost_module_busy()`
- **Calibration**: Loaded from `/data/local/tmp/isp_cal.bin` (1545 words, stock ISP-A)
- **Input buffer**: Uninitialized (nvmap mmap failed on stock, using zero-filled memory)
- **Output buffer**: nvmap IOVMM allocation, checked via `NVMAP_IOC_READ`
- **Relocs**: 4 relocs (3 output Y/U/V + 1 input), patched by kernel at submit
- **Syncpt**: ISP-A syncpt 32, OP_DONE condition
- **Submit**: Single gather with SET_CLASS(0x32) + calibration + output config + trigger

### Cold Boot Verification

Confirmed ISP does NOT work on cold boot with single trigger:

```
Cold boot + trigger 0x05 only → output untouched
Cold boot + trigger 0x0F only → output untouched
Cold boot + trigger 0x0F then 0x05 (sequential submits) → ISP WROTE DATA!
```

Also confirmed on SmokeR24.1 kernel:
```
24.1 kernel + trigger 0x05 only → output untouched (tested extensively)
24.1 kernel + MMIO init + trigger 0x05 → output untouched
24.1 userspace isp_test_24 + relocs + trigger 0x05 → output untouched
```

### MMIO Register State

**Cold boot (power gated)**: All 0xFFFFFFFF — ISP completely off

**After power-on (pre-init)**: All 0x00000000 — hardware defaults

**During stock camera streaming** (captured via devmem):
```
Method  MMIO    Value           Description
0x008   0x020   0xF000F800      Input config
0x00C   0x030   0x00000004      Control (streaming active state)
0x00D   0x034   0x00000100      Status
0x014   0x050   0x000000A9      Per-sensor parameter
0x015   0x054   0x04040007      ISP enable mode
0x018   0x060   0x0A00500A      Processing params
0x019   0x064   0x00008089      Processing params
0x01A   0x068   0x013645CB      Calibration coefficient
0x01B   0x06c   0x000001E7      Calibration coefficient
0x01C   0x070   0x00000001      Unknown
0x01D   0x074   0x00000001      ISP_CG_CTRL (clock gating)
0x01F   0x07c   0x00000003      Mode
0x024   0x090   0xC6BFF67C      Unknown (same for A and B)
0x038   0x0e0   0x242CB07B      Unknown
0x03F   0x0fc   0x00000020      Unknown
0x051   0x144   0x017BA537      Unknown
0x053   0x14C   0x00000001      ISP_ENABLE
0x054   0x150   0x00585B18      Working buffer address (IOVA)
0x05E   0x178   0x00003232      Unknown
```

**After camera close**: Immediately 0xFFFFFFFF — ISP power gated by nvhost idle.

### Key Finding: ISP Power Gate Timing

After `am force-stop` camera app, ISP is **immediately** power gated.
Register 0x054 = 0xFFFFFFFF within 1 second of camera close.
Previous successful tests likely had camera still partially active,
or ISP power gate was slower on earlier firmware.

### Ruled Out Hypotheses

| Hypothesis | Result |
|-----------|--------|
| Gather filter blocks ISP gathers | NO — disabled filter, same result |
| Need relocs for SMMU mapping | NO — userspace with relocs, same result |
| Need contiguous Y/U/V buffer | NO — tested contiguous, same result |
| Need valid ISP working buffer (0x054) | NO — patched address, same result |
| Need MMIO pre-init | NO — wrote all known MMIO values, same result |
| Single trigger 0x05 sufficient | NO — needs 0x0F first |
| ISP works only with real RAW data | NO — works with uninitialized input |
| Problem specific to 24.1 kernel | NO — same on stock without dual trigger |

### Trigger Semantics

```
0x0F = NvCameraHwSettingsApply post-apply callback (static config commit)
       Loads calibration (lens shading, tone curves) into ISP pipeline registers.
       Must be sent BEFORE runtime trigger. Does not produce output.

0x05 = NvIspProcessFrame3 runtime trigger (frame processing)
       Starts ISP DMA: reads input buffer, processes, writes output Y/U/V.
       Only works AFTER 0x0F has been sent on the same channel.

0x04 = Hardware state during active streaming (read from MMIO 0x030).
       Not a trigger value — this is the ISP "busy" state.

0x09 = Unknown runtime mode (tested, no output)
```

### Stock Per-Frame Submit Sequence (6 submits)

From stock cmdbuf capture (Section 11), each frame has 6 submits:
```
Submit 1: 2 words  — Syncpt increment
Submit 2: 45 words — Output config + surfaces + input + trigger 0x05
Submit 3: 2 words  — Syncpt increment
Submit 4: 8 words  — Syncpt WAIT (wait for VI frame)
Submit 5: 2 words  — Syncpt increment
Submit 6: ~1545 words — Calibration + trigger 0x0F (via static callback)
```

Note: trigger 0x0F (submit 6) comes AFTER trigger 0x05 (submit 2) in the
per-frame sequence. This means on the FIRST frame, 0x0F was already sent
during `NvIspSetConfiguration` (static init phase). Subsequent frames
send 0x05 first (new frame), then 0x0F (update calibration for next frame).

### Historical Note

Commit `4b45d0c3eb7` "ISP DMA CONFIRMED WORKING" (on stock kernel) used
`isp_test tests` which runs test suite: trigger 0x0F first, then 0x05.
The dependency was not noticed at the time — both triggers appeared to
"work" independently, but in reality 0x05 only worked because 0x0F had
already been sent in the previous test. This is confirmed by running
each trigger independently on cold boot (both produce zero output).
   - Submit 1: calibration + output config + trigger 0x0F (init)
   - Submit 2: input + output surfaces + trigger 0x05 (process)
2. Test on 24.1 kernel with dual trigger
3. If works: integrate into VI capture pipeline (channel.c)

---

## 14. File Index (updated)

### Documentation:
- `docs/camera/camera-isp-reverse-engineering.md` — this file
- `docs/camera/camera-reverse-engineering.md` — camera architecture overview
- `docs/camera/isp-calibration-decoded.md` — decoded calibration block structure
- `docs/camera/isp-reverse-engineering-status.md` — current RE status for assistant
- `docs/camera/isp-stats-readback.md` — stats readback mechanism
- `docs/camera-isp-profiles/` — extracted ISP calibration data from Xiaomi stock (.isp text files)

### Stock Command Buffer Captures:
- `docs/camera/stock-isp-a-cmdbuf-dump.txt` — full ISP-A dmesg trace (IMX179 rear)
- `docs/camera/stock-isp-b-cmdbuf-dump.txt` — full ISP-B dmesg trace (OV5693 front)
- `docs/camera/stock-isp-a-calibration.txt` — ISP-A calibration block (1545 words)
- `docs/camera/stock-isp-b-calibration.txt` — ISP-B calibration block (1538 words)
- `docs/camera/stock-isp-mmio-rear.txt` — MMIO register dump during rear streaming
- `docs/camera/stock-isp-mmio-front.txt` — MMIO register dump during front streaming

### Kernel (SmokeR24.1, isp/v4l2-driver branch):
- `drivers/media/platform/tegra/camera/isp_t124.c` — T124 ISP MC driver (V4L2 subdev + host1x + debugfs)
- `drivers/media/platform/tegra/camera/isp_t124.h` — ISP method offsets and stock values
- `drivers/media/platform/tegra/camera/isp_t124_cal.h` — stock calibration data arrays
- `drivers/video/tegra/host/isp/isp.c` — legacy nvhost ISP (stock + mc_init hook)
- `drivers/media/platform/tegra/camera/channel.c` — V4L2 video device + ISP channel
- `drivers/media/platform/tegra/camera/graph.c` — media controller graph + ISP entity
- `drivers/media/platform/tegra/camera/mc_common.h` — is_isp_channel flag
- `arch/arm/boot/dts/tegra124-platforms/tegra124-mocha-camera-mc.dtsi` — camera DTS

### Tools:
- `tools/camera/v4l2_diag.c` — capture diagnostic tool (existing, works)
- `tools/camera/isp_test.c` — userspace ISP test for stock kernel (nvmap + nvhost ioctl)
- `tools/camera/isp_test_24.c` — userspace ISP test adapted for SmokeR24.1 (nvmap write/read)
- `tools/camera/isp_init_test.c` — ISP blob init test (NvIspOpen via vendor blob)

### Stock blobs (reference only):
- `libnvisp_v3.so` — ISP HW programming (Shield 63KB variant used for RE)
- `libnvmm_camera_v3.so` (1.4MB) — pipeline orchestrator, 3A
- `libnvrm_graphics.so` (21KB) — host1x command buffer — **SOURCE OBTAINED**
- `libnvodm_imager.so` (2.4MB) — sensor drivers + ISP calibration

### JXD S192 vendor source:
- `tegra/core/drivers/nvrm/graphics/nvrm_stream.c` — NvRmStream* full source (37KB)
- `tegra/core/include/nvrm_channel.h` — Public API + opcode macros (75KB)
- `tegra/core/include/nvrm_surface.h` — NvRmSurface (0x30 bytes)
- `tegra/multimedia-partner/nvmm/include/nvmm_buffertype.h` — NvMMSurfaceDescriptor (0xB0)
- `tegra/camera/core_v3/include/nvcamera_isp.h` — ISP attribute API (49KB)
- `tegra/camera-partner/imager/configs/sensor_bayer_imx179_camera_config.h` — ISP cal (194KB)
- `tegra/camera-partner/android/libnvcamerategra/camera_v3/` — Camera HAL3 source
