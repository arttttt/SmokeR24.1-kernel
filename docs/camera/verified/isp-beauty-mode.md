# ISP Beauty Mode — Noise Reduction / Skin Smoothing

Captured from stock MIUI Smoke kernel 1.2 via `/proc/isp_trace/gather`.
Date: 2026-04-12. Device: Xiaomi Mi Pad 1 (mocha), Tegra K1 (T124).

## Overview

MIUI beauty mode activates register block 0x700-0x70F in addition to
lens shading and tone curves. This block is likely a **noise reduction /
spatial filter** — smooths skin while preserving edges.

Normal mode: 0x700 block all zeros (disabled).
Beauty mode: 0x700[0] = 1 (enabled), with filter parameters.

## Register Block 0x700-0x70F (16 words)

| Register | Value | Purpose (inferred) |
|----------|-------|--------------------|
| 0x700 | 0x00000001 | Enable: 1=on, 0=off |
| 0x701 | 0x00000000 | Reserved |
| 0x702 | 0x00000000 | Reserved |
| 0x703 | 0x00000000 | Reserved |
| 0x704 | 0x00000000 | Reserved |
| 0x705 | 0x00004490 | Threshold / sensitivity (varies: 0x4490-0x44A0) |
| 0x706 | 0x00000000 | Reserved |
| 0x707 | 0x1E700000 | Filter radius/strength (high word) |
| 0x708 | 0x00000000 | Reserved |
| 0x709 | 0x00000000 | Reserved |
| 0x70A | 0x00001E70 | Filter radius/strength (low word, matches 0x707 high) |
| 0x70B | 0x000029E0 | Blend/mix factor (varies: 0x29E0-0x2A00, AE-adaptive) |
| 0x70C | 0x30001000 | Per-channel blend ch0: high=0x3000(3.0), low=0x1000(1.0) |
| 0x70D | 0x30001000 | Per-channel blend ch1 |
| 0x70E | 0x30001000 | Per-channel blend ch2 |
| 0x70F | 0x30001000 | Per-channel blend ch3 |

### Dynamic Values

0x705 and 0x70B change slightly between frames — likely adapted
to current AE/exposure level:
- 0x705: 0x4490 → 0x44A0 (noise threshold, adjusts with brightness)
- 0x70B: 0x29E0 → 0x2A00 → 0x29F0 (blend factor)

### Per-Channel Blend (0x70C-0x70F)

All 4 channels use the same value `0x30001000`:
- High 16 bits = 0x3000 (3.0 in Q12) — maximum blend
- Low 16 bits = 0x1000 (1.0 in Q12) — minimum blend

This likely controls how much the filter affects each channel
(luma vs chroma, or R/G/B).

## Also Active in Beauty Mode

Beauty mode also enables:
- **Lens shading** (0xD00 + 0xD0B): same mechanism as color correction
- **Tone curves** (0x652-0x658): S-curve contrast, same as color filters

The gather size increases from 1527w (normal) to 1555w (beauty) due to
the 0xD00 control block (10w) + 0x700 data (16w + opcode overhead).

## Cal Gather Size Comparison

| Mode | Cal Gather Size | 0x700 | 0xD0B | Tone Curves |
|------|----------------|-------|-------|-------------|
| Normal (no filter) | 1527w | zeros | zeros | identity (0x1000) |
| Color filter | 1527-1538w | zeros | non-zero | S-curve |
| Beauty mode | 1555w | enabled | non-zero | S-curve |

## Implications for Kernel Driver

- Register block 0x700 = noise reduction / spatial filter
- Can be enabled independently of lens shading / tone curves
- Useful for soft-skin effect or general denoising
- Parameters are AE-adaptive (change per frame)
- All 4 channels use the same settings (no per-color NR tuning observed)
