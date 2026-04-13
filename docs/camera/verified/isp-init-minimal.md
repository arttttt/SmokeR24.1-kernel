# ISP Minimal Init — Registers Required for Pixel Output

Discovered via binary search on blob's HwSettingsApply cal gather.
Date: 2026-04-13. Device: Xiaomi Mi Pad 1 (mocha), Tegra K1 (T124).

## Summary

ISP reprocess mode requires a **6-word init gather** before the first
per-frame gather. Without it, ISP accepts gathers and completes (syncpt
increments) but writes zero pixels to output buffer.

## Minimal Init Gather (6 words)

```
SETCLASS(0x32)                 # ISP-A class
INCR(0x019, 1): 0x00000400    # ISP register 0x019 = 0x400
INCR(0x01B, 2): 0x00000200    # ISP register 0x01B = 0x200
                  0x00000002   # ISP register 0x01C = 0x002
```

All three non-zero register values are required. Any single one alone
produces zero output.

## Register Analysis

### 0x019 = 0x400

Part of the ISP pipeline control block (0x018-0x01F).
Value 0x400 = 1024. Likely a DMA burst length, FIFO threshold, or
internal buffer size configuration.

In stock camera init (from isp_t124.c kernel driver), this register
block is written as:
```
INCR(0x018, 5): 0x00000000, 0x00000400, 0x00000000, 0x00000200, 0x00000002
```
Register 0x018 = 0 (unused), 0x019 = 0x400, 0x01A = 0 (unused).

### 0x01B = 0x200

Value 0x200 = 512. Likely paired with 0x019 — could be input vs output
pipeline thresholds, or read vs write DMA configuration.

### 0x01C = 0x002

Value 2. Likely a mode/enable flag. Could be:
- Output plane count (2 = UV interleaved?)
- Pipeline stage enable bits
- DMA channel count

### Possible interpretation

These three registers together configure the ISP's internal DMA engine:
- 0x019: write (output) burst/threshold = 0x400
- 0x01B: read (input) burst/threshold = 0x200
- 0x01C: enable/mode = 2

Without them, ISP processes the gather opcodes but the internal DMA
that moves pixels from ISP core to output memory is not configured.

## Binary Search Results

| Cal gather content | Words | Result |
|---|---|---|
| Full blob HwSettingsApply | 3648 | Works |
| First half (0-1823) | 1824 | Works |
| First quarter (0-911) | 912 | Zero output |
| Second quarter (912-1823) | 912 | Works |
| Words 1368-1823 | 456 | Works |
| Tail only (1801-1823) | 23 | Works |
| 0x300+0x304+0x053+trigger (no 0x018) | 16 | Zero output |
| Trigger 0x0F only | 3 | Zero output |
| Trigger 0x0F + 0x018 block | 10 | Works |
| 0x018 block only (no trigger) | 7 | Works |
| 0x019+0x01B+0x01C (non-zero only) | 6 | Works |
| 0x019 only | 3 | Zero output |
| 0x01B+0x01C only | 4 | Zero output |

## Key Findings

1. **Trigger 0x0F (post-apply) is NOT required** for reprocess output
2. **ISP_ENABLE (0x015) is NOT required** in cal gather (already set by NvIspOpen)
3. **Work buffer (0x053) is NOT required** for basic reprocess
4. **Calibration data (lens shading, tone curves) NOT required** for pixel output
5. **Only 0x019/0x01B/0x01C are required** — ISP DMA pipeline config
6. The kernel driver (isp_t124.c) already writes these values in stream_init
   but may not submit them correctly to ISP hardware

## Additional Findings (2026-04-13)

### Demosaic / Color Output
ISP reprocess mode produces **grayscale only** regardless of:
- Format (0x43, 0x41, 0x2a, 0xC8, 0xC9 — all single channel)
- Calibration (full blob cal gather, lens shading, tone curves — no effect)
- ISP instance (ISP-A and ISP-B — same result)
- Blob vs pure (NvIspOpen+HwSettingsApply gives same grayscale)

Stock streaming mode (trigger 0x05) produces color YUV420.
Reprocess (trigger 0x0B) does not activate demosaic.

Root cause: ISP HW demosaic unit needs specific register config from
NvIspOpen→HwSettingsCreate shadow register defaults (~8KB). These
defaults haven't been RE'd yet.

### Kernel Driver Status
- stream_init already writes 0x019/0x01B/0x01C (in 0x018 INCR block)
- Input format fixed: 0x10200024 (was 0x11000020)
- nvmap heap fixed: IOVMM (was carveout — wrong SMMU mapping)
- Output buffer size fixed: no 2x safety margin, correct per-mode sizing
- Reprocess per-frame: minimal gather, no cal submit needed

### Pure Userspace Test
isp_reprocess_pure.c works completely without blobs:
1. open("/dev/nvhost-isp")
2. PIO write 0xFC=0x20
3. Init gather: 0x019=0x400, 0x01B=0x200, 0x01C=2
4. Per-frame gather: format 0x43 + surfaces + trigger 0x09+0x0B

CHANNEL_OPEN ioctl (NR=112) causes kernel panic on 24.1 — not needed.

## Implications

For the kernel ISP driver: registers 0x019/0x01B/0x01C are already
written in stream_init (0x018 INCR block). Driver has been cleaned
up with verified values. Main remaining issue: demosaic for color.

For the pure userspace test: works end-to-end without blobs.
Color output requires RE of HwSettingsCreate register defaults.
