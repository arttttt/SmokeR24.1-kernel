# Media Controller Framework on Tegra T124 — Feasibility Analysis

## Status: UNTESTED on real hardware (we are the first)

NVIDIA's Bryan Wu (May 2016 commit 201ce36d1dd):
> "Since media controller eventually will replace soc_camera and
> **T124 might not be supported in rel-24**. So disable soc_camera
> and build in media controller driver by default as T210."

No T124 board in the NVIDIA downstream kernel has ever shipped with
MC framework DTS. All T124 boards (Ardbeg, TN8, Loki, mocha) use
the old PCL/soc_camera framework. Our `tegra124-mocha-camera-mc.dtsi`
is the first attempt to use MC on T124 hardware.

---

## 1. Code Support Status

### T124 compatible string: YES

`drivers/media/platform/tegra/vi/vi.c:60-64`:
```c
static struct of_device_id tegra_vi_of_match[] = {
#ifdef TEGRA_12X_OR_HIGHER_CONFIG
    { .compatible = "nvidia,tegra124-vi",
        .data = (struct nvhost_device_data *)&t124_vi_info },
#endif
```

### t124_vi_info vs t21_vi_info comparison

Source: `drivers/video/tegra/host/t124/t124.c:211-237`
vs `drivers/video/tegra/host/t210/t210.c:139-171`

| Field | T124 | T210 | Notes |
|-------|------|------|-------|
| num_channels | 2 | 6 | T124 HW limit: 2 CSI ports |
| class | NV_VIDEO_STREAMING_VI_CLASS_ID | same | |
| powergate_id | TEGRA_POWERGATE_VENC | TEGRA_POWERGATE_VE | Different domains, both correct |
| clocks | vi_bypass, csi, cilab, cilcd, cile, emc, sclk | +vii2c, +i2cslow | T210 has VI-internal I2C |
| slcg_notifier_enable | absent (false) | true | SLCG optimization, not functional |
| bond_out_id | absent | BOND_OUT_VI | T210 fusing, T124 doesn't use |
| finalize_poweron | nvhost_vi_finalize_poweron | same | Same function for both |
| prepare_poweroff | nvhost_vi_prepare_poweroff | same | |
| reset | nvhost_vi_reset_all | absent | T124 has explicit reset |

### MC framework components: all T124-compatible

| Component | File | T124 Status |
|-----------|------|-------------|
| VI probe + MC init | vi/vi.c | OK — uses t124_vi_info, calls tegra_vi_media_controller_init() |
| Channel/streaming | camera/channel.c | OK — T210-only code behind #ifdef TEGRA_21x |
| Graph/DT parsing | camera/graph.c | OK — pure DT parsing, no SoC-specifics |
| CSI driver | csi/csi.c | OK — abstract register access via port pointers |
| camera_common | camera/camera_common.c | OK — clock/regulator/port parsing is generic |
| VI IRQ | vi/vi_irq.c | OK — NUM_VI_WATCHDOG=2 for T124, 6 for T210 |
| VI power | vi/tegra_vi.c | OK — T124 CG_CTRL write in finalize_poweron |

---

## 2. Potential Issues — Detailed Analysis

### 2.1 TEGRA_CSI_BLOCKS = 3 (hardcoded for 6 ports)

**File:** `camera/registers.h:214`

CSI driver maps 3 pixel parser iomem regions (6 ports total).
T124 only has ports A and B (block 0). Blocks 1 and 2 map to
non-existent registers.

**Risk:** None, as long as DTS only configures `csi-port = <0>` and
`csi-port = <2>`. The extra mappings are never accessed.

**Action:** None needed. DTS correctly limits to 2 channels.

### 2.2 Missing slcg_notifier_enable in t124_vi_info

**What it does on T210:** Registers a PLL_D clock source notifier
for power state transitions (SLCG = Second Level Clock Gating).

**T124 reality:** Clock gating is handled differently — via direct
register write `T12_VI_CFG_CG_CTRL` (0xb8) in `nvhost_vi_finalize_poweron()`.

**Risk:** None. This is a power optimization, not a functional requirement.

### 2.3 Powergate VENC vs VE

T124 uses `TEGRA_POWERGATE_VENC` for VI. T210 uses `TEGRA_POWERGATE_VE`.
This is correct for both SoCs — the powergate topology changed between
generations. On T124, VI shares the VENC domain. On T210, VI has its
own VE domain.

**Risk:** None. Both legacy vi2.c and MC vi.c use VENC for T124.

### 2.4 Missing vii2c / i2cslow clocks

T210 has an I2C controller embedded inside the VI block (`i2c@546c0000`),
with dedicated clocks. T124 does NOT have this — sensors are on the
regular APB I2C bus (`i2c@7000c400` / I2C2).

**Risk:** None. Absence is correct for T124 hardware.

### 2.5 MIPI Calibration returns -ENOSYS

**File:** `include/media/mipi_cal.h`
```c
#if defined(CONFIG_ARCH_TEGRA_21x_SOC)
  extern int tegra_mipi_bias_pad_enable(void);
#else
  int tegra_mipi_bias_pad_enable(void) { return -ENOSYS; }
#endif
```

