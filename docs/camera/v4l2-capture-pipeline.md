# V4L2 Capture Pipeline — Tegra K1 (T124)

## Architecture

Capture ops abstraction with two implementations:

```
                    ┌─ t124_capture.c ──── continuous mode, 2-kthread pipeline
channel.c ─ ops ──►│
                    └─ singleshot_capture.c ── single-shot, ring buffer (TPG/ISP/T210)
```

Selected at `start_streaming` based on hardware config and module params.

## T124 Continuous Mode (default)

PP runs in continuous mode (no SINGLE_SHOT), captures every CSI frame.

### Threads

| Thread | Role | Syncpt |
|--------|------|--------|
| `kthread_capture_start` | Program SURFACE0, arm+wait FRAME_START | `syncpt[]` |
| `kthread_capture_done` | Arm+wait MW_ACK_DONE, return buffer | `syncpt_mw[]` |

Separate syncpoints avoid INCR_SYNCPT FIFO conflicts between threads.

### Per-frame flow

```
start-thread:                        done-thread:
  dequeue buffer                       wait for buffer in done queue
  program SURFACE0 = buf.addr          arm MW_ACK_DONE (syncpt_mw)
  arm FRAME_START (syncpt)             wait MW_ACK_DONE
  wait FRAME_START                     signal dma_active=0
  set dma_active=1                     return buffer to userspace
  enqueue buf to done queue
  → loop (next buffer)
```

### Warmup (first frame)

1. `enable_stream` — sensor s_stream + CSI PHY init
2. Wait 2x FRAME_START **without DEST_MEM** — sensor stabilizes, no DMA
3. Enable DEST_MEM — subsequent frames write to memory
4. Proceed with normal capture

No garbage frames reach userspace.

## Key Registers

### PP_COMMAND (Pixel Parser Command)

| Value | Mode |
|-------|------|
| `0xf003` | RST (continuous) |
| `0xf001` | ENABLE (continuous) |
| `0xf007` | RST + SINGLE_SHOT |
| `0xf005` | ENABLE + SINGLE_SHOT |

### Module Parameters

| Parameter | Default | Sysfs | Description |
|-----------|---------|-------|-------------|
| `t124_single_shot` | 0 | `/sys/module/channel/parameters/` | 0=continuous, 1=single-shot |
| `t124_csi_tpg` | 0 | same | Test pattern generator |

## Performance

### IMX179 (rear, 4-lane CSI-A)

| Resolution | FPS | Notes |
|-----------|-----|-------|
| 1280x720 | **31 fps** | Max sensor rate |
| 1920x1080 | **31 fps** | Max sensor rate |
| 3264x2448 | **5.2 fps** | DMA bandwidth limit |

### OV5693 (front, 1-lane CSI-E)

| Resolution | FPS | Notes |
|-----------|-----|-------|
| 1280x720 | **15.5 fps** | 1-lane bandwidth |
| 1920x1080 | **20.7 fps** | Best mode |
| 2592x1944 | **15.5 fps** | Full res |

## Sensor Controls

### IMX179
- `V4L2_CID_EXPOSURE`: microseconds → coarse_time (lines)
- `V4L2_CID_GAIN`: direct value, formula `reg = 256 - 256/gain`

### OV5693
- `V4L2_CID_EXPOSURE`: microseconds → coarse_time (lines, no fractional shift)
- `V4L2_CID_GAIN`: value in `real_gain << 8` format (gain 1x=256, 16x=4096)

## Files

```
drivers/media/platform/tegra/camera/
├── vi_capture.h              — ops struct, shared helper prototypes
├── t124_capture.c            — T124 continuous capture (default)
├── singleshot_capture.c      — legacy single-shot capture
├── channel.c                 — V4L2/vb2 glue, shared helpers, ops dispatch
├── mc_common.h               — tegra_channel struct
├── registers.h               — VI/CSI register offsets
└── t124_registers.h          — T124-specific registers

drivers/media/i2c/
├── imx179_mocha.c            — IMX179 rear sensor
├── imx179_mocha_mode_tbls.h  — IMX179 mode tables
├── ov5693_mocha.c            — OV5693 front sensor
└── ov5693_mocha_mode_tbls.h  — OV5693 mode tables

tools/camera/
└── v4l2_diag.c               — diagnostic/benchmark tool
```

## Known Limitations

- Full res IMX179 capped at ~5fps (DMA bandwidth, not sensor)
- OV5693 limited by 1-lane CSI-E (~20fps max at 1080p)
- High FPS modes (60/90/120) don't reach target — mode table switching TBD
- No ISP — CPU Bayer demosaic in HAL
