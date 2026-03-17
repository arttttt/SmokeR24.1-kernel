# Tegra X1 (T210) Boot ROM and Boot Chain — Technical Reference

## Overview

This document covers the Tegra X1 (T210) boot architecture: what Boot ROM is,
how the boot chain works, what's open source, and what's modifiable.

Applies to all T210 devices: Google Pixel C (smaug/dragon), NVIDIA Shield TV
(2015/2017), Nintendo Switch (original), Jetson TX1.

---

## Boot ROM (IROM)

### What It Is

Boot ROM is the **very first code** executed when a Tegra X1 powers on. It is:

- **Mask ROM** — hardwired into the silicon die during chip fabrication
- **Immutable** — cannot be updated, patched, or modified by any means
- Executed by the **BPMP** (Boot and Power Management Processor), an ARM7TDMI
  co-processor, NOT by the main Cortex-A57 CPU
- Approximately **128-256 KB** of code

Because it's burned into silicon, any bugs in Boot ROM are **permanent** for
that chip revision. This is why Fusee Gelee (CVE-2018-6242) is unpatchable.

### What Boot ROM Does

1. Reads fuse configuration (chip ID, security mode, boot device)
2. Reads strap pins (GPIO) for boot mode selection
3. Checks **PMC_SCRATCH0 bit 1** — if set, enters RCM immediately
4. Checks **Recovery Mode Strap GPIO** — if asserted, enters RCM
5. Determines boot media (eMMC, SPI, USB) from fuses/straps
6. Initializes boot media controller (eMMC/SPI)
7. Reads **BCT** (Boot Configuration Table) from boot media into IRAM
8. Validates BCT hash/signature
9. If BCT contains SDRAM parameters, programs SDRAM controller
10. Reads bootloader (nvtboot) from boot media into IRAM/SDRAM
11. Validates bootloader hash/signature
12. If all valid: jumps to bootloader entry point
13. If any error: enters **RCM** (USB recovery mode)

### RCM (Recovery Mode) in Boot ROM

When entering RCM, Boot ROM:
- Enables **USB1 port in device mode**
- Presents as USB device **`0955:7721`** (NVIDIA Tegra X1 APX)
- Accepts commands via proprietary **Tegra RCM protocol**
- Allows downloading code into IRAM for execution on BPMP
- Screen remains **black** (no display initialization in Boot ROM)

RCM entry conditions (any one triggers it):
1. No valid BCT found (or hash/signature fails)
2. No valid bootloader found (or hash/signature fails)
3. Recovery Mode Strap GPIO asserted
4. PMC_SCRATCH0 bit 1 set

### Boot ROM Dump — Public Availability

The T210 Boot ROM was dumped by **Team ReSwitched** in October 2017 via
hardware glitching on a Nintendo Switch. The binary is the same on ALL T210
devices (Pixel C, Shield TV, Switch, Jetson TX1) — it's a mask ROM.

**Repository**: https://github.com/adeljck/Tegra-X1-Bootrom

Contents:
- `Tegra_X1_BootROM_Nintendo_T210.bin` — Boot ROM binary
- `TegraX1-q3k.idc` — IDA Pro database with reverse-engineered annotations
  by @q3k (function names, structures, comments)

The IDA database makes it possible to study the exact Boot ROM behavior:
how it checks PMC_SCRATCH0, how it validates BCT, how the USB RCM stack
works, and where the Fusee Gelee vulnerability is.

### Boot ROM Memory Map

From Fusee Gelee analysis and ReSwitched reverse engineering:

| Address | Size | Description |
|---------|------|-------------|
| `0x00100000` | ~128KB | IROM (Boot ROM code, read-only) |
| `0x40000000` | 256KB | IRAM (shared on-chip SRAM) |
| `0x40005000` | — | USB DMA buffer 1 |
| `0x40009000` | — | USB DMA buffer 2 |
| `0x40010000` | — | Boot ROM stack (grows downward) / payload area |
| `0x7000E400` | 4 | PMC_CNTRL register (offset 0x00) |
| `0x7000E450` | 4 | PMC_SCRATCH0 register (offset 0x50) |

