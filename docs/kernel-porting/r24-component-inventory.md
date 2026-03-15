# R24.1 Kernel — Complete NVIDIA Component Inventory

## Overview

The R24.1 kernel contains approximately **1,893 NVIDIA-specific files** across multiple subsystems. This document provides a complete breakdown of the NVIDIA driver components present in this BSP.

> **Note**: This inventory was audited via exhaustive `find` + `grep` across the entire kernel tree (not just known paths). Last audit: 2026-03-15.

## Statistics Summary

| Category | File Count |
|----------|------------|
| Core drivers (drivers/video/tegra + drivers/gpu) | 434 |
| Platform drivers (drivers/platform/tegra) | 147 |
| Media drivers (drivers/media/platform/tegra) | 98 |
| Sound drivers (sound/soc/tegra*) | 150 |
| Architecture — arm (arch/arm/mach-tegra) | 167 |
| Architecture — arm64 (arch/arm64/mach-tegra) | 63 |
| Device tree — arm (arch/arm/boot/dts tegra*) | 256 |
| Device tree — arm64 (arch/arm64/boot/dts tegra*) | 318 |
| Scattered drivers (see section below) | ~120 |
| Headers (include/) | ~140 |
| Firmware/BPMP/virt | 25 |
| DT binding docs (Documentation/) | 93 |
| **Total** | **~1,893** |

## Critical for Mocha (Mi Pad 1) — Must Port

Not all files need porting. For the mocha device (T124, arm32), the following are **required**:

| Component | Files | Why Critical |
|-----------|-------|-------------|
| drivers/video/tegra/dc/ | ~100 | Display — no screen without it |
| drivers/video/tegra/host/ | ~60 | Host1x — 2D/VIC/GPU submission |
| drivers/video/tegra/nvmap/ | ~20 | Memory — all drivers depend on it |
| drivers/gpu/nvgpu/gk20a/ | ~100 | GPU — no GL without it |
| drivers/platform/tegra/ (core) | ~50 | DVFS, PMC, EMC, powergate, soctherm |
| arch/arm/mach-tegra/ | ~167 | SoC init, clocks, board setup |
| drivers/clk/tegra/ | 13 | Clock framework — boot dependency |
| drivers/pinctrl/pinctrl-tegra*.c | 7 | Pin muxing — boot dependency |
| drivers/gpio/gpio-tegra.c | 1 | GPIO — boot dependency |
| drivers/irqchip/irq-tegra.c | 1 | IRQ controller — boot dependency |
| drivers/iommu/tegra-smmu*.c | 4 | IOMMU — nvmap/GPU depend on it |
| drivers/video/backlight/tegra_*.c | 2 | Backlight — display visibility |
| sound/soc/tegra/ | ~70 | Audio |
| Device tree (tegra124-*) | ~50 | Mocha-specific DT |
| Headers (include/) | ~140 | All drivers include these |

