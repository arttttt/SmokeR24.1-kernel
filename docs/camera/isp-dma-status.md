# ISP DMA Debug — Complete Status

## Updated: 2026-04-03 (session 2)

## Current State

**ISP output DMA does NOT work via userspace submit on EITHER stock or 24.1.**
Previous "ISP wrote data" results were stale nvmap data — after zeroing output, all results are 0.
ISP **reads** input (stats contain real bayer values) but **does not write** output.

This is NOT an nvhost/kernel difference issue. The problem is identical on both kernels.

## What Works (both kernels)

- ISP ping (IMM_INCR_SYNCPT) — OK
- ISP input DMA read — stats region contains real sensor data
- 6-gather submit structure matches stock exactly
- Calibration block loads and is accepted by ISP
- Correct class IDs: ISP-A=0x32, ISP-B=0x34
- Correct per-syncpt conditional incrs (3 different syncpts)
- SMMU mapping, buffer pinning, relocation — all verified working

## What Does NOT Work

- ISP output DMA write — output always zeros after zeroing
- Conditional syncpt cond4 (OP_DONE) never fires — ISP does not complete processing
- Even with ISP pre-initialized by stock camera HAL, our submit does not produce output

## ISP Syncpt Architecture (discovered this session)

Each ISP has **4 syncpoints** (via GET_SYNCPOINT param 0,1,2,3):

| Param | Name | ISP-A | ISP-B |
|-------|------|-------|-------|
| 0 | memory | 32 | 36 |
| 1 | stats | 33 | 37 |
| 2 | stream | 34 | 38 |
| 3 | loadv | 35 | 39 |

Stock conditional syncpt incrs in G2 use **3 different syncpts**:
- cond 4 → memory syncpt (param 0)
- cond 5 → stats syncpt (param 1)
- cond 6 → loadv syncpt (param 3)

**Previous bug**: We used memory syncpt for all three → timeout.

## Stock Per-Frame Submit Structure (6 gathers)

| Gather | Words | Content |
|--------|-------|---------|
| G1 | 2 | Syncpt incr (immediate) |
| G2 | 45 | SET_CLASS + output dims/format/surfaces + processing + input + 3x conditional syncpt incrs + trigger 0x05 |
| G3 | 2 | Syncpt incr (immediate) |
| G4 | 8 | SET_CLASS(0x01) + WAIT_SYNCPT (waits on stats syncpt of previous frame) |
| G5 | 2 | Syncpt incr (immediate) |
| G6 | ~1538 | Calibration block + INCR(0x053, 2) [enable + input addr] |

### G2 Details (45 words, ISP-B example)
```
SET_CLASS(0x34)
INCR(0xE00,1) → ((W-1) << 16)          output width
INCR(0xE01,1) → ((H-1) << 16)          output height
INCR(0xE02,1) → 0x04FE00E6             format
INCR(0xE03,1) → 0                      color params
INCR(0xE04,3) → [Y_addr, 0, Y_stride]  Y surface
INCR(0xE07,3) → [U_addr, 0, UV_stride] U surface
INCR(0xE0A,3) → [V_addr, 0, UV_stride] V surface
INCR(0x500,6) → [0,0,0,0,0, (H<<16)|W] processing
SET_CLASS(0x34)
INCR(0x100,4) → [in_addr, 0, 0, 0]     input buffer
SET_CLASS(0x34) x2
NONINCR(0x000,1) → (4<<8)|memory_syncpt  cond4: OP_DONE
NONINCR(0x000,1) → (5<<8)|stats_syncpt   cond5: stats done
NONINCR(0x000,1) → (6<<8)|loadv_syncpt   cond6: loadv done
SET_CLASS(0x34)
NONINCR(0x00C,1) → 0x05                  trigger
```

### G4 Details (8 words) — WAIT_SYNCPT
```
SET_CLASS(0x01)                           host1x class
NONINCR(0x008,1) → (stats_id<<24)|thresh  WAIT_SYNCPT for stats
SET_CLASS(0x34)                           back to ISP class
SET_CLASS(0x01)
NONINCR(0x008,1) → (loadv_id<<24)|thresh  WAIT_SYNCPT for loadv
SET_CLASS(0x34)
```

### G6 Tail — INCR(0x053, 2)
```
INCR(0x053,2) → [0x01, input_addr]       ISP enable + input buffer
```

## Stride Calculation

```
Y_STRIDE  = (W + 63) & ~63
UV_STRIDE = ((W/2) + 63) & ~63    (NOT Y_STRIDE/2)
```

