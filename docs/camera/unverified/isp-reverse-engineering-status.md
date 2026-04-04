# ISP Reverse Engineering — Status & Next Steps

## What We Have Now

### Live Register Dump (from stock firmware, camera streaming)

We captured ISP-A and ISP-B MMIO registers via `devmem` while stock camera was actively streaming (IMX179 rear 3280x2460, OV5693 front).

**Key confirmed values:**
```
Method  MMIO    ISP-A (rear)    ISP-B (front)   Notes
0x008   0x20    0xF000F800      0xF000F800      Same — hw default, input config?
0x00C   0x30    0x00000004      0x00000004      Control reg DURING streaming (not 0x0F!)
0x00D   0x34    0x00000100      0x00000100      Same
0x014   0x50    0x000000EB      0x0000019B      Different — per-sensor
0x015   0x54    0x04040007      0x04040007      Enable mode — confirmed from RE
0x018   0x60    0x0A00500A      0x0A00500A      Same — processing params
0x019   0x64    0x00008089      0x00008089      Same
0x01A   0x68    0x013645CB      0x013645CB      Same — calibration?
0x01B   0x6C    0x000001E7      0x000001E7      Same
0x01C   0x70    0x00000001      0x00000001      Same
0x01D   0x74    0x00000001      0x00000001      ISP_CG_CTRL
0x01F   0x7C    0x00000003      0x00000003      Same
0x024   0x90    0xC6BFF67C      0xC6BFF67C      Same
0x025   0x94    0xDBAA349D      0x2EA3307A      Different — per-sensor (probably syncpt/hash)
0x026   0x98    0x56701661      0x731F6FFF      Different — per-sensor
0x028   0xA0    0x00000007      0x00000007      Same
0x029   0xA4    0x00000007      0x00000007      Same
0x02A   0xA8    0x00000008      0x00000007      Slightly different
0x038   0xE0    0x242CB07B      0x242CB07B      Same
0x03B   0xEC    0x017BAD37      0x00100900      Different — per-sensor
0x03F   0xFC    0x00000020      0x00000020      Same
0x051   0x144   0x017BA537      0x017BA537      Same
0x053   0x14C   0x00000001      0x00000001      ISP_ENABLE
0x054   0x150   0x00585B18      0x02016B4C      Different — per-sensor (buffer addr?)
0x05E   0x178   0x00003232      0x00003232      Same
```

**ISP-B was all 0xFFFFFFFF when front camera was off** (power gated).

### What MMIO Dump Does NOT Show
- Methods 0xE00+ (output surface/format) — these are write-only or FIFO registers, readback returns empty/bus-error
- Methods 0x200+ (coefficients) — same, write-only via host1x
- Methods 0x500+ (processing) — write-only
- Methods 0x100+ (tone curve LUT) — FIFO, write-only

### Mapping: Method Offset → MMIO Byte Offset
```
MMIO_byte = method_offset * 4
Example: method 0x015 → MMIO 0x054
```

## What We've Tried in Our Kernel (all fail with "output untouched")

Our kernel ISP driver successfully:
- Registers ISP-A as V4L2 subdev in MC graph
- Maps host1x channel, allocates syncpoint
- Submits command buffers — all 24 known methods accepted
- OP_DONE syncpoint fires for simple writes

But ISP DMA never happens. We tried:
1. Various 0xE00-0xE33 output register encodings
2. 0xE04+3*i output surface descriptors
3. 0xE34+3*i input surface descriptors
4. 0x00C trigger values: 0x01, 0x05, 0x0F
5. 0x015 enable values: 0x01, 0x07, 0x04040007
6. Two-submit approach (buffer addr first, then config)
7. All three runtime blocks (0x60D8 + 0x31C0 + 0x76F8 pattern)

## What You Should Focus On

### Priority 1: ProcessFrame3 Runtime Args
The stock firmware is running on the device with camera streaming. We can dump memory.
Key question: **what exact values does NvIspProcessFrame3 receive?**

From your RE:
- arg9 = array of 3 output descriptor pointers
- arg2..arg13 packed at sp+0x4c
- pack+0x00 = mode (1 or 2)
- pack+0x04..+0x10 = ROI/crop
- pack+0x14 = pointer to normalized 0xB0 surface descriptor

### Priority 2: 0xE04 Surface Entry Layout
What are the 3 words in `INCR(0xE04+3*i, 3)`?
- Word 1: reloc address (confirmed)
- Word 2: `entry[0x14] & 0x3F` — what is this? Format? Tiling?
- Word 3: full `entry[0x14]` — stride? Packed size?

### Priority 3: Control Register 0x00C Semantics
Stock shows 0x00C = 0x04 during active streaming. Your RE found:
- 0x0F = post-apply callback
- 0x05/0x07/0x09 = runtime modes
- 0x04 = what the hardware actually holds during streaming

What's the real sequence? Does 0x04 mean "streaming active" state after trigger?

## Available Tools on Stock Device
- `devmem` at `/system/xbin/devmem` — read/write physical memory
- ISP-A at 0x54600000, ISP-B at 0x54680000
- `/dev/nvhost-isp` and `/dev/nvhost-isp.1` — host1x ISP channels
- Camera is streaming (rear IMX179 or front OV5693)
- Root adb access

## Binary Locations
- Stock libnvisp_v3.so: `/tmp/libnvisp_v3_shield.so` (63KB, Shield T124 variant)
- Full disasm: `/tmp/libnvisp_v3_disasm2.txt` (19K lines, llvm-objdump Thumb-2)
- JXD vendor source: `/Users/artem/Projects/vendor_nvidia_jxd_src/`
- Proprietary vendor: `/Users/artem/Projects/proprietary_vendor_nvidia/` (git history)

