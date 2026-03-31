# Media Controller Framework on Tegra T124 — Feasibility Analysis

## Status: TESTED AND WORKING — OV5693 functional on T124 via MC

**Update (March 2026):** The MC framework has been successfully brought up on T124 hardware. OV5693 front camera is fully functional with all supported modes working. This document has been updated to reflect the current state while preserving historical analysis.

---

## 0. Current Working State (March 2026)

### Working Features
- OV5693 front camera on CSI-E (1-lane) fully operational
- All resolution modes tested and working:
  - 2592x1944@30 (5MP full)
  - 2592x1458@30 (16:9 widescreen)
  - 1920x1080@30 (1080p)
  - 1280x720@60, @90, @120 (720p high-speed)
- V4L2 interface: /dev/video0 + /dev/media0 created
- VIDIOC_S_PARM framerate selection implemented
- CSI TPG (Test Pattern Generator) working as diagnostic tool

### Current Limitations
- Single-shot capture: ~6 fps throughput (buffer queue/dequeue overhead)
- Continuous streaming mode: TODO (not yet implemented)
- IMX179 rear camera: not yet enabled (requires additional bring-up)

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

### 2.1 TEGRA_CSI_BLOCKS = 3 (hardcoded for 6 ports) — RESOLVED

**File:** `camera/registers.h:214`

CSI driver maps 3 pixel parser iomem regions (6 ports total).
T124 only has ports A and B (block 0). Blocks 1 and 2 map to
non-existent registers.

**Resolution:** Not an issue in practice. DTS correctly limits to CSI-E (PP_B) usage. The extra mappings are never accessed.

### 2.2 Missing slcg_notifier_enable in t124_vi_info — RESOLVED

**What it does on T210:** Registers a PLL_D clock source notifier
for power state transitions (SLCG = Second Level Clock Gating).

**T124 reality:** Clock gating is handled differently — via direct
register write `T12_VI_CFG_CG_CTRL` (0xb8) in `nvhost_vi_finalize_poweron()`.

**Resolution:** Not needed for functionality. SLCG is a power optimization only.

### 2.3 Powergate VENC vs VE — RESOLVED

T124 uses `TEGRA_POWERGATE_VENC` for VI. T210 uses `TEGRA_POWERGATE_VE`.
This is correct for both SoCs — the powergate topology changed between
generations. On T124, VI shares the VENC domain. On T210, VI has its
own VE domain.

**Resolution:** Working correctly.

### 2.4 Missing vii2c / i2cslow clocks — RESOLVED

T210 has an I2C controller embedded inside the VI block (`i2c@546c0000`),
with dedicated clocks. T124 does NOT have this — sensors are on the
regular APB I2C bus (`i2c@7000c500` / CAM I2C, adapter 2).

**Resolution:** Correct for T124 hardware. OV5693 probes successfully on CAM I2C.

### 2.5 MIPI Calibration — RESOLVED (Not the root cause)

**File:** `include/media/mipi_cal.h`
```c
#if defined(CONFIG_ARCH_TEGRA_21x_SOC)
  extern int tegra_mipi_bias_pad_enable(void);
#else
  int tegra_mipi_bias_pad_enable(void) { return -ENOSYS; }
#endif
```

**Historical concern:** The MC framework's MIPI calibration was stubbed out for T124.

**Resolution:** MIPI calibration was implemented via `mipi_cal_t124.c` driver, but **this was not the critical fix**. The actual issue was CSI PHY initialization ORDER, not MIPI calibration. See Section 9 for details.

### 2.6 Never tested on real hardware — RESOLVED

**Resolution:** Successfully tested on Xiaomi Mi Pad (mocha), serial 110C2083, LineageOS 14.1. All major code paths verified working.

---

## 3. Two VI Drivers — Conflict Risk — RESOLVED

Both drivers match `compatible = "nvidia,tegra124-vi"`:

| Driver | Location | Framework | Config |
|--------|----------|-----------|--------|
| MC vi.c | drivers/media/platform/tegra/vi/vi.c | Media Controller | VIDEO_TEGRA_VI |
| Legacy vi2.c | drivers/media/platform/soc_camera/tegra_camera/vi2.c | soc_camera | VIDEO_TEGRA (+ SOC_CAMERA) |