The Fusee Gelee exploit overwrites the stack at `0x40010000` via the USB
DMA buffer overflow, redirecting execution to attacker-controlled code.

### Fusee Gelee Vulnerability (CVE-2018-6242)

The T210 Boot ROM's RCM USB stack has a buffer overflow:

> RCM forgets to limit the wLength field of the 8-byte Setup Packet in
> some USB control transfers. Standard Endpoint Request GET_STATUS (0x00)
> can be used to do arbitrary memcpy from malicious RCM command and smash
> the Boot ROM stack before signature checks and after Boot ROM sends UID.

Requirements:
- Device must be in RCM mode
- USB connection to host
- Host sends specially crafted USB control transfer

Effect:
- Arbitrary code execution in Boot ROM context (highest privilege)
- Signature verification can be patched out
- Full control of the device

This vulnerability affects ALL T210 chips ever produced. It cannot be fixed
because Boot ROM is mask ROM. NVIDIA addressed it in T214 (Mariko) by
fixing the bug in the Boot ROM revision.

Discoverers:
- Kate Temkin (Fusee Gelee disclosure)
- fail0verflow (ShofEL2 exploit)
- Team ReSwitched

---

## Complete T210 Boot Chain

```
Power On
    │
    ▼
┌─────────────────────────────────────────────────────┐
│  Stage 1: BOOT ROM (IROM)                           │
│  ─────────────────────────                          │
│  Processor: BPMP (ARM7TDMI, 32-bit)                 │
│  Memory:    Executes from internal ROM               │
│             Uses IRAM (256KB) for data/stack          │
│  Source:    CLOSED — mask ROM on silicon              │
│             (dumped binary available, see above)      │
│                                                      │
│  Actions:                                            │
│  • Read fuses and straps                             │
│  • Check PMC_SCRATCH0 → RCM?                         │
│  • Check Recovery Strap GPIO → RCM?                   │
│  • Initialize eMMC controller                         │
│  • Load BCT from eMMC → IRAM                          │
│  • Validate BCT, init SDRAM                           │
│  • Load nvtboot from eMMC → IRAM                      │
│  • Validate nvtboot signature                         │
│  • Jump to nvtboot                                    │
│  • (On error → enter RCM)                             │
└──────────────────────┬──────────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────┐
│  Stage 2: nvtboot (TegraBoot) — 1st stage bootloader │
│  ─────────────────────────────────                   │
│  Processor: BPMP (still ARM7TDMI)                    │
│  Memory:    IRAM (256KB), then SDRAM                  │
│  Source:    CLOSED — NVIDIA proprietary binary        │
│             (distributed in L4T / factory images)     │
│                                                      │
│  Actions:                                            │
│  • Full SDRAM initialization                          │
│  • Load ARM Trusted Firmware (ATF/TOS)                │
│  • Load coreboot/cboot into SDRAM                     │
│  • Configure power rails and clocks                   │
│  • Start CCPLEX (Cortex-A57 cores)                    │
│  • BPMP halts                                         │
└──────────────────────┬──────────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────┐
│  Stage 3: Coreboot (Pixel C) / CBoot (Jetson/Shield) │
│  ─────────────────────────────────                   │
│  Processor: CCPLEX (ARM Cortex-A57, 64-bit)          │
│  Memory:    SDRAM (3GB on Pixel C)                    │
│  Source:    OPEN — coreboot is fully open source      │
│                                                      │
│  Actions:                                            │
│  • SoC peripheral initialization                      │
│  • Display initialization                             │
│  • Load depthcharge payload                           │
│  • Transfer control to depthcharge                    │
└──────────────────────┬──────────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────┐
│  Stage 4: Depthcharge (Pixel C) / U-Boot (others)    │
│  ─────────────────────────────                       │
│  Processor: CCPLEX                                   │
│  Memory:    SDRAM                                     │
│  Source:    OPEN — depthcharge is open source          │
│                                                      │
│  Actions:                                            │
│  • Fastboot server                                    │
│  • Verified Boot (vboot)                              │
│  • Button handling (volume, power)                    │
│  • Boot mode selection (normal/recovery/bootloader)   │
│  • Load and boot Linux kernel                         │
└──────────────────────┬──────────────────────────────┘
                       ▼
                  Linux Kernel
```

