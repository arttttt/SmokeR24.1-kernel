# MIPI DSI Display Issues — Xiaomi MiPad (mocha)

Analysis of the WIP MIPI DSI implementation in u-boot for the
Sharp LQ079L1SX01 panel (1536x2048, dual-DSI ganged mode).

Reference: working kernel driver at `drivers/video/tegra/dc/dsi.c`
in the SmokeR24.1-kernel tree.

---

## Bug #1 — Reversed writel arguments (BLOCKER)

**File:** `drivers/video/tegra124/dsi_new.c:1006`

```c
writel(APB_MISC_GP_MIPI_PAD_CTRL_0, DSIB_MODE_DSI);
```

The u-boot `writel(value, address)` signature matches the kernel.
This line writes the register address (0x70000820) as a value to
memory location 0x2 (DSIB_MODE_DSI constant), instead of writing
the DSIB mode bit to the pad control register.

DSIB never gets configured as a DSI controller. Since the panel
requires ganged mode (DSIA + DSIB, 8 lanes total), this alone
prevents the display from working.

The author uses `writel` correctly elsewhere in the same file
(e.g. `dsi_write` wrapper at line 53), so this is a typo.

**Fix:**
```c
writel(DSIB_MODE_DSI, APB_MISC_GP_MIPI_PAD_CTRL_0);
```

**Kernel reference** (`dsi.c:2736-2738`):
```c
val = readl(IO_ADDRESS(APB_MISC_GP_MIPI_PAD_CTRL_0));
val |= DSIB_MODE_ENABLE;
writel(val, (IO_ADDRESS(APB_MISC_GP_MIPI_PAD_CTRL_0)));
```

Note: kernel does read-modify-write to preserve other bits in
the register. U-boot writes absolute value, which may also
clear other pad control bits.

---

## Bug #2 — Missing DSI power cycle before configuration (HIGH)

**File:** `drivers/video/tegra124/dsi_new.c`

The kernel performs a full DSI power-off/power-on cycle during
initialization:

1. Power OFF DSI (`DSI_POWER_CONTROL = 0`)
2. Wait 300us
3. Clear all DSI registers to 0
4. Run pad calibration
5. Power ON DSI (`DSI_POWER_CONTROL = 1`)
6. Wait 300us

**Kernel reference** (`dsi.c:2763-2798`):
```c
tegra_dsi_writel(dsi,
    DSI_POWER_CONTROL_LEG_DSI_ENABLE(TEGRA_DSI_DISABLE),
    DSI_POWER_CONTROL);
/* stabilization delay */
udelay(300);

/* clear all DSI registers */
_tegra_dc_dsi_init(dc, dsi);

/* pad calibration */
tegra_dsi_pad_calibration(dsi);

/* power on */
tegra_dsi_writel(dsi,
    DSI_POWER_CONTROL_LEG_DSI_ENABLE(TEGRA_DSI_ENABLE),
    DSI_POWER_CONTROL);
udelay(300);
```

The u-boot driver skips the power-off step and the register
clearing step entirely. It calibrates pads with whatever state
the DSI controller was left in by the previous bootloader stage.

---

## Bug #3 — Undefined variable `panel` (LOW)

**File:** `drivers/video/tegra124/panel.c:169,173`

```c
ret = mipi_dsi_dcs_set_display_on(&plat->link1);
if (ret < 0) {
    dev_err(panel->dev, "failed to set display on: %d\n", ret);
}
```

The variable `panel` is not declared in `sharp_init_sequence()`.
The function parameter is `struct udevice *dev`.

This compiles because u-boot's `dev_err` macro expands to
`printk(fmt, ...)` and the `dev` argument is never evaluated.
Not a functional issue — just dead code in the error path.

**Fix:**
```c
dev_err(dev, "failed to set display on: %d\n", ret);
```

---

## Bug #4 — Inverted error checks (MEDIUM)

**File:** `drivers/video/tegra124/panel.c:135,140,147,154`

```c
ret = mipi_dsi_dcs_exit_sleep_mode(&plat->link1);
if(ret > 0) {
    dev_err(dev, "failed to mipi_dsi_dcs_exit_sleep_mode: ...");
}
```

