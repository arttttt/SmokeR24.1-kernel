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
1-lane CSI-E (OV5693). If we see bit errors or frame corruption, adding
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

The bring-up strategy is: start with OV5693 on CSI-E (simpler, 1-lane),
get basic RAW capture working, then expand to IMX179 and ISP.

---

## 7. Mitigation Plan: Closing Potential Issues

### 7.1 MIPI Calibration — Ready to Port

The MC framework's `tegra_mipi_bias_pad_enable()` returns `-ENOSYS`
on T124. If MIPI cal turns out to be needed (bit errors, frame
corruption at high speeds), we have a **complete reference** in the
legacy vi2.c driver.

**Source:** `drivers/media/platform/soc_camera/tegra_camera/vi2.c:1615-1645`

The entire T124 MIPI bias pad init is 2 register writes:
```c
// MIPI calibrator at 0x700e3000
clk_mipi_cal = clk_get_sys("mipi-cal", NULL);
mipi_cal = ioremap(0x700e3000, 0x100);
regs = devm_regmap_init_mmio(&pdev->dev, mipi_cal, &mipi_cal_config);

clk_prepare_enable(clk_mipi_cal);
regmap_update_bits(regs, 0x58, (1 << 0), 0);  // MIPI_BIAS_PAD_CFG0: clear E_VCLAMP_REF
regmap_update_bits(regs, 0x60, (1 << 1), 0);  // MIPI_BIAS_PAD_CFG2: clear PDVREG
clk_disable_unprepare(clk_mipi_cal);
```

Register defines (from vi2.c):
```
MIPI_CAL_BASE           = 0x700e3000
MIPI_BIAS_PAD_CFG0      = 0x58   (bit 0 = E_VCLAMP_REF, bit 1 = PDVCLAMP)
MIPI_BIAS_PAD_CFG1      = 0x5c
MIPI_BIAS_PAD_CFG2      = 0x60   (bit 1 = PDVREG)
```

**Fix approach:** Add ~20 lines to `channel.c` behind
`#ifdef CONFIG_ARCH_TEGRA_12x_SOC` in `tegra_channel_start_streaming()`,
or create a helper in `camera_common.c`. This can be done in minutes
when needed.

### 7.2 Runtime Debugging — vi2.c as Golden Reference

The legacy `vi2.c` (1700+ lines) is a **complete, production-tested**
T124 camera implementation using the same hardware registers. If MC
framework doesn't work on T124, we can byte-for-byte compare what
vi2.c writes vs what MC channel.c writes.

**Key comparison points:**

| Operation | MC framework (channel.c) | Legacy (vi2.c) | Same registers? |
|-----------|-------------------------|----------------|-----------------|
| CSI PP setup | `csi_write(TEGRA_CSI_PIXEL_PARSER_0_BASE+...)` | Direct VI aperture writes to 0x838+ | YES — same offsets |
| Image format | `TEGRA_VI_CSI_IMAGE_DEF` (0x048) | `TEGRA_VI_CSI_0_IMAGE_DEF` (0x120) | DIFFERENT offsets — needs verification |
| Surface addr | `TEGRA_VI_CSI_SURFACE0_OFFSET_MSB/LSB` | `TEGRA_VI_CSI_0_SURFACE0_OFFSET_MSB` | Same concept, may differ in offset |
| Single shot | `TEGRA_VI_CSI_SINGLE_SHOT` | Direct trigger write | Same mechanism |
| Error status | `TEGRA_VI_CSI_ERROR_STATUS` | `VI_CSI_0_ERROR_STATUS` (0x184) | YES |
| Stream on/off | `tegra_csi_start_streaming()` | `vi2_csi_start()` inline code | Same register sequence |
| Power on | `nvhost_vi_finalize_poweron()` | Same function | Identical |

**Important note on register offsets:**

The MC framework's `channel.c` accesses VI/CSI registers via
`csi_write(chan, index, offset)` where `offset` is **relative to the
CSI port's pixel parser base**. The legacy vi2.c uses **absolute
offsets from the VI aperture base** (e.g., 0x120 for CSI_0_IMAGE_DEF).

Both ultimately write to the same physical registers. The mapping is:
```
MC:     csibase[0] = vi_aperture + 0x0838 (PP0_BASE)
        csi_write(chan, 0, TEGRA_VI_CSI_IMAGE_DEF)
        → writes to vi_aperture + 0x0838 + TEGRA_VI_CSI_IMAGE_DEF

Legacy: vi_aperture + TEGRA_VI_CSI_0_IMAGE_DEF (absolute offset)
```

The offsets in `registers.h` vs vi2.c need to be verified to match
when debugging. Both refer to the same hardware, just different
addressing styles.

### 7.3 CSI Register Mapping — Verified Compatible

CSI pixel parser base addresses in MC framework (`registers.h`):
```
TEGRA_CSI_PIXEL_PARSER_0_BASE = 0x0838  (ports A, B)
TEGRA_CSI_PIXEL_PARSER_2_BASE = 0x1038  (ports C, D — T210 only)
TEGRA_CSI_PIXEL_PARSER_4_BASE = 0x1838  (ports E, F — T210 only)
```

T124 VI aperture: `0x54080000`, size `0x40000` (256KB). All three
PP bases fall within this range, so `ioremap()` won't fail. PP2 and
PP4 physically don't exist on T124 silicon but are never accessed
when DTS limits channels to 2 with `csi-port = <0>` and `<2>`.

Legacy vi2.c uses the same base (0x0838) for CSI-A, confirming
PP0_BASE is correct for T124.

### 7.4 DMA Buffer Management — Same Allocator

Both MC (channel.c) and legacy (vi2.c) use `vb2_dma_contig_memops`
for buffer allocation. The DMA addressing, IOMMU (SMMU) group
assignment, and buffer alignment requirements are identical.

### 7.5 Power and Clock — Confirmed Working

`nvhost_vi_finalize_poweron()` in `tegra_vi.c` is shared by both
MC and legacy paths. It has explicit T124 handling:
```c
#ifdef CONFIG_ARCH_TEGRA_12x_SOC
    if (dev->id == 0)
        host1x_writel(dev, T12_VI_CFG_CG_CTRL, T12_CG_2ND_LEVEL_EN);
#endif
```

This function is called during VI probe regardless of which camera
framework is active. Clock list in `t124_vi_info` (vi_bypass, csi,
cilab, cilcd, cile, emc, sclk) is the same for both frameworks.

### 7.6 What We Don't Have (and don't need yet)

| Missing | Impact | When needed |
|---------|--------|-------------|
| `arisp.h` (ISP register fields) | Cannot program ISP | Phase 3 (ISP driver) |
| T124 MIPI cal in MC framework | May need for 4-lane CSI-A | Phase 2 (IMX179) or earlier if OV5693 has issues |
| Runtime trace comparison | Need live device | First boot with MC |

---

## 8. Summary Risk Matrix

| Risk | Probability | Impact | Mitigation | Effort to fix |
|------|------------|--------|------------|---------------|
| MIPI cal needed | Medium | Frame corruption | Port 2 register writes from vi2.c | ~20 lines |
| Register offset mismatch | Low | Capture fails | Compare MC vs vi2.c register traces | Hours of debugging |
| DMA/buffer issue | Low | No frames | Same allocator as vi2.c | Low |
| Power sequence wrong | Very low | Sensor won't init | Same finalize_poweron | N/A (shared code) |
| Async subdev race | Low | Probe order issue | Standard V4L2 pattern | Moderate |
| Unknown T124 quirk | Unknown | Unknown | vi2.c as byte-level reference | Varies |
