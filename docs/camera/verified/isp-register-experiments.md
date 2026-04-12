# ISP Register Override Experiments

Tested on stock MIUI Smoke kernel 1.2 via `/proc/isp_patch`.
Date: 2026-04-12. Device: Xiaomi Mi Pad 1 (mocha), Tegra K1 (T124).
Camera: OV5693 front, ISP-B (class 0x34), streaming mode.

## Method

Each register patched via `echo '0xNNN=value' > /proc/isp_patch`.
Patches are applied to ISP gathers in-flight by the kernel tracing module.
Camera restarted between some tests, live-patched for others.

## Results

### 0x500 — Processing Flags

Stock value: 0x00000000 (first word of 6-word INCR block).

| Value | Result |
|-------|--------|
| 0 | OK (stock) |
| 1 | No image output |
| 2 | No image output |
| 3 | ISP/VI errors in dmesg, requires reboot |

**Conclusion**: Only 0 works. Non-zero flags break the output pipeline entirely.
The processing block's first word must be 0 for normal operation.
Note: MIUI beauty/color modes also keep flags=0.

### 0xE02 — Output Pixel Format

Stock value: 0x04FE00E6 (YUV420 planar).

| Value | Format | Result |
|-------|--------|--------|
| 0x04FE00E6 | YUV420 planar | OK (stock, confirmed) |
| 0x43 | R8G8B8A8 | Image corrupted (HAL expects YUV420) |
| 0x010000C9 | Unknown (blob init) | Image corrupted |
| 0xC8 | YUYV | Image corrupted |

**Conclusion**: Format must match what downstream expects. In streaming mode,
0x04FE00E6 is the only working format because the HAL/DZ pipeline parses
output as YUV420 planar with specific stride layout.

Format 0x43 (R8G8B8A8) works in **reprocess mode** (userspace test) where
we control the output buffer interpretation directly.

### 0xE03 — Output Color Config

Stock value: 0x00000000.

| Value | Result |
|-------|--------|
| 0 - 0xFFFFFFFF (full range) | No visible effect |

**Conclusion**: This register has no visible effect on output image.
Either unused, affects only metadata/stats, or is only read at init time.

### 0xE06 — Y Plane Stride

Stock value: 0xA40 (2624 = W aligned to 64).

| Value | Decimal | Result |
|-------|---------|--------|
| 0xA40 | 2624 | OK (stock) |
| 0x520 | 1312 (half) | Image corrupted |
| 0xA00 | 2560 (W exact) | Image corrupted |
| 0x1000 | 4096 | Image corrupted |
| 0x1480 | 5248 (double) | Image corrupted |

**Conclusion**: Stride must exactly match the allocated buffer stride.
Any mismatch corrupts the image because ISP DMA writes with the
register stride but the display reads with the buffer's actual stride.
This confirms 0xE06 is indeed the Y plane stride register.

### 0x015 — ISP_ENABLE

Stock value: set during init, not in per-frame gather.

| Value | Result |
|-------|--------|
| 0x00 | No effect |
| 0x01 | No effect |
| 0x03 | No effect |
| 0x07 | No effect |

**Conclusion**: 0x015 is NOT written in per-frame gathers. It is set once
during ISP initialization (possibly via PIO write or init gather that
runs before tracing starts). The patcher cannot intercept it because
it never appears in the gathers being submitted.

The stock init value is likely 0x04040007 (streaming + stats mode).

### 0xD0A — Lens Shading Enable

Stock value: 0x00000000. Written in cal gather (not per-frame).

