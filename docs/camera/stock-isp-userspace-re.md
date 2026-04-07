# Stock ISP Userspace Stack — Reverse Engineering Report

Source: `MiuiSmoke_V8_MiPad_7.2.9_4.4.4.zip` (Tegra K1, Android 4.4.4, Smoke kernel 1.2)
Decompiled with Ghidra headless, 7940 functions across 9 libraries.

## Library Architecture

```
camera.tegra.so (HAL)               — 1.1M, 2373 funcs
  → libnvmm_camera_v3.so            — 1.4M, 3252 funcs (NvMM camera block)
    → libnvisp_v3.so                 — 49K,  178 funcs  (ISP v3 API)
      → libnvrm_graphics.so          — 21K,  203 funcs  (NvRmStream/Channel impl)
        → libnvrm.so                 — 45K,  303 funcs  (nvmap memory)
          → libnvos.so               — 49K,  417 funcs  (ioctl/mmap wrappers)
            → /dev/nvhost-isp.N      (kernel host1x channel)
            → /dev/nvhost-ctrl-isp   (clock/bandwidth control)
            → /dev/nvmap             (buffer management)
```

**NOT camera-related:** `libnvcap.so` = Miracast/Wi-Fi Display screen capture.

---

## ISP v3 Method Map (DAT_ values resolved from binary)

All values confirmed by extracting literal pool data from libnvisp_v3.so via Ghidra.

### Output block

| Method | Opcode (hex) | Purpose | Format |
|--------|-------------|---------|--------|
| 0xE00 | `0x1E000001` INCR(0xE00,1) | Output width | `((W-1) & 0x3FFF) << 16` |
| 0xE01 | `0x1E010001` INCR(0xE01,1) | Output height | `((H-1) & 0x3FFF) << 16` |
| 0xE02 | `0x1E020001` INCR(0xE02,1) | Output format | format_word (e.g. 0x04FE00E6) |
| 0xE03 | `0x1E020001+0x10000` | Output digital gain/crop | via INCR(0xE02+1,1) |
| 0xE04 | `0x1E040003` INCR(0xE04,3) | Output Y plane | [IOVA, 0, stride] |
| 0xE07 | `0x1E070003` INCR(0xE07,3) | Output U plane | [IOVA, 0, stride] |
| 0xE0A | `0x1E0A0003` INCR(0xE0A,3) | Output V plane | [IOVA, 0, stride] |

### Input block (v3-specific, NOT 0x100!)

| Method | Opcode (hex) | Purpose | Format |
|--------|-------------|---------|--------|
| 0xE31 | `0x1E310001` INCR(0xE31,1) | Input dims | `width & 0x7FFF \| (height << 16)` |
| 0xE33 | `0x1E330001` INCR(0xE33,1) | Input format | pixel format descriptor |
| 0xE34 | `0x1E340003` INCR(0xE34,3) | Input plane 0 | [IOVA, 0, stride] via RELOC |
| 0xE37 | `0x1E370003` INCR(0xE37,3) | Input plane 1 | [IOVA, 0, stride] (multi-plane) |
| 0xE3A | `0x1E3A0003` INCR(0xE3A,3) | Input plane 2 | [IOVA, 0, stride] (multi-plane) |
| 0xE32 | `0x1E320001` INCR(0xE32,1) | Input strip cfg | `strip_width & 0x3FFF \| (overlap << 16)` |
| 0xE30 | `0x1E300001` INCR(0xE30,1) | Input trigger | value = 1 (fire!) |

### Control/processing

| Method | Opcode (hex) | Purpose | Values |
|--------|-------------|---------|--------|
| 0x500 | `0x15000006` INCR(0x500,6) | Processing block | [0,0,0,0,0, (H<<16)\|W] |
| 0x015 | `0x10150001` INCR(0x015,1) | ISP_ENABLE | 7 (full pipeline) |
| 0x00C | `0x200C0001` NONINCR(0x00C,1) | ISP CONTROL | 0x05=runtime, 0x0F=post-apply |
| 0x100 | `0x11000004` INCR(0x100,4) | **Stats buffer** (NOT input!) | [IOVA, 0, 0, 0] |