The u-boot `mipi_dsi_dcs_write` returns bytes written (positive)
on success or a negative error code on failure. The condition
`ret > 0` triggers the error message on **success** and stays
silent on actual errors.

**Fix:** Change `if(ret > 0)` to `if(ret < 0)` in all four
occurrences (lines 135, 140, 147, 154).

---

## Bug #5 — DSI_PAD_CONTROL_3 direct write instead of RMW (MEDIUM)

**File:** `drivers/video/tegra124/dsi_new.c:851-853`

```c
value = DSI_PAD_PREEMP_PD_CLK(0x3) | DSI_PAD_PREEMP_PU_CLK(0x3) |
    DSI_PAD_PREEMP_PD(0x03) | DSI_PAD_PREEMP_PU(0x3);
dsi_write(dsi, value, DSI_PAD_CONTROL_3);
```

The kernel reads the register first, then ORs in the preemphasis
values, preserving any other bits that may have been set:

```c
val = tegra_dsi_readl(dsi, DSI_PAD_CONTROL_3_VS1);
val |= (DSI_PAD_PREEMP_PD_CLK(0x3) | DSI_PAD_PREEMP_PU_CLK(0x3) |
       DSI_PAD_PREEMP_PD(0x3) | DSI_PAD_PREEMP_PU(0x3));
tegra_dsi_writel(dsi, val, DSI_PAD_CONTROL_3_VS1);
```

U-boot's direct write may clear configuration bits set by earlier
initialization steps.

---

## Bug #6 — No DSI register reset before calibration (MEDIUM)

**File:** `drivers/video/tegra124/dsi_new.c`

The kernel clears all DSI registers to zero before performing
pad calibration (`_tegra_dc_dsi_init` writes 0 to every register
in the `init_reg` table). This ensures a known clean state.

The u-boot driver starts calibration without clearing registers,
which means leftover state from the stock NVIDIA bootloader may
interfere with the calibration results.

---

## Verified non-issues

The following items were investigated and found to be correct:

- **Ganged mode size register** — u-boot writes `size << 16 | size`
  which produces the same result as the kernel's
  `low_width << 16 | high_width` for symmetric left/right mode
  where `low_width == high_width == h_active/2`.

- **DSI packet sequences** — the non-burst sync event packet
  sequence constants in u-boot (`MIPI_DSI_V_SYNC_START`,
  `MIPI_DSI_H_SYNC_START`, etc.) match the kernel values
  (`CMD_VS=0x01`, `CMD_HS=0x21`, `CMD_RGB=0x3E`).

- **MIPI D-PHY calibration values** — the Tegra124-specific
  calibration constants are identical between u-boot and kernel:
  - `HSCLKPDOS=0x1`, `HSCLKPUOS=0x2`
  - `NOISE_FLT=0xa`, `PRESCALE=0x2`, `CLKEN_OVR=0x1`
  - PAD_CONTROL_2: `SLEWUPADJ=0x7`, `SLEWDNADJ=0x7`,
    `LPUPADJ=0x1`, `LPDNADJ=0x1`

- **Regulator initialization** — `sharp_power_init()` enables all
  18 PMIC regulators because u-boot does not call
  `regulators_enable_boot_on()` elsewhere. This compensates for
  the missing general PMIC initialization. The kernel enables the
  same regulators from different subsystems (regulator framework
  auto-enables `always-on`/`boot-on` rails; individual drivers
  enable their own supplies). The voltages match the kernel DTS.

---

## Notes

- The DSI init command sequence in u-boot sends commands to both
  links, while the kernel sends only to link0. For ganged mode
  with symmetric left/right, this may or may not matter depending
  on how the panel processes DCS commands. Needs testing.

- The `MIPI_DSI_MODE_VIDEO_SYNC_PULSE` flag is not set in
  `panel.c:207`. The panel DTSI in the kernel specifies
  non-burst sync events mode (not sync pulses). The u-boot
  `tegra_dsi_configure` at line 633 checks for this flag and
  selects the appropriate packet sequence. This needs
  verification against the kernel's `dsi_pkt_seq` selection
  to confirm which mode is actually correct for this panel.
