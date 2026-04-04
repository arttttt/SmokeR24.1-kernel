# ISP Stats Readback — Decoded

## Mechanism

Stats are **NOT** read via registers/MMIO. ISP writes stats into a **DMA buffer**
at offset 0x20000 (128KB) from the base address programmed via `INCR(0x100, 4)`.

This is the same buffer we thought was "input" — it's actually a **shared
ISP working buffer** used for both input data and stats output.

## NvIspGetStats Flow (from libnvisp_v3.so at 0xAE50)

1. `NvOsMutexLock` — take stats mutex
2. Search for matching syncpt in per-channel array (stride 0x2D0)
3. `NvRmFenceWait` — wait for ISP frame completion syncpoint
4. `NvRmMemRead(handle, local_buf, 0x20000)` — read 128KB from ISP DMA buffer
5. Parse stats header at `local_buf[0x0C]`:
   - Bits 24-31: type (1-6)
   - Bits 0-23: data offset
6. Table dispatch for 6 stats types
7. Inner loop: for each window, extract 4 packed int16 pairs
8. `NvOsMutexUnlock`

## Stats Data Format

```
ISP DMA buffer layout:
+0x00000: ISP working memory (input/processing)
+0x20000: Stats output region (128KB)
  +0x00: header
  +0x0C: type_count word (type << 24 | count)
  +0x10: stats data
    Each window entry: 16 bytes (4 x uint32)
    Each uint32 contains 2x int16 (low/high half)
    Channels: R, G, B per window
```

## 6 Stats Types

From table dispatch at 0xAF3E:
1. AE luminance
2. AWB color ratios
3. AF sharpness metrics
4. Histogram
5. Flicker detection
6. (reserved?)

## Implications for Kernel Driver

- `INCR(0x100, 4)` buffer must be >= 256KB (128KB working + 128KB stats)
- After ISP frame completion (syncpt), read stats from buffer+0x20000
- Parse header at +0x0C for type and count
- Extract int16 pairs from +0x10 onwards

## Stats Config

Programmed per-frame via:
- `INCR(0x902, 1) + NONINCR(0x903, 64)` — AE/AWB window config
- `INCR(0x906, 1) + NONINCR(0x907, 36)` — AF/focus window config

Config data captured in `stock-isp-stats-config.txt`.
Config changes per-frame (3A feedback loop).