### ISP_ENABLE cached values
- `7` = full processing pipeline enabled
- `0x04040007` = statistics-only mode (from DAT_00012DC4)
- Only re-written when value changes (cached at context+0x1264)

---

## Stock v3 Per-Frame Gather Sequence

From FUN_00012AF0 (input) and FUN_00013044 (output) in libnvisp_v3.c:

### Output gather:
```
NvRmStreamBegin(stream, words, 0, num_output_relocs, 0)
  SET_CLASS(0x32)
  INCR(0xE00, 1):  output_width      — ((W-1) & 0x3FFF) << 16
  INCR(0xE01, 1):  output_height     — ((H-1) & 0x3FFF) << 16
  INCR(0xE02, 1):  output_format     — format word
  INCR(0xE03, 1):  output_gain       — digital gain / crop config
  INCR(0xE04, 3):  [IOVA_Y,  0, stride_Y]    — via NvRmStreamPushReloc
  INCR(0xE07, 3):  [IOVA_U,  0, stride_UV]   — via NvRmStreamPushReloc
  INCR(0xE0A, 3):  [IOVA_V,  0, stride_UV]   — via NvRmStreamPushReloc
  INCR(0x500, 6):  [scaler_flags, h_scale, v_scale, v_ratio, 0, (H<<16)|W]
NvRmStreamEnd()
```

### Input gather:
```
NvRmStreamBegin(stream, num_planes*4 + 0xb, 0, num_planes, 0)
  SET_CLASS(0x32)
  INCR(0xE31, 1):  input_dims        — width & 0x7FFF | (height << 16)
  INCR(0xE33, 1):  input_format      — pixel format descriptor
  for each input plane:
    INCR(0xE34 + plane*3, 3):  [IOVA, 0, stride]  — via NvRmStreamPushReloc
  INCR(0xE32, 1):  strip_config      — strip_width & 0x3FFF | (overlap << 16)
  INCR(0x015, 1):  ISP_ENABLE = 7    — only if changed (cached)
  INCR(0xE30, 1):  input_trigger = 1 — FIRES ISP processing
NvRmStreamEnd()
```

### Syncpt gather (separate submit via NvRmStreamFlush):
```
NvRmStreamBegin(stream, 7, 0, 0, 0)
  SET_CLASS(0x32)
  NONINCR(0x000, 1):  syncpt_incr cond=4 (OP_DONE)    — syncpt_memory
  NONINCR(0x000, 1):  syncpt_incr cond=5               — syncpt_stats
  NONINCR(0x000, 1):  syncpt_incr cond=6 (RD_DONE)     — syncpt_loadv
NvRmStreamEnd()

NvRmStreamBegin(stream, 5, 0, 0, 0)
  SET_CLASS(0x32)
  NONINCR(0x00C, 1):  ISP_CONTROL = trigger_value
NvRmStreamEnd()

NvRmStreamFlush() → SUBMIT ioctl → returns fence

NvRmStreamBegin(stream, 8, 2, 0, 0)
  SET_CLASS(host1x class=1)
  WAIT_SYNCPT(syncpt_memory, threshold)
  WAIT_SYNCPT(syncpt_stats, threshold)
NvRmStreamEnd()

NvRmStreamFlush() → second SUBMIT

NvRmFenceWait(fence, timeout=5000ms) — CPU wait
```

---

## DIFFERENCES: Our Driver vs Stock v3

### Our per-frame submit (current isp_t124.c):
```
SET_CLASS(0x32)
INCR(0xE00, 1): output_width          ✓ correct
INCR(0xE01, 1): output_height         ✓ correct
INCR(0xE02, 1): output_format         ✓ correct
INCR(0xE03, 1): output_color = 0      ✓ correct
INCR(0xE04, 3): output Y plane        ✓ correct
INCR(0xE07, 3): output U plane        ✓ correct
INCR(0xE0A, 3): output V plane        ✓ correct
INCR(0x500, 6): processing block      ✓ correct
SET_CLASS(0x32)
INCR(0x100, 4): [in_IOVA, 0, 0, 0]   ✗ WRONG — 0x100 = stats buffer, not input!
SET_CLASS(0x32) ×2
syncpt incrs cond=4,5,6               ✓ correct
SET_CLASS(0x32)
NONINCR(0x00C, 1): trigger 0x05       ✓ correct
```