**Not needed for mocha**: arm64/*, T210-specific drivers, BPMP (T210+), hypervisor, UFS, baseband, NAND.

---

## 1. CORE: drivers/video/tegra/ (227 files)

The core display and host1x driver stack.

### dc/ (~100 files)
- Display controller implementation
- ext/ subdirectory for userspace interface (tegra_dc_ext.h)
- Window management, color correction, overlay support
- HDMI, DSI, eDP output drivers

### host/ (~60 files)
Host1x command submission infrastructure:
- **flcn/** — Falcon microprocessor support
- **host1x/** — Core host1x driver
- **isp/** — Image signal processor
- **nvdec/** — Video decoder
- **t124/** — Tegra K1 specific code
- **t210/** — Tegra X1 specific code
- **tsec/** — Security processor
- **vhost/** — Virtualization host support

### nvmap/ (~20 files)
- NVIDIA memory allocator
- Userspace API for buffer management
- Cache management operations

### camera/
- Camera platform support
- ISP integration

### virt/
- Virtualization support for display

---

## 2. GPU: drivers/gpu/nvgpu/ (163 files)

NVIDIA GPU driver stack for Tegra SoCs.

### gk20a/
- GK20A GPU driver (Tegra K1 Kepler GPU)
- Graphics, compute, and video acceleration
- Power management (rail gating, clock gating)

### gm20b/
- GM20B GPU driver (Tegra X1 Maxwell GPU)
- Shares infrastructure with gk20a

### vgpu/
- Virtual GPU support
- Para-virtualized GPU access

---

## 3. HOST1X DRM: drivers/gpu/host1x/

DRM-based host1x support:
- Hardware abstraction layer (hw/)
- Syncpoint management
- Channel allocation
- Integration with DMA-BUF

---

## 4. PLATFORM: drivers/platform/tegra/ (147 files)

Platform-level power, thermal, and system management.

### mc/
- Memory controller driver
- Bandwidth management
- Error handling

### nvadsp/
- Audio DSP driver
- ADSP firmware loading
- Communication interface

### nvdumper/
- Crash dump collection
- RAM preservation across reboot

### powergate/
- Power gating infrastructure
- genpd integration

### Core Platform Files
- dvfs.c — Dynamic voltage and frequency scaling
- tegra_cl_dvfs.c — Closed-loop DVFS
- soctherm.c — SoC thermal management
- emc.c — External memory controller
- pmc.c — Power management controller
- edp.c — Electrical design point management

---

## 5. MEDIA: drivers/media/platform/tegra/ (98 files)

Camera and video capture subsystem.

### Subdirectories
- **auto/** — Auto-focus and auto-exposure
- **cam_dev/** — Camera device interface
- **camera/** — Core camera driver
- **csi/** — CSI interface driver
- **mipical/** — MIPI calibration
- **nvavp/** — Audio/video processor
- **tpg/** — Test pattern generator
- **vi/** — Video input (capture)

### Camera Sensor Drivers
| Sensor | Resolution | Interface |
|--------|------------|-----------|
| imx132 | 2MP | MIPI CSI |
| imx135 | 13MP | MIPI CSI |
| imx179 | 8MP | MIPI CSI |
| imx214 | 13MP | MIPI CSI |
| imx219 | 8MP | MIPI CSI |
| imx091 | 13MP | MIPI CSI |
| ov5640 | 5MP | MIPI CSI/DVP |
| ov5650 | 5MP | MIPI CSI |
| ov5693 | 5MP | MIPI CSI |
| ov7695 | VGA | MIPI CSI |
| ov9772 | 720p | MIPI CSI |
| ov10823 | 10MP | MIPI CSI |
| ov14810 | 14MP | MIPI CSI |
| ov2710 | 2MP | MIPI CSI |
| ov9726 | 1MP | MIPI CSI |
| ar0261 | 2MP | MIPI CSI |
| ar0832 | 8MP | MIPI CSI |
| ar0833 | 8MP | MIPI CSI |
| mt9m114 | 1.3MP | MIPI CSI |
| soc380 | VGA | MIPI CSI |

---

## 6. SOUND: sound/soc/ (141 files)

Audio subsystem with three parallel implementations.

### tegra/
Standard Tegra audio stack:
- I2S controller
- SPDIF output
- DMIC (digital microphone)
- DAM (digital audio mixer)
- AHUB (audio hub)

### tegra-alt/
Alternative audio stack:
- ADMA (audio DMA)
- ADSP integration
- APM (audio processing manager)

### tegra-virt-alt/
Virtualized audio support for hypervisor environments.

### Machine Drivers
Audio codec integration for various boards:
- WM8903 (NVIDIA reference boards)
- WM8753
- RT5640 / RT5639 / RT5671 (Realtek)
- MAX98088 / MAX98090 (Maxim)

---

## 7. ARCHITECTURE: arch/arm/mach-tegra/ (167 files)

SoC-specific board support and platform initialization.

### Board Support
- Ardbeg (Jetson TK1 reference)
- Loki (Shield Tablet)
- Laguna
- Norrin
- TN8
- And many others

### Panel Drivers (15+)
LCD and touch panel drivers for various displays.

### Core Subsystems
- Power management
- Memory configuration
- CPU idle (LP2)
- Thermal management
- EDP (electrical design point)

---

## 8. DEVICE TREE: arch/arm/boot/dts/ (256 tegra files)

Device tree source files for all supported boards.

### tegra124-* Files
- tegra124-jetson-tk1.dts — Jetson TK1
- tegra124-ardbeg.dts — Ardbeg reference
- tegra124-loki.dts — Loki/Shield Tablet
- tegra124-laguna.dts — Laguna board
- tegra124-norrin.dts — Norrin board
- tegra124-tn8.dts — TN8 board

### Platform Includes
- Power domains and rail definitions
- Thermal zone configurations
- Pinmux settings
- Clock bindings

---

## 9. SCATTERED DRIVERS

Drivers distributed across various kernel subsystems.

### Cryptography
- tegra-se.c — Security engine
- tegra-aes.c — AES acceleration

### DMA
- tegra20-apb-dma.c — Tegra 2/3/4 APB DMA
- tegra210-adma.c — Tegra X1 ADMA

### I2C
- i2c-tegra.c — Main I2C driver
- 3 platform variants

### SPI
- spi-tegra114.c — Main SPI driver
- 5 platform variants

### MMC/SD
- sdhci-tegra.c — SDHCI controller

### PCI
- pci-tegra.c — PCI Express root complex

### USB
- ehci-tegra.c — USB 2.0 host
- xhci-tegra.c — USB 3.0 host
- phy-tegra-usb.c — USB PHY
- tegra-otg.c — USB OTG
- tegra_udc.c — USB device controller

### Network
- tegra_hv_net.c — Hypervisor virtual network
- bcmdhd / bcmdhd_88 — Broadcom WiFi

### Input
- tegra-kbc.c — Keyboard controller
- Touch drivers: Raydium, Maxim, Synaptics

### Watchdog
- tegra_wdt.c — Watchdog timer
- tegra_hv_wdt.c — Hypervisor watchdog

### RTC
- rtc-tegra.c — Real-time clock

### PWM
- pwm-tegra.c — Pulse width modulation

### Clocksource
- tegra-nvtimers.c — Legacy timers
- tegra-tsc-timer.c — Timestamp counter
- tegra-wakeup-nvtimers.c — Wakeup timers
- tegra210_timer.c — T210 specific timers

### IOMMU
- tegra-smmu.c — System MMU (Tegra 4/X1)
- tegra-gart.c — GART (Tegra 2/3)
- of_tegra-smmu.c — Device tree SMMU
- hv_tegra-smmu.c — Hypervisor SMMU

### Mailbox
- tegra-xusb-mailbox.c — XUSB firmware mailbox

### Memory Controller
- tegra20-mc.c — Tegra 2 MC
- tegra30-mc.c — Tegra 3 MC

### AMBA
- tegra-ahb.c — AHB bus configuration

### Serial
- serial-tegra.c — UART driver
- tegra_hv_comm.c — Hypervisor console

### Misc
- tegra-profiler/ (43 files) — Performance profiling
- tegra-fuse — Fuse programming
- tegra-cec — HDMI CEC
- tegra-baseband — Modem interface
- tegra-cryptodev — Crypto device node
- tegra-throughput — Bandwidth monitoring
- tegra_cpc — Core power controller
- tegra_ppm — Power profiling module

### Staging
- nvshm/ (24 files) — NVIDIA shared memory
- ion/tegra/tegra_ion.c — ION allocator

### ATA
- ahci-tegra.c — SATA AHCI
- sata_aux_tegra.c — SATA auxiliary

### Regulator
- tegra-dfll-bypass-regulator.c — DFLL bypass

### Thermal
- tegra_aotag.c — Always-on thermal alarm

### Virtualization
- tegra_hv.c — Hypervisor core driver

### Previously Undocumented (found in audit)
- drivers/firmware/tegra/ (9 files) — BPMP driver (T210+, not needed for mocha)
- drivers/virt/tegra/ (8 files) — Hypervisor support (hv_sys_test, ivcbench, hyp_syscall)
- drivers/video/backlight/tegra_pwm_bl.c, tegra_dsi_bl.c — **Display backlight (needed for mocha)**
- drivers/clk/tegra/ (13 files) — **Clock framework (boot critical)**
- drivers/pinctrl/pinctrl-tegra*.c (7 files) — **Pin control (boot critical)**
- drivers/gpio/gpio-tegra.c — **GPIO driver (boot critical)**
- drivers/irqchip/irq-tegra.c — **IRQ controller (boot critical)**
- drivers/cpuidle/cpuidle-tegra210.c — T210 cpuidle (not needed for mocha)
- drivers/padctrl/padctrl-tegra210-pmc.c — T210 pad control (not needed for mocha)
- drivers/block/tegra_hv_vblk.c — Hypervisor virtual block
- drivers/scsi/ufs/ufs-tegra.c — UFS storage (not on mocha)
- drivers/mtd/devices/tegra_nand.c — NAND flash
- drivers/mipi_bif/mipi-bif-tegra.c — MIPI BIF
- drivers/w1/masters/tegra_w1.c — 1-Wire
- drivers/char/tegra-efshlp.c, tegra_pflash.c — EFS/pflash helpers
- drivers/misc/mods/mods_tegradc.c — MODS display
- net/sched/sch_tegra.c — Tegra scheduler
- arch/arm64/mach-tegra/ (63 files) — 64-bit support (not needed for mocha arm32)
- arch/arm64/boot/dts/ tegra* (318 files) — 64-bit DT (not needed for mocha)
- Documentation/devicetree/bindings/ tegra* (93 files) — DT binding docs

---

## 10. HEADERS: include/ (~140 files)

NVIDIA-specific header files.

### Core Headers (include/linux/)
- tegra-soc.h, tegra-pm.h, tegra-pmc.h, tegra-powergate.h, tegra-fuse.h
- tegra-cpuidle.h, tegra-ahb.h, tegra-ivc.h, tegra-timer.h
- tegra_audio.h, tegra_avp_audio.h, tegra_nvadsp.h, tegra_nvavp.h
- tegra_gr_comm.h, tegra_dsi_backlight.h, tegra_ion.h, tegra_pm_domains.h
- tegra_vgpu.h, tegra_vhost.h
- tegra_throttle.h, tegra_soctherm.h, tegra_ppm.h, tegra_cpc.h
- tegra_cluster_control.h
- nvhost.h, nvhost_ioctl.h, nvhost_isp_ioctl.h, nvhost_nvdec_ioctl.h, nvhost_vi_ioctl.h
- nvmap.h, gk20a.h, clk/tegra.h
- i2c-tegra.h, i2c-tegra-hv.h
- pci-tegra.h, rtc-tegra.h, mipi-bif-tegra.h

### Platform Data (include/linux/platform/tegra/) — 19 files
- clock.h, common.h, cpu-tegra.h, dvfs.h, flowctrl.h, io-dpd.h
- isomgr.h, latency_allowance.h, mc.h, mcerr.h, mc-regs-t12x.h, mc-regs-t21x.h
- reset.h, tegra_cl_dvfs.h, tegra_emc.h, tegra_mc.h, tegra12_emc.h, tegra21_emc.h

### Platform Data (include/linux/platform_data/) — 13 files
- gpio-tegra.h, serial-tegra.h, mmc-sdhci-tegra.h, tegra_ahci.h
- tegra_bpc_mgmt.h, tegra_edp.h, tegra_emc_pdata.h, tegra_nor.h
- tegra_usb.h, tegra_usb_modem_power.h, tegra_wakeup_monitor.h, nvshm.h

### SoC/DT/Video/Sound/Media/Trace Headers
- include/soc/tegra/ (4 files): chip-id.h, tegra_bpmp.h, tegra_pasr.h, xusb.h
- include/dt-bindings/ (17 files): clk, display, gpio, media, memory, padctrl, pinctrl, soc, sound, usb, ata
- include/video/ (3 files): tegra_camera.h, tegra_dc_ext.h, tegrafb.h
- include/media/ (3 files): tegra_camera_platform.h, tegra_dtv.h, tegra_v4l2_camera.h
- include/sound/ (4 files): tegra_nvfx.h, tegra_nvfx_apm.h, tegra_nvfx_plugin.h, tegra_wm8903.h
- include/trace/events/ (8 files): nvhost.h, nvhost_podgov.h, nvmap.h, nvavp.h, nvpower.h, nvsecurity.h, tegra_smmu.h, tegra_throughput.h

### Userspace API
- include/uapi/linux/nvgpu.h — GPU ioctls
- include/uapi/drm/tegra_drm.h — DRM (not used by our userspace)
- include/uapi/video/tegra_adf.h — ADF

---

## 11. NVIDIA PATCHES TO GENERIC KERNEL CODE

> **Found by cross-referencing 5 independent search methods** (filename, content grep, Kconfig/Makefile, #include/symbols, copyright). These are modifications NVIDIA made to upstream kernel files — they don't have "tegra" in their filename and are easy to miss.

### Core Kernel Patches (35 files)

**Memory management:**
- `mm/page_alloc.c` — Tegra-specific OOM condition when ZRAM disabled (`CONFIG_ARCH_TEGRA`)
- `mm/mmap.c`, `mm/mprotect.c` — `#include <linux/tegra_profiler.h>`

**Scheduler:**
- `kernel/sched/core.c` — `nr_running_integral()` for Tegra profiler

**Panic/debug:**
- `kernel/panic.c` — `CONFIG_TEGRA_NVDUMPER` crash dump hooks
- `kernel/stop_machine.c` — `CONFIG_TEGRA_SERIALIZE_DISABLE_IRQ`
- `kernel/trace/tracedump.c`, `kernel/trace/tracelevel.c` — NVIDIA additions

**Proc/FS:**
- `fs/proc/meminfo.c` — Reports nvmap page pool and iovmm usage (`#include <linux/nvmap.h>`)
- `fs/pstore/` (6 files) — NVIDIA copyright, pstore extensions

**Networking:**
- `net/sched/sch_generic.c`, `sch_mq.c`, `sch_mqprio.c` — `CONFIG_NET_SCH_TEGRA` hooks
- `net/ipv4/tcp_output.c` — `CONFIG_NET_SCH_TEGRA` conditionals
- `net/ipv4/udp.c` — `CONFIG_BCMDHD_CUSTOM_SYSFS_TEGRA`

**Other:** `kernel/hrtimer.c`, `kernel/smp.c`, `kernel/irq_work.c`, `kernel/power/qos.c`, `kernel/time/alarmtimer.c`, `lib/genalloc.c`, `fs/eventpoll.c`, `fs/select.c`, `net/core/sock.c`, `net/rfkill/rfkill-gpio.c`, `net/socket.c`

### ARM Architecture Patches

- `arch/arm/kernel/smp.c` — Tegra CPU die handling
- `arch/arm/kernel/smp_scu.c` — `CONFIG_ARCH_TEGRA_14x_SOC`
- `arch/arm/kernel/suspend.c` — `#ifndef CONFIG_ARCH_TEGRA`
- `arch/arm/kernel/relocate_kernel.S` — `TEGRA_PMC_BASE` for kexec
- `arch/arm/mm/proc-v7.S` — `TEGRA_CLK_RESET_BOND_OUT` CPU identification
- `arch/arm/mm/cache-l2x0.c` — `CONFIG_TEGRA_USE_SECURE_KERNEL`

### drivers/edp/ — Electrical Design Point (8 files, NEW)

System-level power budgeting framework — **not documented before**:
- `sysedp.c`, `sysedp_batmon_calc.c`, `sysedp_debug.c`
- `sysedp_dynamic_capping.c` — GPU/CPU power capping with tegra_edp integration
- `sysedp_modem.c`, `sysedp_reactive_capping.c`, `sysedp_sysfs.c`, `sysedp_internal.h`

### drivers/cpuquiet/ — CPU Hotplug Governors (12 files, NEW)

NVIDIA's CPU core online/offline framework:
- `cpuquiet.c`, `cpuquiet.h`, `cpuquiet_attribute.c`, `cpuquiet-smp-hotplug.c`
- `driver.c`, `governor.c`, `sysfs.c`
- `governors/balanced.c`, `governors/runnable_threads.c`, `governors/userspace.c`
- `tegra/cpuquiet.c`, `tegra/Makefile`

### drivers/devfreq/ — Frequency Governors (4 files, NEW)

- `governor_pod_scaling.c` — POD (Performance On Demand) governor for GPU, uses `nvhost_podgov`, `tegra_throughput_get_hint()`
- `governor_wmark_active.c`, `governor_wmark_simple.c` — Watermark-based governors
- `devfreq.c` — NVIDIA patches to core devfreq

### PMIC/Power Ecosystem — Generic Drivers with NVIDIA Patches (116 files, NEW)

NVIDIA extensively patched PMIC, regulator, power supply, RTC, GPIO, and extcon drivers. These are **not tegra-named** but contain NVIDIA code.

**MFD (Multi-Function Device)** — 17 files:
- `as3722.c` — AMS AS3722 PMIC (**T124 primary PMIC**)
- `palmas.c` — TI Palmas PMIC
- `max77620-core.c`, `max77660-core.c`, `max77663-core.c`, `max77665.c` — Maxim PMICs
- `tps6591x.c`, `tps65090.c`, `tps80031.c`, `tps8003x-gpadc.c`
- `ricoh583.c`, `rc5t583.c`, `rc5t583-irq.c`
- `max8907.c`, `max8907c-irq.c`, `max8831.c`
- `aat2870-core.c`, `tlv320aic3xxx-spi.c`

**Regulators** — 19 files:
- `as3722-regulator.c`, `palmas-regulator.c`, `max77620-regulator.c`
- `max77660-regulator.c`, `max77663-regulator.c`, `max8907-regulator.c`, `max8973-regulator.c`
- `tps51632-regulator.c`, `tps61280-regulator.c`, `tps62360-regulator.c`, `tps6238x0-regulator.c`
- `tps65090-regulator.c`, `tps65132-regulator.c`, `tps6591x-regulator.c`, `tps80031-regulator.c`
- `ricoh583-regulator.c`, `rc5t583-regulator.c`, `max15569-regulator.c`, `max16989-regulator.c`
- `pinmux-fixed-regulator.c`, `pwm-regulator.c`, `aat2870-regulator.c`

**Power supply/charger** — 22 files:
- `bq2419x-charger.c`, `bq2419x-charger-st8.c`, `bq2471x-charger.c`, `bq2477x-charger.c`
- `bq27441_battery.c`, `bq27x00_battery.c`, `max17042_battery.c`, `max17048_battery.c`
- `lc709203f_battery.c`, `cw201x_battery.c` — contain `tegra_get_board_battery_id()`
- `smb349-charger.c`, `palmas-charger.c`, `palmas_battery.c`
- `max77660-charger-extcon.c`, `max77665-charger.c`, `max8907c-charger.c`
- `tps65090-charger.c`, `tps80031-charger.c`, `tps80031_battery_gauge.c`
- `battery-charger-gauge-comm.c`, `power_supply_extcon.c`, `sbs-battery.c`
- `reset/as3722-poweroff.c`, `reset/max77620-poweroff.c`, `reset/palmas-poweroff.c`, `reset/system-pmic.c`

**RTC** — 14 files:
- `rtc-as3722.c`, `rtc-palmas.c`, `rtc-max77620.c`, `rtc-max77660.c`, `rtc-max77663.c`
- `rtc-max8907.c`, `rtc-max8907c.c`, `rtc-tps6586x.c`, `rtc-tps65910.c`, `rtc-tps6591x.c`
- `rtc-tps80031.c`, `rtc-ricoh583.c`, `rtc-rc5t583.c`, `hctosys.c`

**GPIO** — 9 files:
- `gpio-palmas.c`, `gpio-max77620.c`, `gpio-max77660.c`, `gpio-max77663.c`
- `gpio-tps6586x.c`, `gpio-rc5t583.c`, `gpio-tmpm32x.c`, `gpio-pca953x.c`

**Watchdog** — 5 files: `as3722_wdt.c`, `max77620_wdt.c`, `max77660_sys_wdt.c`, `palmas_wdt.c`, `softdog_platform.c`

**Extcon** — 5 files: `extcon-palmas.c`, `extcon-max77665.c`, `extcon-gpio-otg.c`, `extcon-gpio-states.c`, `extcon-cable-xlate.c`

**LEDs** — 3 files: `leds-cy8c.c`, `leds-max77660.c`, `leds-max8831.c`

**Hwmon** — 4 files: `ina219.c`, `ina230.c`, `ina3221.c`, `tmon-tmp411.c`

**Pinctrl core patches** — 8 files:
- `core.c`, `devicetree.c`, `devicetree.h`, `pinmux.c`, `pinctrl-utils.c`, `pinctrl-utils.h`, `pinctrl-consumer.c`
- PMIC pin controllers: `pinctrl-as3722.c`, `pinctrl-max77620.c`, `pinctrl-max77660.c`, `pinctrl-palmas.c`

### Thermal — Non-tegra-named (11 files, NEW)

- `adaptive_skin.c` — Adaptive skin temperature governor
- `pid_thermal_gov.c` — PID thermal governor
- `pwm_fan.c` — PWM fan control
- `thermal_debugfs.c` — Thermal debug
- PMIC thermals: `as3722_thermal.c`, `max77620-thermal.c`, `palmas_thermal.c`
- `generic_adc_thermal.c`, `of_generic_adc_thermal.c`, `modem_thermal.c`, `tmp006.c`

### HID / Input / Haptics (36 files, NEW)

**HID (Shield controllers):**
- `hid-nvidia.c`, `hid-nvidia-blake.c` — Shield wireless joystick
- `hid-atv-jarvis.c` — Shield TV remote
- `hid-steam.c`, `hid-steam.h` — Steam controller (NVIDIA copyright)
- `hid-core.c`, `hid-input.c`, `hidraw.c`, `usbhid/hid-core.c` — NVIDIA device ID patches

**Touch:**
- `rm31080a_ts.c`, `rm31080a_ctrl.c` — Raydium touch
- `maxim_sti.c` — Maxim touch
- `nvtouch/nvtouch_kernel.c`, `nvtouch.h`, `nvtouch_kernel.h` — NVIDIA touch framework
- `lr388k7_ts.c`, `panjit_i2c.c`, `rmi4/rmi_f11.c`, `rmi4/rmi_spi.c`

**Haptics:** `drv2603-vibrator.c`, `max77660_haptic.c`, `max77665_haptic.c`, `tspdrv/ImmVibeSPI.c` (x2)

**Input misc:** `input-cfboost.c` (CPU frequency boost on input), `gpio_keys.c`, `gpio_timed_keys.c`, `alps_gpio_scrollwheel.c`
- `mpu/inv_gyro.c`, `inv_gyro.h`, `inv_gyro_misc.c`, `inv_mpu3050.c` — InvenSense IMU
- `pressure/bmp180.c`, `compass/ak8975_input.c`

### IIO / Sensors (52 files, NEW)

**NVIDIA Sensor Framework (NVS):**
- `iio/common/nvs/` — 8 files: `nvs_auto.c`, `nvs_dsm.c`, `nvs_iio.c`, `nvs_light.c`, `nvs_of_dt.c`, `nvs_proximity.c`, `nvs_timestamp.c`, `nvs_vreg.c`

**Sensors:**
- Accelerometer: `nvs_ais328dq.c`
- Gyro: `nvs_a3g4250d.c`
- IMU: `nvi_mpu/` (8 files) — NVIDIA InvenSense wrapper
- Light: `nvs_bh1730fvc.c`, `nvs_cm3217.c`, `nvs_cm3218.c`, `nvs_isl2902x.c`, `nvs_jsa1127.c`, `nvs_ltr659.c`, `nvs_max4400x.c`
- Magnetometer: `ak8975.c`, `nvi_ak89xx.c`
- Pressure: `nvi_bmpX80.c`
- Proximity: `nvs_iqs2x3.c`

**Staging IIO:** `cm3217.c`, `cm3218.c`, `iqs253.c`, `isl29018.c`, `isl29028.c`, `jsa1127.c`, `ls_dt.c`, `ls_sysfs.c`, `ltr558als.c`, `max44005.c`, `stm8t143.c`, `tcs3772.c`, `bmp180.c`, `ina219.c`, `ina230.c`, `ina3221.c`, `ads1015.c`, `as3722-adc-extcon.c`, `max77660-adc.c`, `palmas_gpadc.c`

### Wireless bcmdhd Tegra Extensions (59 files)

Both `bcmdhd/` and `bcmdhd_88/` contain extensive tegra sysfs hooks:
- `dhd_custom_sysfs_tegra.c/.h` — Main sysfs interface
- `dhd_custom_net_bw_est_tegra.c/.h` — Bandwidth estimation
- `dhd_custom_net_diag_tegra.c/.h` — Network diagnostics
- `dhd_custom_net_perf_tegra.c/.h` — Performance monitoring
- `dhd_custom_sysfs_tegra_ping.c` — Ping statistics
- `dhd_custom_sysfs_tegra_rssi.c` — RSSI monitoring
- `dhd_custom_sysfs_tegra_scan.c/.h` — Scan management
- `dhd_custom_sysfs_tegra_stat.c/.h` — Statistics
- `dhd_custom_sysfs_tegra_tcpdump.c` — TCP dump
- `dhd_custom_sysfs_tegra_rf_test.c` — RF testing
- Plus tegra hooks in: `dhd_linux.c`, `dhd_sdio.c`, `dhd_pcie.c`, `wl_cfg80211.c`, `wl_android.c`

### Camera I2C Sensors (32 files)

Sensor drivers in `drivers/media/i2c/` with tegra platform_data:
- `imx219.c`, `lc898212.c`, `ov23850.c`
- `soc_camera/imx230_v4l2.c`, `ov13860_v4l2.c`, `ov5693_v4l2.c`

Plus `drivers/media/platform/soc_camera/tegra_camera/` (6 files): `common.c`, `common.h`, `vi2.c`, `vi_bypass.c`, `Kconfig`, `Makefile`

### Misc — Non-tegra-named (23 files, NEW)

- `nct1008.c` — Temperature sensor (**critical for mocha**)
- `therm_est.c`, `therm_fan_est.c` — Thermal estimation
- `bcm4329_rfkill.c` — Bluetooth rfkill
- `bluedroid_pm.c` — Bluetooth power management
- `gps_wake.c` — GPS wakeup
- `nv_gamepad_reset.c` — Shield gamepad reset
- `cpuload.c` — CPU load monitor
- `force_idle_t132.c`, `idle_test_t132.c` — Denver idle tests
- `max1749.c` — Motor driver
- `palmas-sim.c`, `palmas-ldousb-in.c`, `max77660-sim.c` — PMIC SIM
- `tfa9887.c` — NXP audio amplifier
- `saf775x/` (3 files) — Audio DSP
- `c2port/c2port-loki.c`, `c2port/core.c` — Loki C2 port
- `issp/` (3 files) — In-System Serial Programming

### Other Patched Generic Drivers

- `drivers/iommu/arm-smmu.c` — Tegra SMMU config functions, includes `tegra-smmu.h`
- `drivers/irqchip/irq-gic.c` — Tegra AGIC (Audio GIC) support functions
- `drivers/pci/pci-driver.c` — `tegra_smmu_device_pci_nb` notifier
- `drivers/bluetooth/bluesleep.c` — `tegra_uart_request_clock_on/off()`
- `drivers/tty/serial/8250/8250_core.c` — `serial8250_tegra_handle_irq()`
- `drivers/tty/serial/of_serial.c` — `nvidia,tegra20-uart` compatible
- `drivers/usb/host/ehci-hcd.c`, `ehci-hub.c` — Tegra EHCI integration
- `drivers/usb/host/xhci.c` — Tegra XHCI integration
- `drivers/usb/gadget/nvxxx_udc.c`, `nvxxx.h` — Tegra XUDC driver
- `drivers/video/backlight/lp855x_bl.c`, `pwm_bl.c` — Tegra backlight hooks
- `sound/pci/hda/hda_intel.c` — Tegra HDA power domain, nvmap DMA allocation
- `sound/soc/codecs/max97236.c`, `audience/es-d300.c` — Tegra board info hooks
- `drivers/of/plugin-manager.c` — NVIDIA device tree plugin manager
- `drivers/cpufreq/cpufreq_interactive.c`, `cpufreq_smartmax.c` — Tegra input boost
- `drivers/cpuidle/cpuidle-denver.c`, `cpuidle.c` — Denver CPU idle
- `drivers/dma/dmaengine.c`, `of-dma.c` — NVIDIA patches
- `drivers/staging/android/lowmemorykiller.c` — nvmap page pool integration
- `drivers/mmc/core/` (8 files) — Tegra eMMC trace, debug extensions
- `drivers/usb/gadget/` (18 files) — NVIDIA USB gadget (nvusb, f_nvusb, android.c patches)
- `security/tlk_driver/` (8 files) — Trusted Little Kernel driver

---

## Summary

**Total NVIDIA/Tegra-specific files: ~2,954** (high-confidence, found by 2+ independent search methods)

> Cross-referenced by 5 agents using: filename patterns (2,147 hits), content grep (3,422), Kconfig/Makefile tracing (1,824), #include/symbol analysis (1,132), and NVIDIA copyright search (2,013). Union: 3,960. Files found by all 5: 424. High-confidence (2+): 2,954.

For mocha (Mi Pad 1, T124, arm32), the porting scope after excluding arm64, T210-only, hypervisor, and irrelevant peripherals is approximately **1,100-1,300 files**, including:
- ~800 "obvious" tegra-named drivers
- ~300-350 NVIDIA patches to generic kernel code (PMIC, thermal, sensors, core kernel, WiFi, USB)

### NVMAP_IOC_CLAIM Status

`NVMAP_IOC_CLAIM` (ioctl #1) is **NOT used** by any blob or source code. The R24.1 kernel doesn't even have a handler for it (returns -ENOTTY). Safe to drop in any ported kernel.

This is the last NVIDIA BSP to support T124 (Tegra K1) and represents the most complete proprietary driver stack available for this SoC.
