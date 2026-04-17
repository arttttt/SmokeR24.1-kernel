# ISP-B Streaming Pipeline — OV5693 Front Camera

Captured from stock MIUI Smoke kernel 1.2 via `/proc/isp_trace/`.
Date: 2026-04-12. Device: Xiaomi Mi Pad 1 (mocha), Tegra K1 (T124).

## Pipeline Overview

ISP-B (class 0x34, `/dev/nvhost-isp.1`) processes OV5693 front sensor data
in streaming mode. VI captures raw Bayer frames and feeds them directly to ISP.

### Per-Frame Sequence (3 submits per frame, ~15fps)

```
Submit 1: Calibration update
  G[0]: ~1527-1544 words (cal data: lens shading + tone curves + stats config)
  G[1]: 2 words (immediate syncpt incr for stream)
  Syncpts: 1 (stream only)
  Relocs: 0

Submit 2: Per-frame trigger
  G[0]: 45 words (output config + processing + syncpt incrs + trigger 0x05)
  G[1]: 2 words (immediate syncpt incr for stream)
  Syncpts: 4 (memory, stats, loadv, stream)
  Relocs: 1 (output buffer IOVA — changes each frame for multi-buffering)

Submit 3: Post-frame wait
  G[0]: 8 words (WAIT_SYNCPT for memory + stats + loadv)
  G[1]: 2 words (immediate syncpt incr for stream)
  Syncpts: 1 (stream)
  Relocs: 0
```

### Syncpoint IDs (ISP-B)

| ID | Name | Condition | Purpose |
|----|------|-----------|---------|
| 36 | syncpt_memory | cond=4 (OP_DONE) | Output buffer written |
| 37 | syncpt_stats | cond=5 (STATS_DONE) | Stats buffer written |
| 38 | syncpt_stream | immediate | Submit ordering/sequencing |
| 39 | syncpt_loadv | cond=6 (RD_DONE) | Input read complete |

### Output Configuration (from per-frame gather)

| Register | Value | Description |
|----------|-------|-------------|
| 0xE00 | 0x0A1F0000 | Width = 2592 |
| 0xE01 | 0x07970000 | Height = 1944 |
| 0xE02 | 0x04FE00E6 | Format: YUV420 planar |
| 0xE03 | 0x00000000 | Color config: default |
| 0xE04 | IOVA, 0, 2624 | Y plane: stride=2624 (W aligned 64) |
| 0xE07 | IOVA, 0, 1344 | U plane: stride=1344 (W/2 aligned 64) |
| 0xE0A | IOVA, 0, 1344 | V plane: stride=1344 (W/2 aligned 64) |
| 0x500 | 0,0,0,0,0, 0x07980A20 | Processing: flags=0, dims=1944×2592 |
| 0x015 | (in cal gather) | ISP_ENABLE: set once at init |
| 0x00C | 0x05 | Trigger: streaming runtime |

### Output Buffer Layout (YUV420 planar)

```
Y plane:  2624 × 1944 = 5,101,056 bytes  (offset 0)
U plane:  1344 ×  972 = 1,306,368 bytes  (offset 0x540000 from reloc)
V plane:  1344 ×  972 = 1,306,368 bytes
Total: ~7.7 MB per frame
```

The RELOC in submit 2 patches the output IOVA. The target offset is consistently
0x540000 = 5,505,024 bytes, which is the U plane offset from output base
(Y plane = 2624×1944 = 5,101,056, rounded up to 0x540000 = 5,505,024 for alignment).

### Multi-Buffering

Output IOVA changes each frame (observed IOVAs: 0x8AD00000, 0x8B600000, 0x8BF00000,
0x8DA00000...) — stock camera uses at least 3-4 output buffers for pipelining.

### Calibration Gather Structure

Each frame submits calibration data (~1527 words) containing:

1. **Zero init block** — clear ISP register state
   - 0x202-0x204: input config (zeros)
   - 0x200-0x201: input enable (zeros)
   - 0x205-0x208: stride/format (zeros)
   - 0x700-0x75F: reserved blocks (zeros)
2. **Lens shading** (0xD00-0xD0B)
   - 0xD00: control (10 words) — zeros on this device
   - 0xD0A: enable — zero
   - 0xD0B: LUT (480 words) — zeros (no .isp profile)
3. **Stats config** (0x900-0x91F)
   - 0x91F = 0x00000002
4. **Tone curves** (0x651-0x658)
   - 4 channels × 257 entries
   - All 0x1000 = identity (1.0 in Q12 fixed point)
5. **ISP enable** (0x053)
6. **Post-apply trigger** (0x00C = 0x0F)
7. **Tail registers** (0x018-0x05F)
   - 0x018: 5 words — timing/sync config
   - 0x01F: 0x00000001
   - 0x05F: 0x00000010

### Post-Frame Submit (8 words)

```
SETCLASS(0x34)
WAIT_SYNCPT(syncpt_memory, threshold)    # Wait for output done
WAIT_SYNCPT(syncpt_stats, threshold)     # Wait for stats done
WAIT_SYNCPT(syncpt_loadv, threshold)     # Wait for input read done
```

This submit blocks until the current frame is fully processed before
allowing the next frame's calibration submit.

### Key Observations

1. **Every frame gets fresh calibration** — cal data is resubmitted each frame,
   not just once at init. This allows per-frame 3A updates.
2. **No input registers in per-frame** — ISP receives data from VI automatically
   in streaming mode (trigger 0x05).
3. **Output format 0x04FE00E6** is YUV420 planar with separate Y/U/V planes,
   NOT the 0x010000C9 seen in blob init or 0x43 (RGBA) used for reprocess.
4. **Processing flags = 0** — no special flags needed for normal operation.
5. **Calibration is all zeros** — no .isp profile installed on this MIUI ROM,
   ISP runs with default identity settings (linear tone curves, no lens shading).