### What we're missing:

| # | What | Method | Impact |
|---|------|--------|--------|
| 1 | **Input surface goes to wrong method** | We write to 0x100 (stats). Should be 0xE34 | ISP never receives input data |
| 2 | **No input dimensions** | Missing INCR(0xE31, 1) | ISP doesn't know input size |
| 3 | **No input format** | Missing INCR(0xE33, 1) | ISP doesn't know pixel format |
| 4 | **No input strip config** | Missing INCR(0xE32, 1) | ISP doesn't know tiling/strip layout |
| 5 | **No ISP_ENABLE** | Missing INCR(0x015, 1) = 7 | ISP pipeline may not be active |
| 6 | **No input trigger** | Missing INCR(0xE30, 1) = 1 | ISP never starts processing input |
| 7 | **Input is [IOVA,0,0,0]** | Should be [IOVA, 0, stride] per plane | Wrong surface descriptor |

### What we do correctly:
- Output methods 0xE00-0xE0A ✓
- Processing block 0x500 ✓
- Syncpt increments cond=4,5,6 ✓
- ISP CONTROL trigger 0x00C ✓
- SET_CLASS inside gather ✓
- 4 syncpoints per job ✓

---

## NvRmStream/Channel Implementation (from libnvrm_graphics.so)

### Push Buffer
- Default size: **32KB (0x8000)**, double-buffered (2 × 16KB)
- Heap: IOVMM, alignment 0x20
- Internal state: **0x6E78 bytes** tracking gathers, relocs, wait checks

### Opcode Encoding (confirmed)
```
SET_CLASS:  class_id << 6
INCR:       0x10000000 | (method << 16) | count
NONINCR:    0x20000000 | (method << 16) | count
```

### Relocation Entry (16 bytes)
```c
struct nvhost_reloc {
    uint32_t cmdbuf_mem;     // mem handle of command buffer
    uint32_t cmdbuf_offset;  // byte offset where address goes
    uint32_t target_mem;     // mem handle of target buffer
    uint32_t target_offset;  // byte offset in target buffer
};
// + 4-byte shift entry (bits to right-shift resolved address)
```

### Submit Ioctl
- Primary: `NVHOST_IOCTL_CHANNEL_SUBMIT` (new version)
- Fallback: older submit ioctl
- Passes: gathers (12B each), relocs (16B each), wait checks (16B each), syncpt fences
- Fence output: array of `{syncpt_id, threshold}` pairs

### Channel Open
- Opens `/dev/nvhost-isp` (or `/dev/nvhost-vi`, etc.) based on module ID table
- Sets nvmap fd via `NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD`
- Channel struct: 20 bytes (fd, unused, path, class_id, state)

### Module/Class ID Mapping
| module_id | class | device |
|-----------|-------|--------|
| 0x000B:0 | 3 | ISP-A |
| 0x000B:1 | 0x10003 | ISP-B |
| 0x000C:0 | 2 | ISP alt |
| 0x0004:0 | 0 | VI-A |
| 0x0004:1 | 1 | VI-B |

### Clock/Bandwidth
- `NvRmChannelSetModuleClockRate(ch, module_id, rate_khz)` → NVHOST_IOCTL_CHANNEL_SET_CLK_RATE (Hz)
- `NvRmChannelSetModuleBandwidth(ch, module_id, bw_khz)` → same ioctl with `module_id | 0x01000000`
- `NvRmChannelRegRd/RegWr` → NVHOST_IOCTL_CHANNEL_MODULE_REGRDWR (PIO register access)

---

## v3-Specific Initialization (not in v1)

1. **NvIspCtrlInitialize** — opens `/dev/nvhost-ctrl-isp` or `/dev/nvhost-ctrl-isp.1` for bandwidth control
2. **NvRmHostModuleRegWr(reg=0xFC, value=0x20)** — direct ISP host register write at init (clock gating?)
3. **ISP_ENABLE caching** — context+0x1264 stores last value, only re-writes when changed
4. **NvRmMemHandleAllocAttr** — replaces separate Alloc+Pin with attribute-based allocation

---

## ISP Initialization Sequence (NvIspOpen)