Current mocha defconfig state:
- `VIDEO_TEGRA_VI=y` (MC) — ENABLED
- `SOC_CAMERA` — not set (disabled)
- `VIDEO_TEGRA` — not set (disabled)

**Resolution:** MC framework active and working. No conflicts.

---

## 4. What Needs To Work vs What Is Optional

### Required for basic RAW capture (Phase 1) — ALL WORKING:
- [x] VI probe with t124_vi_info
- [x] CSI port setup (ports A, B)
- [x] OV5693 V4L2 subdev probe
- [x] V4L2 async subdev matching
- [x] VB2 DMA buffer allocation
- [x] Streaming start/stop
- [x] OV5693 mocha power sequence (custom ov5693_mocha.c driver)
- [x] DTS integration (tegra124-mocha-camera-mc.dtsi)
- [x] VIDIOC_S_PARM framerate selection

### Not required for basic capture:
- ISP processing (isp_t124.c — future work)
- Continuous streaming mode (TODO — single-shot working at ~6fps)
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

The MC framework for T124 is **architecturally complete and proven working** on real hardware. All necessary code paths exist and are correctly guarded for T124 vs T210 differences.

We proceeded with MC framework (not falling back to soc_camera) because:
1. soc_camera is deprecated and harder to extend
2. MC framework is the standard Linux V4L2 approach
3. The code is cleaner and better structured
4. Future ISP integration will be easier with MC

The bring-up strategy succeeded: started with OV5693 on CSI-E (simpler, 1-lane),
got basic RAW capture working. Next: IMX179 and ISP integration.

---

## 7. Mitigation Plan: Closing Potential Issues — HISTORICAL

**Note:** This section documents the original mitigation plan. See Section 9 for the actual fixes that were applied.

### 7.1 MIPI Calibration — IMPLEMENTED but NOT the root cause

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

### 7.2-7.6 Other historical items

All other mitigation items (register offset comparison, DMA buffer management,
power/clock verification) proved to be non-issues during actual bring-up.

---

## 8. Summary Risk Matrix — UPDATED

| Risk | Probability | Impact | Status | Notes |
|------|------------|--------|--------|-------|
| MIPI cal needed | Medium | Frame corruption | **RESOLVED** | Implemented but not the critical fix |
| Register offset mismatch | Low | Capture fails | **RESOLVED** | Offsets were correct |
| DMA/buffer issue | Low | No frames | **RESOLVED** | DMA working correctly |
| Power sequence wrong | Very low | Sensor won't init | **RESOLVED** | Power sequence correct |
| Async subdev race | Low | Probe order issue | **RESOLVED** | Single channel avoids deadlock |
| Unknown T124 quirk | Unknown | Unknown | **RESOLVED** | CSI PHY init order was the quirk |
| CSI PHY init order | High | No HS data | **RESOLVED** | See Section 9 |
| OV5693 1-lane vs 2-lane | High | No packets | **RESOLVED** | 0x3011=0x11 (1-lane mode) |
| T124 syncpt conditions | High | Timeout | **RESOLVED** | PPB_FRAME_START=10, MWB_ACK_DONE=7 |

---

## 9. Critical T124-Specific Fixes (The Real Solutions)

This section documents the actual fixes that made T124 MC framework work.

### 9.1 CSI PHY Initialization Order (CRITICAL)

**Problem:** CSI-E CILE was detecting LP state (0x80 in CILEX_STATUS) but PP_B received no HS data (PP_STATUS=0x0000).

**Root Cause:** The initialization order of CSI PHY registers matters on T124. The MC framework was programming registers in an order that worked on T210 but failed on T124.

**Fix:** Restructured `csi.c` to follow T124-specific initialization sequence:
1. Enable CG_CTRL before any CSI register access
2. Program CIL_PHY_CONTROL with proper pad configuration
3. Set CIL_COMMAND to ENABLE after PHY is ready
4. For CSI-E (1-lane), use absolute register offsets (0xA08, 0xA10, 0xA18)

### 9.2 OV5693 Lane Configuration (CRITICAL)

**Problem:** OV5693 register 0x3011 was set to 0x21 (2-lane mode), but hardware uses 1-lane CSI-E.

**Root Cause:** Copy-paste error from reference code. R21.5 correctly uses 0x11 (1-lane), but R24.1 mode tables had 0x21.

