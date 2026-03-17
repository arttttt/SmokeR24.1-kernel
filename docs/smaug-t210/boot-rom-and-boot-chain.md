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
10. Reads bootloader from boot media into IRAM (coreboot bootblock on Pixel C,
    nvtboot on Jetson/Shield)
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

## Pixel C Boot Chain (Verified from Coreboot Source)

**IMPORTANT**: The Pixel C does **NOT** use nvtboot. Unlike Jetson TX1 and
Shield TV which use the standard NVIDIA boot flow (Boot ROM → nvtboot → cboot),
the Pixel C uses coreboot directly as the first-stage bootloader loaded by
Boot ROM. This is confirmed by the coreboot tegra210 source code:
- `src/soc/nvidia/tegra210/bootblock.c` — runs on BPMP, reads BCT from IRAM
- `src/soc/nvidia/tegra210/romstage.c` — initializes SDRAM, starts CCPLEX
- `src/soc/nvidia/tegra210/ccplex.c` — powers up Cortex-A57 cores

Source: [coreboot/coreboot tegra210 SoC](https://github.com/coreboot/coreboot/tree/master/src/soc/nvidia/tegra210)

### Boot Flow Diagram

```
Power On
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│  Stage 1: BOOT ROM (IROM)                                │
│  ─────────────────────────                               │
│  Processor: BPMP (ARM7TDMI, 32-bit)                      │
│  Memory:    Executes from internal ROM                    │
│             Uses IRAM (256KB) for data/stack               │
│  Source:    CLOSED — mask ROM on silicon                   │
│             (dumped binary available, see above)           │
│                                                           │
│  Actions:                                                 │
│  • Read fuses and straps                                  │
│  • Check PMC_SCRATCH0 → RCM?                              │
│  • Check Recovery Strap GPIO → RCM?                        │
│  • Initialize eMMC controller                              │
│  • Load BCT from eMMC → IRAM                               │
│  • Validate BCT hash/signature                             │
│  • Load coreboot bootblock from eMMC → IRAM                │
│  • Validate bootblock hash/signature                       │
│  • Jump to coreboot bootblock entry point                  │
│  • (On error → enter RCM)                                  │
└────────────────────────┬─────────────────────────────────┘
                         ▼
┌──────────────────────────────────────────────────────────┐
│  Stage 2: Coreboot Bootblock                              │
│  ────────────────────────                                │
│  Processor: BPMP (still ARM7TDMI, 32-bit)                 │
│  Memory:    IRAM (256KB) — NO SDRAM YET                    │
│  Source:    OPEN — src/soc/nvidia/tegra210/bootblock.c     │
│                                                           │
│  Actions:                                                 │
│  • Enable JTAG                                            │
│  • MBIST workaround (clock gating fixes)                  │
│  • Clock init (UART, mselect, timers)                     │
│  • Read ODMDATA from BCT in IRAM                          │
│  • Console init                                           │
│  • Jump to romstage                                       │
│                                                           │
│  >>> BUTTON CHECK CAN BE ADDED HERE <<<                   │
│  GPIO read + PMC write work in IRAM, no SDRAM needed      │
└────────────────────────┬─────────────────────────────────┘
                         ▼
┌──────────────────────────────────────────────────────────┐
│  Stage 3: Coreboot Romstage                               │
│  ───────────────────────                                 │
│  Processor: BPMP (still ARM7TDMI)                         │
│  Memory:    IRAM → initializes SDRAM                       │
│  Source:    OPEN — src/soc/nvidia/tegra210/romstage.c      │
│                                                           │
│  Actions:                                                 │
│  • SDRAM initialization (sdram_init)                      │
│  • TrustZone region init                                  │
│  • GPU/NVDEC/TSEC/VPR carveout regions                    │
│  • CBMEM initialization                                   │
│  • ccplex_cpu_prepare() — power rails, clocks, RAM repair │
│  • Jump to ramstage                                       │
└────────────────────────┬─────────────────────────────────┘
                         ▼
┌──────────────────────────────────────────────────────────┐
│  Stage 4: Coreboot Ramstage                               │
│  ───────────────────────                                 │
│  Processor: CCPLEX (ARM Cortex-A57, 64-bit)               │
│  Memory:    SDRAM (3GB)                                    │
│  Source:    OPEN                                           │
│                                                           │
│  Transition: ccplex_cpu_start() launches A57 core,        │
│              clock_halt_avp() stops BPMP                   │
│                                                           │
│  Actions:                                                 │
│  • Full SoC peripheral initialization                     │
│  • Display init (JDI panel driver)                        │
│  • Load depthcharge payload from CBFS                     │
│  • Transfer control to depthcharge                        │
└────────────────────────┬─────────────────────────────────┘
                         ▼
┌──────────────────────────────────────────────────────────┐
│  Stage 5: Depthcharge                                     │
│  ─────────────────────                                   │
│  Processor: CCPLEX                                        │
│  Memory:    SDRAM                                          │
│  Source:    OPEN — github.com/coreboot/depthcharge         │
│                                                           │
│  Actions:                                                 │
│  • Fastboot server                                        │
│  • Verified Boot (vboot)                                  │
│  • Button handling (volume, power)                        │
│  • Boot mode selection (normal/recovery/bootloader)       │
│  • Load and boot Linux kernel                             │
└────────────────────────┬─────────────────────────────────┘
                         ▼
                    Linux Kernel
```

### Key Difference: Pixel C vs Other T210 Devices

| | Pixel C (smaug) | Jetson TX1 / Shield TV |
|---|---|---|
| Boot ROM loads | **Coreboot bootblock** | nvtboot (proprietary) |
| SDRAM init by | **Coreboot romstage** (open) | nvtboot (closed) |
| CCPLEX start by | **Coreboot romstage** (open) | nvtboot (closed) |
| 2nd stage | **Depthcharge** (open) | CBoot or U-Boot |
| Entire boot chain open? | **YES** (except Boot ROM) | No (nvtboot is closed) |

This means Pixel C has a **fully open boot chain** from the first instruction
after Boot ROM all the way to the kernel.

### What's Open, What's Closed

| Component | Open Source? | Repository / Location |
|-----------|-------------|----------------------|
| Boot ROM | **NO** (dumped binary available) | [Tegra-X1-Bootrom](https://github.com/adeljck/Tegra-X1-Bootrom) |
| BCT | Configurable (binary format) | Generated by NVIDIA cbootimage tool |
| Coreboot bootblock | **YES** | [coreboot tegra210](https://github.com/coreboot/coreboot/tree/master/src/soc/nvidia/tegra210) |
| Coreboot romstage | **YES** | same |
| Coreboot ramstage | **YES** | same |
| ARM Trusted Firmware | **YES** | [arm-trusted-firmware tegra](https://github.com/ARM-software/arm-trusted-firmware/tree/master/plat/nvidia/tegra) |
| Depthcharge | **YES** | [coreboot/depthcharge](https://github.com/coreboot/depthcharge) |
| Linux Kernel | **YES** | mainline + vendor trees |

### Where Button Handling Can Be Added

To add a "Vol Up + Vol Down → enter RCM" feature:

| Stage | Can add button check? | Helps when bricked? |
|-------|----------------------|-------------------|
| Boot ROM | **NO** (mask ROM) | N/A |
| **Coreboot bootblock** | **YES — BEST PLACE** | Yes, if eMMC/BCT readable |
| Coreboot romstage | YES | Yes, same scope as bootblock |
| Depthcharge | YES (but late) | Only if full boot chain works |

The coreboot bootblock is the **earliest modifiable code**. It runs on BPMP
in IRAM before SDRAM init. GPIO reads and PMC register writes work at this
stage. If Boot ROM can load the bootblock (BCT + bootblock valid in eMMC),
the button check will work.

If the device is bricked at the eMMC/BCT level (Boot ROM can't load anything),
**no software modification can help** — only hardware methods (eMMC shorting,
ISP programming).

---

## Proposed Coreboot Bootblock Patch: Button Combo → RCM

Adding ~10 lines to `tegra210_main()` in `bootblock.c`, **before** any other
init, gives a hardware button combination to force RCM entry:

```c
void tegra210_main(void)
{
    /*
     * Check Vol Up + Vol Down → reboot to RCM.
     * GPIO(M,4) = Volume Up, GPIO(X,7) = Volume Down.
     * Both are active LOW on Pixel C (smaug).
     * GPIO registers are memory-mapped, work from IRAM.
     */
    /* Configure as input (clear output enable bits) */
    uint32_t gpio_m_oe = read32((void *)0x6000d110);  /* GPIO port M OE */
    uint32_t gpio_x_oe = read32((void *)0x6000d610);  /* GPIO port X OE */
    write32((void *)0x6000d110, gpio_m_oe & ~(1 << 4));
    write32((void *)0x6000d610, gpio_x_oe & ~(1 << 7));

    uint32_t gpio_m_in = read32((void *)0x6000d108);  /* GPIO port M input */
    uint32_t gpio_x_in = read32((void *)0x6000d608);  /* GPIO port X input */

    if (!(gpio_m_in & (1 << 4)) && !(gpio_x_in & (1 << 7))) {
        /* Both volume buttons pressed — reboot into RCM */
        struct tegra_pmc_regs *pmc = (void *)TEGRA_PMC_BASE;
        write32(&pmc->scratch0, read32(&pmc->scratch0) | 0x2);
        write32(&pmc->cntrl, read32(&pmc->cntrl) | 0x10);
        while (1)
            ; /* wait for reset */
    }

    /* Normal boot continues below */
    enable_jtag();
    mbist_workaround();
    // ... rest of existing code ...
```

### Why This Works

- **Runs on BPMP (ARM7)** in IRAM — no SDRAM needed
- **GPIO is memory-mapped** at fixed addresses — works immediately after reset
- **PMC registers** are always accessible — no init needed
- **Adds no new dependencies** — uses only existing MMIO
- **~10 lines of code** in an existing, tested file

### Build and Flash

```bash
# 1. Clone coreboot-smaug:
git clone https://github.com/ttefke/coreboot-smaug
cd coreboot-smaug

# 2. Apply the button combo patch to src/soc/nvidia/tegra210/bootblock.c

# 3. Build:
make defconfig  # or use smaug_defconfig
make

# 4. Flash via fastboot (requires unlocked bootloader):
fastboot flash bootloader build/coreboot.rom
fastboot reboot

# 5. Test: hold Vol Up + Vol Down during power-on → RCM
```

### Safety Net Behavior

| Scenario | Without patch | With patch |
|----------|-------------|-----------|
| Normal power on | Boot normally | Boot normally |
| Vol Up + Vol Down held | Boot normally | **→ RCM (USB recovery)** |
| Corrupt kernel | Stuck | Vol+Vol → RCM → reflash |
| Corrupt depthcharge | Stuck | Vol+Vol → RCM → reflash |
| Corrupt romstage | Brick | Vol+Vol → RCM → reflash |
| Corrupt bootblock | Brick | Brick (patch is in bootblock) |
| Corrupt BCT/eMMC | Brick | Brick (Boot ROM can't read) |

---

## BCT (Boot Configuration Table)

The BCT is a binary data structure stored in the first sectors of eMMC. It
tells Boot ROM:

- How to configure the eMMC controller (timing, bus width)
- SDRAM controller parameters (training, timing, voltage)
- Where the bootloader is located on eMMC (coreboot bootblock on Pixel C,
  nvtboot on Jetson/Shield)
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
