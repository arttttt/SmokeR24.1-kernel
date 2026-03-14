# R24.1 Kernel — Complete NVIDIA Component Inventory

## Overview

The R24.1 kernel contains approximately **1,480 NVIDIA-specific files** across multiple subsystems. This document provides a complete breakdown of the NVIDIA driver components present in this BSP.

## Statistics Summary

| Category | File Count |
|----------|------------|
| Core driver files (drivers/video/tegra + drivers/gpu) | ~390 |
| Platform drivers | 147 files |
| Media drivers | 98 files |
| Sound drivers | 141 files |
| Architecture (mach-tegra) | 167 files |
| Device tree | 256 files |
| Headers | ~100 files |
| Misc/scattered | ~180 files |
| **Total** | **~1,480** |

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

---

## 10. HEADERS: include/

NVIDIA-specific header files.

### Core Headers
- linux/tegra_*.h (50+ files)
- linux/platform/tegra/ — Platform data structures
- dt-bindings/ — Device tree bindings
- soc/tegra/ — SoC-specific definitions

### Userspace API
- uapi/ — Userspace-visible structures
- video/ — Framebuffer extensions, tegra_dc_ext.h
- sound/ — Audio IOCTLs

---

## Summary

The R24.1 kernel represents a complete BSP with:
- Full display stack (DC + Host1x + DRM)
- GPU acceleration (GK20A/GM20B)
- Camera and media capture
- Audio with DSP support
- Comprehensive power management
- Virtualization support
- Production-quality drivers for all major subsystems

This is the last NVIDIA BSP to support T124 (Tegra K1) and represents the most complete proprietary driver stack available for this SoC.