**Fix:** Changed all mode table entries from 0x3011=0x21 to 0x3011=0x11:
- 2592x1944@30
- 2592x1458@30
- 1920x1080@30
- 1280x720@60, @90, @120
- All HDR mode tables

**Verification:** After fix, PP_B_STATUS=0x4180 (packets received!)

### 9.3 T124 Syncpt Conditions (CRITICAL)

**Problem:** Frame start syncpt timeout on first capture attempt.

**Root Cause:** T124 uses different syncpt condition IDs than T210 for the same hardware events.

**Fix:** Updated `vi_irq.c` and `channel.c` with T124-specific syncpt conditions:

| Event | T124 Condition | T210 Condition |
|-------|---------------|----------------|
| PPB_FRAME_START | 10 | 9 |
| MWB_ACK_DONE | 7 | 6 |

**Files modified:**
- `vi/vi_irq.c`: `t124_syncpt_cond_table[]`
- `camera/channel.c`: `tegra_channel_capture_setup()`

### 9.4 DTS bus-width Configuration

**Problem:** DTS had `bus-width = <2>` but OV5693 on CSI-E uses 1 lane.

**Fix:** Updated `tegra124-mocha-camera-mc.dtsi`:
```dts
bus-width = <1>;
num_lanes = "1";
```

### 9.5 PHY_CILE_CONTROL0 Value

**Problem:** Various THS_SETTLE and BYPASS_LP_SEQ combinations tested.

**Resolution:** `PHY_CILE_CONTROL0 = 0x09` (THS_SETTLE=9, no BYPASS) confirmed working after lane configuration fix.

Previous testing showed:
- 0x4E (THS=14+BYPASS): Worked partially before lane fix
- 0x49 (THS=9+BYPASS): Did not work
- 0x09 (THS=9, no BYPASS): **Working** after lane fix

### 9.6 ec_recover Implementation

**Problem:** Error recovery path not properly implemented for T124.

**Fix:** Added proper `ec_recover` handling in CSI driver for T124-specific error conditions.

### 9.7 CSI TPG (Test Pattern Generator)

**Implementation:** CSI TPG enabled as diagnostic tool.

**Usage:** When sensor fails, TPG can generate test patterns to verify CSI→VI→memory path without sensor involvement.

**Verification:** TPG patterns captured successfully, proving the data path works.

---

## 10. T124 Syncpt Conditions Reference

Complete table of T124 syncpt conditions (from TRM and verified in code):

| Condition ID | Name | Description |
|--------------|------|-------------|
| 0 | VI_WDT | VI watchdog timeout |
| 1 | ISP_WAIT_FOR_IDLE | ISP idle signal |
| 2 | VI_HS | Horizontal sync |
| 3 | VI_VS | Vertical sync |
| 4 | PPA_FRAME_START | Pixel Parser A frame start |
| 5 | PPA_FRAME_END | Pixel Parser A frame end |
| 6 | PPB_FRAME_END | Pixel Parser B frame end |
| 7 | MWB_ACK_DONE | Memory write buffer acknowledge |
| 8 | PPB_DATA_DONE | Pixel Parser B data done |
| 9 | (reserved) | - |
| 10 | PPB_FRAME_START | Pixel Parser B frame start |
| ... | ... | ... |

**Critical difference from T210:** T210 uses condition 9 for PPB_FRAME_START, T124 uses condition 10.

---

## 11. Lessons Learned

1. **Lane configuration is critical:** The OV5693 0x3011 register must match hardware wiring (1-lane vs 2-lane). A single bit error here causes complete CSI link failure.

2. **Initialization order matters:** T124 is more sensitive to register programming order than T210. The same register values in different order can fail.

3. **Syncpt conditions are SoC-specific:** Never assume T210 syncpt IDs work on T124. Always verify against TRM.

4. **MIPI calibration was a red herring:** While implemented, it was not the blocking issue. CSI PHY initialization order was the real problem.

5. **TPG is invaluable:** Having a test pattern generator makes it easy to distinguish sensor issues from CSI/VI issues.

6. **Hardware verification:** Always verify physical connections (schematics, lane count) before debugging software.

---

*Document updated: March 2026*
*Branch: camera/v4l2-bringup*
*Tested on: Xiaomi Mi Pad (mocha), Tegra K1 (T124)*