### What's Open, What's Closed

| Component | Open Source? | Repository / Location |
|-----------|-------------|----------------------|
| Boot ROM | **NO** (dumped binary available) | [Tegra-X1-Bootrom](https://github.com/adeljck/Tegra-X1-Bootrom) |
| BCT | Configurable (binary format) | Generated by NVIDIA cbootimage tool |
| nvtboot | **NO** (proprietary binary blob) | Distributed in L4T / factory images |
| ARM Trusted Firmware | **YES** | [arm-trusted-firmware tegra](https://github.com/ARM-software/arm-trusted-firmware/tree/master/plat/nvidia/tegra) |
| Coreboot (Pixel C) | **YES** | [ttefke/coreboot-smaug](https://github.com/ttefke/coreboot-smaug) |
| Depthcharge (Pixel C) | **YES** | [coreboot/depthcharge](https://github.com/coreboot/depthcharge) |
| CBoot (Jetson/Shield) | Partial | Some source in L4T |
| U-Boot (Jetson) | **YES** | [OE4T/u-boot-tegra](https://github.com/OE4T/u-boot-tegra) |
| Linux Kernel | **YES** | mainline + vendor trees |

### Where Button Handling Can Be Added

To add a "Vol Up + Vol Down → enter RCM" feature:

| Stage | Can add button check? | Helps when bricked? |
|-------|----------------------|-------------------|
| Boot ROM | **NO** (mask ROM) | N/A |
| nvtboot | **NO** (closed source, signed) | Would help, but impossible |
| Coreboot | **YES** (open source) | Only if coreboot itself starts |
| Depthcharge | **YES** (open source) | Only if depthcharge starts |

If the device is bricked at the eMMC/BCT level (Boot ROM can't load nvtboot),
**no software modification can help** — only hardware methods (eMMC shorting,
ISP programming).

---

## BCT (Boot Configuration Table)

The BCT is a binary data structure stored in the first sectors of eMMC. It
tells Boot ROM:

- How to configure the eMMC controller (timing, bus width)
- SDRAM controller parameters (training, timing, voltage)
- Where the bootloader (nvtboot) is located on eMMC
- Bootloader load address in memory
- Bootloader entry point
- Cryptographic hash/signature of the bootloader

### BCT Tools

- **cbootimage** — NVIDIA open-source tool to create/modify BCT files
  - https://github.com/nicman23/cbootimage (Pixel C fork)
  - Part of L4T (Linux4Tegra) toolchain
- BCT is generated from a `.cfg` text file describing parameters
- Can be flashed to eMMC via RCM + tegrarcm, or via dd in ISP mode

---

## PMC (Power Management Controller) Registers

### PMC_SCRATCH0 (0x7000E450, offset 0x50)

This register persists across warm resets. Boot ROM reads it during early boot.

| Bit | Value | Mode | Set by |
|-----|-------|------|--------|
| 1 | `0x00000002` | **Forced Recovery (RCM)** | Any software before reboot |
| 30 | `0x40000000` | Bootloader (fastboot) | Bootloader/kernel |
| 31 | `0x80000000` | Android Recovery | Bootloader/kernel |

Source: [Kernel patch — tegra: Support reboot modes](https://patchwork.kernel.org/project/linux-arm-kernel/patch/1399388651-12819-1-git-send-email-thierry.reding@gmail.com/)

### PMC_CNTRL (0x7000E400, offset 0x00)

Writing `0x10` triggers a system-wide reset. Combined with PMC_SCRATCH0,
this is the software method to enter RCM:

```c
// Pseudocode for entering RCM from software:
writel(0x00000002, 0x7000E450);  // Set RCM bit in SCRATCH0
writel(0x00000010, 0x7000E400);  // Trigger reset
// Device reboots → Boot ROM sees bit 1 → enters RCM
```

---

## Pixel C GPIO Buttons

From upstream kernel `tegra210-smaug.dts` (gpio-keys patch):

| Button | GPIO | Pin | Active Level | Debounce |
|--------|------|-----|-------------|----------|
| Power | TEGRA_GPIO(X, 5) | — | LOW | 30ms |
| Volume Down | TEGRA_GPIO(X, 7) | — | LOW | — |
| Volume Up | TEGRA_GPIO(M, 4) | — | LOW | — |
| Lid Switch | TEGRA_GPIO(B, 4) | — | — (magnetic) | — |
| Tablet Mode | TEGRA_GPIO(Z, 2) | — | — | — |

Source: [tegra210-smaug gpio-keys patch](https://patchwork.ozlabs.org/comment/1286927/)

**Recovery Mode Strap GPIO**: Not publicly documented for Pixel C. On
Nintendo Switch, the strap is Pin 10 of the right Joy-Con connector shorted
to GND. On Pixel C, this strap is almost certainly routed only to an
internal testpoint (if at all), not to any external connector.

---

## Tegra QEMU Emulator

An emulator exists for T210 Boot ROM and early boot stages:

**Repository**: https://github.com/yellows8/tegra_qemu

Supports: Tegra2, X1 (T210), X1+ (T214)

This can be used to:
- Study Boot ROM behavior without hardware
- Test RCM payloads in emulation
- Debug boot chain issues
- Understand memory map and peripheral behavior

---

## Source Links

### Boot ROM and Boot Chain
- [NVIDIA Tegra Boot Flow (official)](https://http.download.nvidia.com/tegra-public-appnotes/tegra-boot-flow.html)
- [NVIDIA T210 nvtboot Boot Flow (official)](https://http.download.nvidia.com/tegra-public-appnotes/t210-nvtboot-flow.html)
- [Tegra X1 Boot Flow (TRENTOS documentation)](https://trent-os.github.io/trentos/platform-support/nvidia-tegra/nvidia_tegra_x1_bootflow.html)
- [BCT Overview (NVIDIA)](https://http.download.nvidia.com/tegra-public-appnotes/bct-overview.html)

### Boot ROM Dump and Analysis
- [Tegra X1 Boot ROM dump + IDA database](https://github.com/adeljck/Tegra-X1-Bootrom)
- [T210 bootrom dumped — wololo.net](https://wololo.net/2017/10/18/nintendo-switch-team-reswitched-dumped-tegra-210-bootrom-means/)
- [T210 ipatch dump/decode](https://gist.github.com/CTCaer/3213ba6ebc9f258e7811ec38f3c0b7ce)

### Exploits and Vulnerability Research
- [Fusee Gelee disclosure (Kate Temkin, PDF)](https://misc.ktemkin.com/fusee_gelee_nvidia.pdf)
- [ShofEL2 — fail0verflow blog](https://fail0verflow.com/blog/2018/shofel2/)
- [ShofEL2 source code](https://github.com/fail0verflow/shofel2)
- [Methodically Defeating Nintendo Switch Security (paper)](https://ar5iv.labs.arxiv.org/html/1905.07643)

### Firmware Source Code
- [Coreboot for Pixel C (smaug)](https://github.com/ttefke/coreboot-smaug)
- [Depthcharge bootloader](https://github.com/coreboot/depthcharge)
- [ARM Trusted Firmware — Tegra platform](https://github.com/ARM-software/arm-trusted-firmware/tree/master/plat/nvidia/tegra)
- [U-Boot for Tegra](https://github.com/OE4T/u-boot-tegra)
- [NVIDIA tegrarcm tool](https://github.com/NVIDIA/tegrarcm)

### Emulation
- [tegra_qemu — Tegra2/X1/X1+ emulation](https://github.com/yellows8/tegra_qemu)

### Kernel PMC Reboot Modes
- [ARM: tegra: Support reboot modes (kernel patch)](https://patchwork.kernel.org/project/linux-arm-kernel/patch/1399388651-12819-1-git-send-email-thierry.reding@gmail.com/)
- [T210 tegra_def.h (ARM-TF)](https://github.com/ARM-software/arm-trusted-firmware/blob/master/plat/nvidia/tegra/include/t210/tegra_def.h)
