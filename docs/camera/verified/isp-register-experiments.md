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

## Key Takeaways

1. **ISP registers are not all hot-patchable** — some (0x015) are set once
   at init and cannot be changed per-frame.

2. **Format and stride are tightly coupled to buffer allocation** — changing
   them without matching buffer layout always corrupts output.

3. **Processing flags must be 0** — any non-zero value breaks output.
   This applies to both streaming and reprocess modes.

4. **0xE03 is unused** — safe to set to 0 and ignore.

5. **Real color control is through calibration** — lens shading (0xD0B)
   and tone curves (0x652-0x658), not through output config registers.