| Value | Result |
|-------|--------|
| 1 | No effect (patcher doesn't reach cal gather) |

**Conclusion**: Not patchable — written in calibration gather which is a
separate submit from the per-frame gather. Patcher only modifies per-frame.

### 0x651 — Tone Curve Ch0 Control

Stock value: 0x00000000. Written in cal gather.

| Value | Result |
|-------|--------|
| 1 | No effect (same reason — cal gather) |

**Conclusion**: Not patchable via per-frame patch.

### 0x053 — Work Buffer Enable + IOVA

Stock value: 0x00000001 + IOVA. Written in cal gather.

| Value | Result |
|-------|--------|
| 0 | No effect (cal gather, not per-frame) |

**Conclusion**: Not patchable via per-frame patch.

### 0x100 — Stats Buffer IOVA

Stock value: stats buffer IOVA. Written in per-frame gather.

| Value | Result |
|-------|--------|
| 0 | No visible effect |

**Conclusion**: Stats buffer address doesn't affect pixel output.
ISP writes stats to this address but output image is independent.

### 0x00C — ISP Control Trigger

Stock value: 0x05 (streaming). Written in per-frame gather via NONINCR.

| Value | Result |
|-------|--------|
| 0x0B (reprocess) | No visible effect |

**Conclusion**: Changing trigger from streaming (0x05) to reprocess (0x0B)
on a live streaming pipeline has no visible effect. ISP may continue
processing VI data on inertia, or 0x0B without input surface config
simply does nothing. Trigger change likely only matters at pipeline start.

## Patcher Scope

The isp_patch module patches gathers as they are submitted. However,
not all registers are in every gather:

**Per-frame gather (45w)** — patchable:
- 0xE00-0xE0A (output config + surfaces)
- 0x500 (processing)
- 0x100 (stats buffer)
- 0x00C (trigger)
- syncpt incrs

**Cal gather (~1527w)** — also patchable (same patcher runs on all ISP gathers):
- 0xD00-0xD0B (lens shading)
- 0x651-0x658 (tone curves)
- 0x053 (work buffer)
- 0x00C = 0x0F (post-apply trigger)

However, some cal registers (0xD0A, 0x651, 0x053) showed no effect when
patched. This may be because:
1. The values are the same as stock defaults (0→1 for enable might not change behavior with zero data)
2. The ISP latches these at init and per-frame writes are ignored
3. These need companion data (e.g. 0xD0A=1 needs non-zero 0xD0B data)

## DANGER: Registers That Break ISP (require reboot)

| Register | Description | What breaks it |
|----------|-------------|----------------|
| **0x500[0]** | Processing flags | Any non-zero value (1, 2, 3) |
| **0xE00** | Output width | Any value ≠ stock (even before camera open) |
| **0xE01** | Output height | Any value ≠ stock (even before camera open) |
| **0xE02** | Output format | Any value ≠ 0x04FE00E6 (corrupts, may need reboot) |

These cause ISP panic / fence timeouts and require device reboot to recover.

## Safe Registers (no effect or image corruption only)

| Register | Description | Effect |
|----------|-------------|--------|
| 0xE03 | Output color config | No effect (unused) |
| 0xE05 | Y surface word 2 | No effect (unused) |
| 0xE06 | Y stride | Image corruption (recoverable, no panic) |
| 0x015 | ISP_ENABLE | No effect (not in per-frame gather) |
| 0x100 | Stats buffer | No effect on image |
| 0x00C | Trigger | No visible effect on live stream |
| 0xD0A | Lens shading enable | No effect (cal gather) |
| 0x651 | Tone curve ctrl | No effect (cal gather) |
| 0x053 | Work buffer | No effect (cal gather) |

## Key Takeaways

1. **ISP registers fall into two categories**: per-frame (output config,
   trigger) and init-only (ISP_ENABLE 0x015, possibly others).

2. **Format and stride are tightly coupled to buffer allocation** — changing
   them without matching buffer layout always corrupts output.

3. **Processing flags must be 0** — any non-zero value breaks output.

4. **0xE03 is unused** — safe to set to 0 and ignore.

5. **Real color control is through calibration** — lens shading (0xD0B)
   and tone curves (0x652-0x658), not through output config registers.

6. **Trigger changes on live pipeline have no effect** — pipeline mode
   (streaming vs reprocess) is set at start, not switchable mid-stream.

7. **Stats buffer (0x100) is independent of pixel output** — can be
   zeroed without affecting the image.

## Additional Experiments (Round 2)

### 0xE00 / 0xE01 — Output Width / Height

| Register | Value | When | Result |
|----------|-------|------|--------|
| 0xE00 | 0x050F0000 (W=1296) | Live | ISP panic |
| 0xE00 | 0x050F0000 (W=1296) | Before camera open | ISP panic |
| 0xE01 | 0x03CB0000 (H=972) | Before camera open | ISP panic |

**Conclusion**: Output dimensions cannot be changed — must match VI/buffer
allocation. ISP panics on any dimension mismatch. Downscaling is likely
handled by a separate scaler block (see NvIspHwSettingsCopyOutputDownScaler
in blob), not through 0xE00/0xE01.

### 0xE05 — Y Surface Word 2 (normally 0)

| Value | Result |
|-------|--------|
| 0 - 0xFFFFFFFF (full range) | No visible effect |

**Conclusion**: Unused padding word in surface descriptor. Safe to leave as 0.

### 0x650 — Tone Curve Master Enable (?)

Not tested to conclusion — register purpose unclear, likely in cal gather.
