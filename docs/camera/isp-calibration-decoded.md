# ISP Calibration Block Decoded

## Source
Captured from stock Smoke kernel 1.2 with ISP cmdbuf hex dump.
Decoded from `stock-isp-a-calibration.txt` and `stock-isp-b-calibration.txt`.

## Block Structure

### ISP-A (IMX179, 1545 words)

| Index | Opcode | Method | Count | Description |
|-------|--------|--------|-------|-------------|
| 0 | SET_CLASS(0x32) | — | — | ISP-A class |
| 1 | INCR | 0xD00 | 10 | Lens shading control params |
| 12 | INCR | 0xD0A | 1 | Lens shading enable (=0) |
| 14 | NONINCR | 0xD0B | 480 | Lens shading correction table (FIFO) |
| 495 | INCR | 0xD20 | 6 | Lens shading extra params |
| 502 | INCR | 0x651 | 1 | Tone curve ch0 control (=0) |
| 504 | NONINCR | 0x652 | 257 | Tone curve ch0 LUT (FIFO, 257 entries) |
| 762 | INCR | 0x653 | 1 | Tone curve ch1 control (=0) |
| 764 | NONINCR | 0x654 | 257 | Tone curve ch1 LUT |
| 1022 | INCR | 0x655 | 1 | Tone curve ch2 control (=0) |
| 1024 | NONINCR | 0x656 | 257 | Tone curve ch2 LUT |
| 1282 | INCR | 0x657 | 1 | Tone curve ch3 control (=0) |
| 1284 | NONINCR | 0x658 | 257 | Tone curve ch3 LUT |
| 1542 | INCR | 0x053 | 2 | ISP enable + buffer addr (0x00745f5c) |

### ISP-B (OV5693, 1538 words)

Same structure except:
- No 0xD20 block (7 fewer words)
- Different lens shading values (per-sensor calibration)
- Different 0x053 addr (0x02016b4c)
- Tone curves identical (all linear 0x1000)

## Method Map

### Lens Shading (0xD00-0xD20)
```
0xD00: 10 control registers
  [0] = 0x00000001 (enable)
  [1-8] = lens falloff coefficients (per-channel, per-sensor)
  [9] = packed control bits
0xD0A: 1 register (enable/mode = 0)
0xD0B: 480 entries via FIFO (NONINCR)
  Lens shading correction mesh, 4 channels x 120 points
  Values are fixed-point correction factors
0xD20: 6 extra control registers (ISP-A only)
  [0] = 0x00003101 (mesh dimensions?)
  [1] = 0x0000004c
  [2-3] = 0x07ae0444 (scale factors)
  [4-5] = more scale factors
```

### Tone Curves (0x651-0x658)
```
4 channels, each:
  0x65N: 1 control word (=0)
  0x65(N+1): 257 LUT entries via FIFO (NONINCR)

Stock values: ALL 0x00001000 = 4096 = LINEAR IDENTITY
This means stock camera runs with linear gamma (no tone mapping).
ISP profiles would override these with actual gamma curves.

LUT format: 12-bit fixed-point (0x1000 = 1.0)
257 entries for 256 input levels + 1 endpoint
```

### ISP Enable (0x053)
```
0x053: 2 words
  [0] = 0x00000001 (ISP enable)
  [1] = buffer address (IOVA) — different per ISP instance
```

## Comparison: ISP-A vs ISP-B

| Component | ISP-A (IMX179) | ISP-B (OV5693) | Same? |
|-----------|---------------|----------------|-------|
| Lens shading structure | D00+D0A+D0B+D20 | D00+D0A+D0B | No D20 for ISP-B |
| Lens shading values | Per-sensor | Per-sensor | Different |
| Tone curves (4ch) | Linear 0x1000 | Linear 0x1000 | Identical |
| ISP enable | 1 | 1 | Same |
| Total words | 1545 | 1538 | ISP-A has 7 more |

## New Method Offsets (not in original RE doc)

These methods were not in the original 24 extracted from libnvisp_v3.so:

| Method | Type | Count | Block | Description |
|--------|------|-------|-------|-------------|
| 0x053 | INCR | 2 | Control | ISP enable + buffer addr |
| 0x651 | INCR | 1 | Tone Curve | Channel 0 control |
| 0x652 | NONINCR | 257 | Tone Curve | Channel 0 LUT (FIFO) |
| 0x653 | INCR | 1 | Tone Curve | Channel 1 control |
| 0x654 | NONINCR | 257 | Tone Curve | Channel 1 LUT (FIFO) |
| 0x655 | INCR | 1 | Tone Curve | Channel 2 control |
| 0x656 | NONINCR | 257 | Tone Curve | Channel 2 LUT (FIFO) |
| 0x657 | INCR | 1 | Tone Curve | Channel 3 control |
| 0x658 | NONINCR | 257 | Tone Curve | Channel 3 LUT (FIFO) |
| 0xD00 | INCR | 10 | Lens Shading | Control params |
| 0xD0A | INCR | 1 | Lens Shading | Enable |
| 0xD0B | NONINCR | 480 | Lens Shading | Correction table (FIFO) |
| 0xD20 | INCR | 6 | Lens Shading | Extra params (ISP-A only) |

Note: Original RE doc had 0xD31 and 0xDAF for lens shading — stock uses
0xD00/0xD0B/0xD20 instead. Different method offsets between mocha (50KB)
and shield (63KB) libnvisp_v3.so binaries, or the original RE was from
a different code path.
