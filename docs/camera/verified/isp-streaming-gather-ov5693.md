# ISP-B Streaming Per-Frame Gather — OV5693 Front Camera

Captured from stock MIUI Smoke kernel 1.2 camera app via `/proc/isp_trace/gather`.
Date: 2026-04-12. Device: Xiaomi Mi Pad 1 (mocha), Tegra K1 (T124).

ISP-B (class 0x34) processes frames from OV5693 front sensor (2592×1944).

## Per-Frame Gather (45 words)

Each streaming frame submits this gather after calibration.
ISP receives pixel data from VI automatically (trigger 0x05).

```
SETCLASS(0x34)                          # ISP-B class

# ---- Output configuration ----
INCR(0xE00, 1): 0x0A1F0000             # Output width:  ((0x0A1F >> 0) & 0x3FFF) + 1 = 2592
INCR(0xE01, 1): 0x07970000             # Output height: ((0x0797 >> 0) & 0x3FFF) + 1 = 1944
INCR(0xE02, 1): 0x04FE00E6             # Output pixel format (YUV420 planar)
INCR(0xE03, 1): 0x00000000             # Output color config (unused)

# ---- Output surfaces (YUV420 planar: Y + U + V) ----
INCR(0xE04, 3): IOVA 0x00000000 2624   # Y plane:  stride=0xA40=2624 (W aligned to 64)
INCR(0xE07, 3): IOVA 0x00000000 1344   # U plane:  stride=0x540=1344 (W/2 aligned to 64)
INCR(0xE0A, 3): IOVA 0x00000000 1344   # V plane:  stride=0x540=1344 (W/2 aligned to 64)

# ---- Processing block ----
INCR(0x500, 6):
  0x00000000                            # [0] flags: 0 = default processing
  0x00000000                            # [1] reserved
  0x00000000                            # [2] reserved
  0x00000000                            # [3] reserved
  0x00000000                            # [4] reserved
  0x07980A20                            # [5] dims: H=1944(0x798) × W=2592(0xA20)

# ---- Stats buffer ----
SETCLASS(0x34)
INCR(0x100, 4): IOVA 0x00000000 0x00000000 0x00000000
                                        # Stats output buffer (AE/AWB/AF)

# ---- Syncpoint conditional increments ----
SETCLASS(0x34)
SETCLASS(0x34)
NONINCR(0x000, 1): 0x00000424          # cond=4 (OP_DONE)    | syncpt_memory
NONINCR(0x000, 1): 0x00000525          # cond=5 (STATS_DONE) | syncpt_stats
NONINCR(0x000, 1): 0x00000627          # cond=6 (RD_DONE)    | syncpt_loadv

# ---- Trigger ----
SETCLASS(0x34)
NONINCR(0x00C, 1): 0x00000005          # ISP_CONTROL = 0x05 (streaming runtime trigger)
```

## Calibration Gather (~1527 words)

Submitted before each per-frame gather. Contains:
- Lens shading tables (0xD00-0xD0B, 480 words) — all zeros on this device
- Stats config (0x900-0x91F)
- Tone curves (0x651-0x658, 4×257 words) — identity (0x1000)
- ISP enable (0x053)
- Post-apply trigger (0x00C = 0x0F)

Note: calibration data is all zeros because no `.isp` profile is installed
on this MIUI ROM. The ISP operates with default/identity settings.

## Key Observations

1. **Format 0x04FE00E6** is the real stock streaming format (YUV420 planar),
   not 0x010000C9 (which was from MIUI blob's HwSettingsApply init, not per-frame)
2. **No input registers (0xE30-0xE3A)** — streaming mode receives data from VI
3. **ISP_ENABLE not in per-frame** — set once during init via calibration gather
4. **Per-frame submit is small** (45 words) — just output config + trigger
5. **Processing flags = 0** — no special flags needed

## Register Reference

| Register | Name | Description |
|----------|------|-------------|
| 0x00C | ISP_CONTROL | Trigger: 0x05=streaming, 0x0B=reprocess, 0x0F=post-apply |
| 0x015 | ISP_ENABLE | Pipeline enable: 0x07=full, 0x04040007=streaming+stats |
| 0x053 | ISP_WORK_BUF | Work buffer enable + IOVA |
| 0x100 | STATS_BUF | Stats output buffer IOVA |
| 0x500 | PROCESSING | Processing config: [flags, 4×reserved, H<<16\|W] |
| 0x651-0x658 | TONE_CURVES | 4 channels × (1 ctrl + 257 LUT entries), 0x1000=identity |
| 0xD00 | LS_CTRL | Lens shading control (10 words) |
| 0xD0A | LS_ENABLE | Lens shading enable |
| 0xD0B | LS_TABLE | Lens shading LUT (480 words) |
| 0xD20 | LS_EXTRA | Lens shading extra config (6 words) |
| 0xE00 | OUT_WIDTH | Output width: ((W-1) & 0x3FFF) << 16 |
| 0xE01 | OUT_HEIGHT | Output height: ((H-1) & 0x3FFF) << 16 |
| 0xE02 | OUT_FORMAT | Output pixel format |
| 0xE03 | OUT_COLOR | Output color config |
| 0xE04 | OUT_SURF_Y | Y plane: [IOVA, 0, stride] |
| 0xE07 | OUT_SURF_U | U plane: [IOVA, 0, stride] |
| 0xE0A | OUT_SURF_V | V plane: [IOVA, 0, stride] |
| 0xE30 | IN_TRIGGER | Input trigger (reprocess only): 1=fire |
| 0xE31 | IN_DIMS | Input dims: H<<16 \| W |
| 0xE32 | IN_STRIP | Strip config: W & 0x3FFF |
| 0xE33 | IN_FORMAT | Input pixel format (e.g. 0x10200024 = BG10) |
| 0xE34 | IN_SURF0 | Input plane 0: [IOVA, 0, stride] |

## Format Codes (verified)

| Code | Format | BPP | Notes |
|------|--------|-----|-------|
| 0x04FE00E6 | YUV420 planar | Y:8, UV:8 | Stock streaming output, Y+U+V planes |
| 0x010000C9 | Unknown YUV | 32? | From blob init, causes W/2 in reprocess |
| 0x43 | R8G8B8A8 | 32 | Works for reprocess, full width |
| 0x41 | A8R8G8B8 | 32 | Works for reprocess, byte order differs |
| 0x10200024 | Bayer BGGR 10-bit | 16 | Input format (16-bit LE container) |
