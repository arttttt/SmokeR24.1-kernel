# ISP Color Correction — How MIUI Camera Adjusts Colors

Captured from stock MIUI Smoke kernel 1.2 via `/proc/isp_trace/gather`.
Date: 2026-04-12. Device: Xiaomi Mi Pad 1 (mocha), Tegra K1 (T124).

## Overview

MIUI camera color filters modify ISP output in real-time by changing two
sets of registers in the calibration gather submitted every frame:

1. **Lens shading table** (0xD0B) — per-pixel color gains across the frame
2. **Tone curves** (0x652/654/656/658) — per-channel gamma/contrast curves

No Color Correction Matrix (CCM at 0x300) is used — the ISP does not
receive CCM data in any observed gather.

## Default State (no color filter)

When no color filter is active:
- **0xD0B**: all 480 words = 0 (lens shading disabled)
- **0xD0A**: 0 (lens shading enable flag = off)
- **0xD00**: not present (ctrl block omitted)
- **0x652-0x658**: all 257 entries = 0x1000 (identity, 1.0 in Q12)

ISP outputs a correct image with these defaults — no calibration needed
for basic operation.

## With Color Filter Active

### Lens Shading (0xD0B, 480 words NONINCR)

All 480 words become non-zero. Values are ~0x00017000-0x0001A000 range.
Each word encodes per-zone color gain corrections.

The data changes between filters — 471/480 words differ between two
observed filters. This is the primary mechanism for color effects.

**Filter A** first 3 words: `00018488 000186d0 00018670`
**Filter B** first 3 words: `00018378 00018648 00018618`

### Lens Shading Control (0xD00, 10 words INCR)

Appears in some gathers when lens shading is first enabled:
```
0xD00: 00000001 00ca4580 006522c0 00ca4580 010db200 0086d900 010db200 05100288 03cc01e6 00000021
```

| Offset | Value | Purpose |
|--------|-------|---------|
| [0] | 0x00000001 | Enable flag |
| [1] | 0x00CA4580 | Center X / gain scale |
| [2] | 0x006522C0 | Center Y / gain scale |
| [3] | 0x00CA4580 | Duplicate of [1] |
| [4] | 0x010DB200 | Radial coefficient |
| [5] | 0x0086D900 | Radial coefficient |
| [6] | 0x010DB200 | Duplicate of [4] |
| [7] | 0x05100288 | Grid dimensions: 0x0510=1296, 0x0288=648 |
| [8] | 0x03CC01E6 | Grid dimensions: 0x03CC=972, 0x01E6=486 |
| [9] | 0x00000021 | Mode flags (changes: 0x21 or 0x24 observed) |

Note: 0xD0A (enable) stays 0 even when lens shading data is present.
The enable flag in 0xD00[0] controls activation instead.

### Tone Curves (0x652/654/656/658, 257 words each NONINCR)

All 4 channels use the same curve (ISP does not apply per-channel
color grading through tone curves).

Format: Q12 fixed point (0x1000 = 1.0, 0x2000 = 2.0, 0x3000 = 3.0).

257 entries = LUT mapping input [0..256] to output gain.

**Observed curve shape** (entries 0-256):
```
[0..64]:   0x1000 (identity, 1.0x) — shadows unchanged
[65..192]: ramp from ~0x1086 to ~0x3000 — midtones boosted
[193..256]: 0x3000 (3.0x clamp) — highlights saturated at 3x
```

This is an S-curve contrast boost: shadows stay, midtones lifted, highlights clipped.

Different filters produce slightly different curves:
- **Filter A** @128: 0x275A (~2.46x)
- **Filter B** @128: 0x2852 (~2.52x)

### Work Buffer (0x053)

Always `enable=1, iova=0x02016B4C` — ISP internal work buffer address
does not change between filters.

## Key Findings

1. **Color correction = lens shading + tone curves**, no CCM
2. **Lens shading is the primary color effect** — 480 words of per-zone
   gain corrections, nearly all differ between filters
3. **Tone curves add contrast** — same curve on all 4 channels,
   boosting midtones 2-3x with shadow/highlight clamp
4. **Default (no filter) = all zeros/identity** — ISP works correctly
   without any calibration data
5. **Per-frame update** — calibration is resubmitted every frame,
   allowing real-time filter switching visible immediately on preview
6. **Ring buffer limitation** — trace captures only last ~6 cal gathers,
   earlier filter switches are lost

## Implications for Kernel Driver

- For basic ISP operation: **no calibration data needed** (zeros work)
- For color quality: load lens shading from `.isp` profile or compute from sensor data
- Tone curves: identity (0x1000) for linear output, or apply gamma in userspace
- Color effects can be implemented by modifying 0xD0B + tone curves per-frame
- CCM is NOT available through ISP registers — do color matrix in software if needed