## Current Offline RE State

Use this section as the current working handoff. It reflects the latest confirmed caller-side ABI and runtime-path findings from `libnvisp_v3.so` and `libnvmm_camera_v3.so`.

### Confirmed Call Sites
- `libnvmm_camera_v3.so` call at `0x2efd2` is the real public ISP runtime submit via `NvIspProcessFrame`
- `libnvmm_camera_v3.so` call at `0x2f0f4` is `NvViCsiCaptureFrame`, not ISP
- PLT mapping is confirmed:
  - `0x23abc -> NvIspProcessFrame`
  - `0x23ac8 -> NvViCsiCaptureFrame`

### Confirmed T12x Runtime Callbacks in `libnvisp_v3.so`
- `ctx + 0xd8 = 0x76f8`
- `ctx + 0xdc = 0x60d8`
- `ctx + 0xe0 = 0x31c0`
- `ctx + 0xe4 = 0x4210`

### `NvIspProcessFrame` Wrapper Model
- Public `NvIspProcessFrame` is a wrapper over `NvIspProcessFrame3`
- It repacks public args and builds two local arrays:
  - `arg9_array = { public_arg9, 0, 0 }`
  - `arg11_array = { public_arg11, 0, 0 }`
- Therefore normal public callers only populate slot `0` of the 3-entry arrays seen by `NvIspProcessFrame3`

### Caller-Side Header Before `NvIspProcessFrame`
`libnvmm_camera_v3.so` prepares a local header in `sp+0x94..0xb0` before calling `NvIspProcessFrame`.

#### Mode 1 path
- `sp+0x94 = 1`
- builds a local `0xb0` block at `sp+0x160`
- stores pointer to that block at `sp+0xa8`

#### Mode 2 path
- `sp+0x94 = 2`
- stores width/height from `ctx+0x278/0x27c` into `sp+0xa8/sp+0xac`

This means the public call effectively passes:
- `arg2 = sp+0x94`
- `arg3 = sp+0x98`
- `arg4 = sp+0x9c`
- `arg5 = sp+0xa0`
- `arg6 = sp+0xa4`
- `arg7 = sp+0xa8`
- `arg8 = sp+0xac`

### Public Args 9..13 at the `0x2efd2` Call Site
Immediately before the `NvIspProcessFrame` call:
- `public arg9  = sp+0x210 + 0xb0*slot`
- `public arg10 = sp+0x78 + 8*slot`
- `public arg11 = [sp+0x3b8] + count*8`
- `public arg12 = sp+0x5c`
- `public arg13 = [sp+0x3bc]` or `0`

This strongly suggests:
- `arg9` is a pointer to a local `NvMMSurfaceDescriptor`-like block
- `arg11/arg12/arg13` are runtime control/sync objects, not surface descriptors

### Runtime Block Roles

#### `0x60d8`
This currently looks like the primary runtime output block:
- writes `0xe31`, `0xe33`, `0xe32`
- may write `0x015 = 7`
- writes `0xe30 = 1`
- then emits the surface loop at `0xe34 + 3*i`

#### `0x31c0`
This currently looks like the secondary/additional output block:
- writes `0x500`
- writes `0xe00`, `0xe01`, `0xe02`, `0xe03`
- then emits the surface loop at `0xe04 + 3*i`

#### `0x76f8`
This is the runtime control/sync block:
- consumes the packed runtime header
- selects control behavior from mode-like values
- emits control writes to `0x00c`
- uses the `arg11/arg12/arg13` family for runtime state/sync bookkeeping

### Important Confirmed Register Behavior
- Static post-apply callback `0x4210` writes `0x00c = 0x0f`
- Stock live MMIO during actual streaming shows:
  - `0x015 = 0x04040007`
  - `0x00c = 0x00000004`
- Therefore `0x0f` is not the final steady-state streaming value of `0x00c`

### Still-Unresolved Contradiction
This is the main open issue in the model.

- In public mode 1, `pack+0x14` appears to point at a local `0xb0` block prepared by `libnvmm_camera_v3.so`
- But `0x60d8` reads from the object at `pack+0x14` up to at least `+0xf0/+0xf4/+0x100`
- This means one of the following must be true:
  1. mode 1 is not the actual steady-state preview path we care about
  2. the visible `0xb0` block is only the front of a larger composite object
  3. the caller-side model still misses one layer of indirection

Do not treat this as solved yet.

## Practical Working Model

### Static Config Path
- `SetOutputT12x`-like setup
- `HwSettingsApply`
- post-apply callback `0x4210`
- control write `0x00c = 0x0f`

### Runtime Frame Path
- public `NvIspProcessFrame`
- wrapper into `NvIspProcessFrame3`
- runtime output/config blocks:
  - `0x60d8`
  - `0x31c0`
  - `0x76f8`

Current conclusion:
- static config alone is not enough to trigger DMA
- real frame processing depends on runtime descriptor ABI, not just method existence

## Next Live Dump Targets

When the stock device is available for a focused dump, capture the following around the `NvIspProcessFrame` call site at `0x2efd2`:

1. `r0-r3`
2. stack args `5..13`
3. memory at:
   - `sp+0x94`
   - `sp+0xa0`
   - `sp+0x160`
   - `sp+0x210`
   - `sp+0x3b8`
   - `sp+0x3bc`
   - `sp+0x3c0`
4. pointees of public `arg11` and `arg12`

Most useful immediate goal:
- resolve what object really sits behind `pack+0x14`
- confirm how the runtime control object at `sp+0x3b8` is initialized and evolves
