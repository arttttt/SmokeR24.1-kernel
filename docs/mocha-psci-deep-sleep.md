# Tegra K1 (T124) Mocha — PSCI and Deep Sleep Issue

## Problem Summary

Device does not wake from deep sleep (LP0/LP1). Root cause: architectural
conflict between the legacy bootloader (KitKat-era, pre-PSCI) and the kernel
which expects a working PSCI 0.2+ implementation.

## Current Boot Chain

```
Boot ROM → nvtboot (KitKat-era blob) → TLK (TrustZone blob, PSCI 0.2)
               │                          │
               │                          ├── cpu_on       ✅
               │                          ├── cpu_off      ✅
               │                          ├── cpu_suspend   ✅ (LP2)
               │                          ├── LP1 suspend   ❌ does not wake
               │                          └── LP0 deep      ❌ does not wake
               │
               └── NVIDIA fastboot → kernel
```

TLK implements PSCI 0.2 via `psci_t124.c` (compiled into blob).
CPU hotplug and LP2 idle work. Deep sleep (LP1/LP0) does not.

## How the Kernel Enters Suspend

File: `arch/arm/mach-tegra/pm.c`

```
tegra_suspend_enter()                    ← platform_suspend_ops.enter
    └── tegra_sleep_core(LP0 or LP1)
            │
            ├── if (tegra_cpu_is_secure() && psci_ops.cpu_suspend)
            │       │
            │       └── psci_ops.cpu_suspend()     ← SMC call
            │               └── TLK psci_t124.c    ← CLOSED BLOB
            │                   ├── suspend entry   ← probably works
            │                   └── resume path     ← ❌ BROKEN
            │
            └── else (fallback, non-secure)
                    └── cpu_suspend(tegra3_sleep_core_finish)
                        └── sleep-t30.S            ← CORRECT T124 code
                            (never called)
```

`tegra_cpu_is_secure()` returns true (TLK is running) → kernel **always**
takes the PSCI SMC path → TLK → bug in TLK deep sleep resume.

The correct native T124 suspend/resume code in `sleep-t30.S` exists in
the kernel but is **never called** due to PSCI interception.

## T124 Suspend Levels

```
LP2 (cpu idle):     Single CPU core power-gated, DRAM active
                    Resume: Flow Controller IRQ → instant wakeup
                    Status: ✅ WORKS via TLK PSCI

LP1 (cluster off):  All CPUs off, clocks gated, DRAM in self-refresh
                    Resume: Flow Controller → clocks → exit self-refresh
                    Status: ❌ DOES NOT WAKE

LP0 (deep sleep):   Everything off except PMC + PMIC + DRAM self-refresh
                    Resume: Boot ROM → warmboot (WB0) → reinit SDRAM
                    Status: ❌ DOES NOT WAKE
```

## Root Cause: Bugs in TLK Resume Path

Analysis of JXD vendor source (the only available reference for TLK-era
bootloader code) revealed 8 critical differences from the correct T124 code
in kernel `sleep-t30.S`:

| # | What is broken | Correct (kernel) | Result |
|---|---------------|------------------|--------|
| 1 | `EMC_CFG_DIG_DLL` not disabled | `emc_cfg_dig_dll_off` macro | DLL may be active during self-refresh entry |
| 2 | `EMC_CFG bit 29` not cleared | `bic #(1<<29)` | DYN_SELF_REF not fully disabled |
| 3 | `AUTO_CAL wait` commented out | Active wait loop | Calibration may be active — undefined EMC state |
| 4 | `E_NO_VTTGEN` mask `#7` (3 bits) | `#0x3f` (6 bits) | 3 of 6 VTTGEN pad groups not disabled → leakage current |
| 5 | `PMC_IO_DPD_REQ = 0x8EC00000` (T30) | `0x80400000` (T124) | Wrong pads enter DPD |
| 6 | `PMC_IO_DPD3` entirely absent | 4 sequential steps + status waits | DPD3 pads unmanaged → **hang on resume guaranteed** |
| 7 | `PMC_POR_DPD_CTRL` absent | `orr/bic #0x80000003` | DPD override not configured |
| 8 | Flow Controller `HALT_CPU_IRQ/FIQ` | `HALT_LIC_IRQ/FIQ` | CPU does not wake — wrong interrupt routing |

Bugs #5, #6, #8 — each one independently guarantees a hang on resume.

The TLK blob contains a compiled version of this code. The blob cannot be
fixed without TLK source code.

## Solutions

### Solution 1: Kernel Bypass — Disable PSCI for Deep Sleep (Quick Fix)

Force the kernel to use the native tegra PM path for LP0/LP1, bypassing
PSCI SMC → TLK. PSCI remains active for cpu_on/cpu_off/LP2.

File: `arch/arm/mach-tegra/pm.c`, function `tegra_sleep_core()`:

```c
// CURRENT CODE:
if (tegra_cpu_is_secure()) {
    #if defined(CONFIG_ARM_PSCI)
    if (psci_ops.cpu_suspend) {
        pps.id = TEGRA_ID_CPU_SUSPEND_LP0;
        pps.type = PSCI_POWER_STATE_TYPE_POWER_DOWN;
        pps.affinity_level = TEGRA_PWR_DN_AFFINITY_CLUSTER;
        psci_ops.cpu_suspend(pps, virt_to_phys(tegra_resume));
    }
    #endif
}
// fallback:
cpu_suspend(v2p, tegra3_sleep_core_finish);
```

Patch options:

**Option A**: Skip PSCI for LP0/LP1 (keep it for LP2):
```c
if (tegra_cpu_is_secure()) {
    #if defined(CONFIG_ARM_PSCI)
    // Skip PSCI for deep sleep — TLK resume path is broken on T124.
    // Use native tegra PM path (sleep-t30.S) which has correct
    // T124 DPD3, PMC_IO_DPD_REQ, and Flow Controller handling.
    // PSCI still used for cpu_on/off/LP2 via cpuidle.
    #endif
}
cpu_suspend(v2p, tegra3_sleep_core_finish);
```

**Option B**: Same patch in `tegra_sleep_cpu_prefinish()` and
`tegra_stop_mc_clk()` — same `#if defined(CONFIG_ARM_PSCI)` blocks.

**Affected functions** (3 in `pm.c`):
- `tegra_sleep_core()` — LP0/LP1 entry
- `tegra_sleep_cpu_prefinish()` — cluster power down
- `tegra_stop_mc_clk()` — MC clock stop (LP1.1)

**Risks**:
- TLK may expect suspend to go through it (internal state tracking)
- Resume path in TLK may not match native resume in sleep-t30.S
- Secure/non-secure context on resume may be inconsistent

**Estimate**: 1-3 days for patch + testing.

### Solution 2: Replace TLK with ARM Trusted Firmware (Proper Fix)

```
Boot ROM → nvtboot (keep) → TF-A (replaces TLK) → fastboot → kernel
```

nvtboot loads the secure monitor from the TOS partition on eMMC. If the
TF-A binary is compatible in entry point and format, nvtboot will launch it.

TF-A provides:
- Reference PSCI 0.2/1.0 implementation
- Framework for platform-specific suspend/resume
- Existing `plat/nvidia/tegra/` support (for T210, adapt for T124)

T124 platform code needed (~1000 lines):
- PSCI cpu_suspend → LP2 via Flow Controller
- PSCI system_suspend → LP1 via sleep-t30.S T124 paths
- LP0 warmboot → replacement for WB0

**Challenges**:
- nvtboot sets TZDRAM size/address — need to determine what is allocated
- nvtboot configures MC security carveouts — TF-A must fit within them
- WB0 (warmboot binary) is tied to nvtboot/TLK — needs replacement

**Estimate**: 4-6 weeks.

### Solution 3: Coreboot + TF-A — Full Chain Replacement

```
Boot ROM → coreboot (replaces nvtboot) → TF-A (replaces TLK) → U-Boot → kernel
```

Full control: TZDRAM, carveouts, PSCI, warmboot — all open source.

Coreboot mainline already has `src/soc/nvidia/tegra124/` from the Nyan
Chromebook (same T124 SoC). A mainboard port for mocha is needed.

**Estimate**: 2-3 months for full LP0 support.

## Recommended Approach

```
Step 1: Kernel bypass (solution 1)       ← days, validate hypothesis
        │
        ├── Works → problem is in TLK, native path is correct
        │           can stop here or proceed to TF-A
        │
        └── Fails → problem is deeper (nvtboot/WB0/carveouts)
                     need solution 2 or 3

Step 2: TF-A replaces TLK (solution 2)   ← weeks, proper fix
        │
        ├── Works → full PSCI, suspend fixed
        │
        └── nvtboot conflicts (carveouts, WB0) → need solution 3

Step 3: Coreboot + TF-A (solution 3)     ← months, full control if needed
```

## Key Files

### Kernel Suspend Code (Correct T124 Implementation)

| File | Contents |
|------|----------|
| `arch/arm/mach-tegra/pm.c` | Platform suspend ops, PSCI dispatch, LP mode entry |
| `arch/arm/mach-tegra/sleep-t30.S` | LP0/LP1/LP2 entry + resume + self-refresh (**with T124 ifdefs**) |
| `arch/arm/mach-tegra/sleep.S` | Common helpers: cache flush, MMU off |
| `arch/arm/mach-tegra/sleep.h` | Macros: `cpu_to_halt_reg`, `cpu_to_csr_reg` |
| `arch/arm/mach-tegra/cpuidle-t11x.c` | CPU idle driver (LP2 via PSCI) |

### PSCI in TLK Blob (Closed Source)

| Component | Description |
|-----------|------------|
| `psci_t124.c` | PSCI implementation inside TLK (compiled into tos.img) |
| `nvtbootwb0.bin` | Warmboot binary for LP0 resume (tied to TLK) |

### Coreboot T124 (If Needed)

| File | Contents |
|------|----------|
| `src/soc/nvidia/tegra124/bootblock.c` | Bootblock for T124 (from Nyan) |
| `src/soc/nvidia/tegra124/romstage.c` | SDRAM init, CCPLEX start |
| `src/soc/nvidia/tegra124/flow_ctrl.c` | Flow Controller (suspend/resume!) |
| `src/soc/nvidia/tegra124/power.c` | Power gating |
| `src/mainboard/google/nyan/` | Nyan Chromebook mainboard (reference for mocha port) |

### External References

| Resource | Link |
|----------|------|
| ARM Trusted Firmware — Tegra | https://github.com/ARM-software/arm-trusted-firmware/tree/master/plat/nvidia/tegra |
| Coreboot T124 SoC | https://github.com/coreboot/coreboot/tree/master/src/soc/nvidia/tegra124 |
| Coreboot Nyan mainboard | https://github.com/coreboot/coreboot/tree/master/src/mainboard/google/nyan |
| STM32MP psci.c (reference) | `arch/arm/mach-stm32mp/psci.c` in U-Boot (~200 lines, full PSCI 1.0) |
| NVIDIA Tegra Boot Flow | https://http.download.nvidia.com/tegra-public-appnotes/tegra-boot-flow.html |
| JXD vendor source (reference) | `/Users/artem/Projects/vendor_nvidia_jxd_src/` |