```
1. NvOsAlloc(0x1330)                        — v3 context (4912 bytes)
2. PopulateIspHwFunctions_T12x()            — vtable for T124 (~40 ISP block functions)
3. NvIspCtrlInitialize()                    — open /dev/nvhost-ctrl-isp.N (v3 only)
4. NvRmHostModuleRegWr(0xFC, 0x20)          — direct host register write (v3 only)
5. NvRmChannelOpen(hRm, &ch, 1, &modId)    — opens /dev/nvhost-isp
6. NvRmStreamInit(hRm, ch, &stream)         — allocates 32KB double-buffered push buf
7. NvRmChannelGetModuleSyncPoint() ×4       — 4 syncpoints (indices 0-3)
8. Stats buffer alloc: 8 × 256KB rings      — via NvRmMemHandleAllocAttr
9. NvCameraIspConfigPrimaryPipeline()       — sets processing stage order
10. NvCameraIspMergeOpen()                   — double-buffered settings (2 × 18KB)
```

### ISP Module Enable Defaults (from libnvmm_camera_v3)

| Module | ID | Init State |
|--------|----|-----------|
| Black Level | 4 | **enabled** |
| Lens Shading | 0x4001 | disabled |
| Bad Pixel | 0x4002 | disabled |
| Color Correction | 0x4003 | depends on version |
| CSC | 0x4004 | disabled |
| Edge Enhancement | 6 | disabled |
| Module 0x4008 | 0x4008 | **enabled** |
| Module 0x4009 | 0x4009 | **enabled** |
| Module 0x400a | 0x400a | **enabled** |

---

## Stats System

8-slot ring buffer, 256KB each. v3 uses `NvRmMemHandleAllocAttr`:
```
alignment=0x20, heap=2, size=0x40000 (256KB)
```
Additional 1MB readback buffer at context+0x165C.

Stats config register values:
```
0x800 = stats buffer size config
0x820 = AE/AWB stats method
0x930 = AF stats method
0xC00 = histogram method
0x400 = lens shading method
0x480 = additional method
```

Types: 1,6=zone stats (AWB), 2=histogram, 3,4=color matrices, 5=AF, 0xFF=end marker.

---

## NvOs/NvRm Ioctl Map

| Function | Ioctl | Description |
|----------|-------|-------------|
| NvOsIoctl | wraps ioctl() | packs {nr, p4, p5, p6, p3} into stack struct |
| NvMapMemHandleFree | 0x4e04 | NVMAP_IOC_FREE (magic 'N'=0x4e) |
| NvMapMemPinMult | NVMAP_IOC_PIN | returns IOVA/physical address |
| NvMapMemMap | mmap + ioctl | two-step: mmap nvmap fd, then bind to handle |
| NvRmChannelOpen | SET_NVMAP_FD | sets nvmap fd for channel |
| NvRmChannelSubmit | NVHOST_SUBMIT | gathers+relocs+waitchks+fences |
| NvRmChannelGetModuleSyncPoint | GET_SYNCPOINT | returns syncpt ID by index |
| NvRmFenceWait | SYNCPT_WAITEX | waits on /dev/nvhost-ctrl |
| NvRmChannelSetModuleClockRate | SET_CLK_RATE | sets clock in Hz |
| NvRmChannelSetModuleBandwidth | SET_CLK_RATE | with module_id | 0x01000000 |
| NvRmChannelRegRd/Wr | MODULE_REGRDWR | PIO register access |

---

## Files Analyzed

| File | Functions | Key Content |
|------|-----------|-------------|
| libnvisp_v3.c | 178 | **ISP v3 API** — the one actually used |
| libnvrm_graphics.c | 203 | NvRmStream/Channel implementation |
| libnvmm_camera_v3.c | 3252 | Camera NvMM block, ISP integration |
| camera.tegra.c | 2373 | Camera HAL (no direct ISP access) |
| libnvrm.c | 303 | nvmap memory + RPC transport |
| libnvos.c | 417 | OS abstractions: ioctl/mmap/alloc |
| libnvisp.c | 149 | ISP v1 API (legacy, not used by stock camera) |
| libnvodm_imager.c | 323 | Sensor drivers (IMX179/OV5693) |
| libnvcap.c | 872 | NOT camera! Miracast screen capture |

Decompiled output at: `/tmp/miui_re/decompiled/`