| Sensor | W | H | Y_STRIDE | UV_STRIDE |
|--------|---|---|----------|-----------|
| IMX179 | 3280 | 2460 | 3328 (0xD00) | 1664 (0x680) |
| OV5693 | 2592 | 1944 | 2624 (0xA40) | 1344 (0x540) |

## Investigated and Ruled Out

| What | Result |
|------|--------|
| nvhost kernel differences | ISP DMA fails identically on stock |
| Gather filter | Disabled for ISP |
| SMMU fixup | Working |
| Pushbuffer format | Identical to stock |
| Wrong class ID for ISP-B | Fixed: 0x34 |
| Wrong UV stride | Fixed: align W/2 to 64 |
| Wrong syncpt IDs | Fixed: 3 different syncpts |
| Single gather vs 6-gather | Both tested, same result |
| Missing INCR(0x053) | Added, no change |
| Missing E30-E33 regs | Stock doesn't use them either |
| Camera HAL pre-init of ISP | ISP stays primed, still no output |
| All trigger values (0x05, 0x09, 0x0F) | No output |
| Multiple format codes (0x20, 0x22, 0xCA, 0xE6) | No output (0x20 never works) |

## Working Hypothesis

ISP conditional syncpts (cond4 OP_DONE) never fire without real VI input. ISP reads memory
buffers for stats (lightweight operation) but does NOT start full output DMA pipeline without
a proper frame event — likely tied to VI syncpt or internal frame counter.

Evidence:
- Stats contain real bayer data from nvmap input buffer → ISP DMA read works
- cond4 (OP_DONE) never fires → ISP never completes processing → output stays zero
- Same behavior on stock AND 24.1 → not a kernel/nvhost issue
- Even with ISP pre-initialized by stock camera HAL → still no output from our submit
- Stock ISP WAIT_SYNCPT waits on its own stats/loadv syncpts (inter-frame sync), not VI

## Blob Injection Experiment

Attempted to call NvIsp API from within stock mediaserver process:
- LD_PRELOAD injection into mediaserver works (delayed thread after NvRm init)
- `NvIspOpen()` returns err=4 (cannot create second ISP instance)
- `NvCameraIspGetNvIspHandle()` returns opaque NvRm handle (0xe9000002)
- Direct NvIsp* calls with this handle crash — it's an opaque ID, not a pointer
- **Conclusion**: blob wrapper approach requires full RE of NvIsp struct layouts (weeks of Ghidra work), not practical

## Next Step: Kernel Integration

**Integrate ISP into real VI→ISP→output pipeline on 24.1 kernel.** This is the correct path because:
1. VI already works, streams from OV5693
2. ISP-B registered as V4L2 subdev in MC graph
3. Full stock cmdbuf sequence decoded
4. ISP syncpt architecture known (memory/stats/stream/loadv)
5. VI will provide the frame event that ISP needs to start output DMA
6. All userspace test findings feed directly into kernel driver

Implementation plan:
- Wire ISP-B subdev into channel.c capture path
- After VI captures frame into raw buffer, submit ISP cmdbuf with that buffer as input
- Use VI frame_done syncpt as ISP's "input ready" signal
- Output to separate YUV buffer → userspace gets processed frame

## Files

| File | Purpose |
|------|---------|
| `drivers/media/platform/tegra/camera/isp_t124.c` | ISP MC driver + kernel dma_test |
| `drivers/media/platform/tegra/camera/isp_t124.h` | ISP defines |
| `drivers/media/platform/tegra/camera/isp_t124_cal.h` | Stock calibration data |
| `tools/camera/isp_test_24.c` | Userspace ISP DMA test (correct syncpts, 6-gather, ISP-B class) |
| `tools/camera/isp_blob_test.c` | Standalone NvIsp dlopen test (NvIspOpen hangs without NvRm) |
| `tools/camera/isp_payload.c` | LD_PRELOAD payload for mediaserver injection |
| `tools/camera/isp_injector.c` | ptrace-based .so injector |
| `docs/camera/isp-dma-debug-24.1.md` | Full debug data dump |
| `docs/camera/camera-isp-reverse-engineering.md` | Main ISP RE document |
| `docs/camera/stock-isp-a-cmdbuf-dump.txt` | Stock ISP-A cmdbuf (52K lines) |
| `docs/camera/stock-isp-b-cmdbuf-dump.txt` | Stock ISP-B cmdbuf (50K lines) |
