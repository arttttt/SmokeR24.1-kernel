# ISP Reprocess Demosaic Investigation

Date: 2026-04-14. Device: Xiaomi Mi Pad 1 (mocha), Tegra K1 (T124).

## Summary

ISP reprocess mode (trigger 0x09+0x0B) works but produces grayscale only.
Demosaic (Bayer→RGB conversion) is not activated. All tested configurations
give single-channel luma output.

## Working Reprocess Configuration

### Minimal init gather (separate submit)
```
SETCLASS(ISP_CLASS)
INCR(0x019, 1) = 0x400    # DMA output threshold
INCR(0x01B, 2) = 0x200    # DMA input threshold
                   0x002    # DMA enable
```

### Per-frame reprocess gather
```
Work buffer: INCR(0x053, 2) = {1, IOVA}
Output: INCR(0xE00-0xE0A) = dims + format 0x43 + surfaces
Input: INCR(0xE31-0xE34, 0xE32, 0xE30) = dims + format + surface + trigger
ISP_ENABLE: INCR(0x015, 1) = 0x07 or 0x04040007
Syncpts: cond 4,5,6
Trigger: NONINCR(0x00C, 1) = 0x09, NONINCR(0x00C, 1) = 0x0B
```

Output: R=0xFF, G=luma, B=0, A=0 in RGBA (format 0x43).

## Register 0x200-0x208: Pipeline Mode Control

### Effect
- **All zeros**: ISP does minimal processing → grayscale luma
- **Any non-zero**: ISP enters full pipeline mode → solid R=255 (broken)
- **Sticky**: writing non-zero poisons ISP state, must zero to recover
- Stock streaming: 0x200=1, 0x202=1, 0x203=0x78|0x78, 0x206=0x600c8, 0x207=0xf000f
- These streaming values don't work for reprocess — different pixel input path

### Interpretation
0x200 enables the ISP internal pixel pipeline for VI-fed streaming.
In reprocess mode (memory input via 0xE34), the pipeline expects
a different configuration. Correct reprocess values are UNKNOWN.

## ISP_ENABLE (0x015) Bit Map

| Value | Result |
|-------|--------|
| 0x07 | Grayscale, works |
| 0x17 | Grayscale, works |
| 0x04040007 | Grayscale, works |
| 0x0F | TIMEOUT (bit 3) |
| 0x1F | TIMEOUT (bit 3+4) |
| 0x27 | TIMEOUT (bit 5) |
| 0x37 | TIMEOUT (bit 4+5) |
| 0x57 | TIMEOUT (bit 6) |

Bits 3 (0x08), 5 (0x20), 6 (0x40) hang ISP in reprocess mode.

## Output Format Experiments

| Format | Output Pattern | Notes |
|--------|---------------|-------|
| 0x43 (R8G8B8A8) | R=FF, G=luma, B=0, A=0 | Single channel |
| 0x41 (A8R8G8B8) | A=luma, R=0, G=0, B=FF | Same, byte order shifted |
| 0x2a (R4G4B4A4) | Luma in nibble | Same |
| 0x04FE00E6 (YUV420) | All zeros | Stride mismatch |

All RGBA/packed formats give single-channel luma — demosaic not active.

## What Does NOT Enable Demosaic

| Tested | Effect |
|--------|--------|
| 0x506 demosaic coefficients (from stock S5) | No change |
| S5 register blocks (0x700/750/506/600/650/C00) | No change (or breaks) |
| 0x200 input config (any values) | Solid red |
| Lens shading 0xD00+0xD0B | No change |
| Tone curves (S-curve) | No change |
| ISP_ENABLE variations | Grayscale or timeout |
| NvIspSetConfiguration type=1+2 | Clears settings, output zeros |
| NvIspProcessFrame | VALIDATE reject (struct layout unknown) |
| Full blob cal gather (3648 words, all zeros) | Same grayscale |

## Cal Gather Contents (blob HwSettingsApply)

3648 words, almost all zeros. Non-zero only:
- 0x91a: [8]=0x200 (stats config)
- 0x91f: 0x02
- 0x00c: 0x0f (trigger post-apply)
- 0x018: 5 words (0, 0x400, 0, 0x200, 0x002) — DMA config
- 0x01f: 0x01
- 0x05f: 0x10

Demosaic coefficients NOT in cal gather. No .isp profile on this device.

## Next Steps

1. **Boot MIUI ROM** and trace ISP init when stock camera produces color
2. Capture register writes during NvIspOpen → HwSettingsCreate → first streaming frame
3. Find what registers enable demosaic in full pipeline mode
4. Likely: 0x200 block needs specific values for reprocess OR demosaic is in HwSettingsCreate shadow registers that only appear with .isp profile loaded