`channel.c` calls `tegra_mipi_bias_pad_enable()` in `tegra_channel_start()`.
On T124 it returns `-ENOSYS` but the **return value is not checked** —
execution continues normally.

The full `tegra_channel_mipi_cal()` function is behind
`#if defined(CONFIG_ARCH_TEGRA_21x_SOC)` and compiles as `return 0`
on T124.

**T124 MIPI calibration reality:** T124 has its own MIPI calibrator
at `mipi-cal@700e3000`. The legacy vi2.c driver uses it via
`tegra_mipi_cal_clk_enable()`. The MC framework does NOT call this.

**Risk:** MEDIUM. MIPI calibration may be needed for stable high-speed
capture. However, basic capture should work without it — especially on
2-lane CSI-B (OV5693). If we see bit errors or frame corruption, adding
T124 MIPI cal to the MC framework is the fix.

**Workaround:** MIPI cal can be performed once at boot by a separate
driver or in the sensor power-on sequence, before MC framework starts
streaming.

### 2.6 Never tested on real hardware

**Risk:** HIGH. This is the only real concern. All code paths look correct
from static analysis, but there could be:
- Register timing differences between T124 and T210
- DMA buffer alignment assumptions
- Race conditions in capture loop exposed by T124's 2-channel limit
- CSI PHY initialization differences not covered by the MC framework
- Edge cases in v4l2_async_register_subdev on T124

**Mitigation:** Incremental testing:
1. First verify VI probes and creates /dev/videoN
2. Then verify OV5693 subdev probes and links to VI
3. Then attempt single-frame capture
4. Then continuous streaming

---

## 3. Two VI Drivers — Conflict Risk

Both drivers match `compatible = "nvidia,tegra124-vi"`:

| Driver | Location | Framework | Config |
|--------|----------|-----------|--------|
| MC vi.c | drivers/media/platform/tegra/vi/vi.c | Media Controller | VIDEO_TEGRA_VI |
| Legacy vi2.c | drivers/media/platform/soc_camera/tegra_camera/vi2.c | soc_camera | VIDEO_TEGRA (+ SOC_CAMERA) |

Only ONE can be active. Current mocha defconfig state:
- `VIDEO_TEGRA_VI=y` (MC) — ENABLED
- `SOC_CAMERA` — not set (disabled)
- `VIDEO_TEGRA` — not set (disabled)

This is correct. If both were enabled, the first driver to probe()
would claim the device node and the second would fail.

---

## 4. What Needs To Work vs What Is Optional

### Required for basic RAW capture (Phase 1):
- [x] VI probe with t124_vi_info ← code exists
- [x] CSI port setup (ports A, B) ← code exists
- [x] OV5693 V4L2 subdev probe ← ov5693.c exists
- [x] V4L2 async subdev matching ← graph.c exists
- [x] VB2 DMA buffer allocation ← channel.c exists
- [x] Streaming start/stop ← channel.c exists
- [ ] **OV5693 mocha power sequence** ← needs custom driver
- [ ] **DTS integration** ← tegra124-mocha-camera-mc.dtsi created

### Not required for basic capture:
- ISP processing (isp_t124.c — future work)
- MIPI calibration (can add later if needed)
- SLCG optimization
- Bandwidth management via isomgr (nice to have)

---

## 5. Files Reference

### MC Framework (all under drivers/media/platform/tegra/):
```
vi/vi.c              — VI host1x client, probe, MC init
vi/tegra_vi.c        — Power on/off, ioctl, reset, bandwidth
vi/vi.h              — Structs, register defines
vi/vi_irq.c          — IRQ handler, watchdog
camera/channel.c     — V4L2 video device, VB2 buffers, capture loop
camera/graph.c       — Media controller graph, DT port/endpoint parsing
camera/mc_common.c   — MC helpers, power, v4l2_dev init
camera/core.c        — Format tables
camera/camera_common.c — Sensor resource management (clocks, regulators, ports)
camera/registers.h   — VI/CSI register offsets
csi/csi.c            — CSI PHY setup, streaming start/stop
csi/csi.h            — CSI port structs, enums
```

### Device data:
```
drivers/video/tegra/host/t124/t124.c  — t124_vi_info (lines 211-237)
drivers/video/tegra/host/t210/t210.c  — t21_vi_info (lines 139-171)
```

### Headers:
```
include/media/mipi_cal.h    — MIPI calibration stubs for non-T210
include/media/camera_common.h — camera_common framework API
include/media/ov5693.h       — OV5693 platform data, IOCTL defs
```

---

## 6. Conclusion

The MC framework for T124 is **architecturally complete** — all necessary
code paths exist and are correctly guarded for T124 vs T210 differences.
The only real risk is that nobody has ever tested it on hardware.

We should proceed with MC framework (not fall back to soc_camera) because:
1. soc_camera is deprecated and harder to extend
2. MC framework is the standard Linux V4L2 approach
3. The code is cleaner and better structured
4. Future ISP integration will be easier with MC

The bring-up strategy is: start with OV5693 on CSI-B (simpler, 2-lane),
get basic RAW capture working, then expand to IMX179 and ISP.
