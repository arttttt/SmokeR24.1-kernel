# TrustZone & TLK (Trusted Little Kernel) — Tegra124 / MiPad

Analysis of the stock TOS (Trusted OS) image from Xiaomi MiPad (mocha).
Source: TOS partition dump (`/dev/block/platform/sdhci-tegra.3/by-name/TOS`).

---

## Overview

NVIDIA TLK is a minimal Trusted OS running in ARM TrustZone secure world.
It provides secure services (DRM, keystore, crypto) to Android via SMC calls.

Stock boot chain with TLK:

```
BootROM -> BCT -> nvtboot (NVC) -> TLK init -> Bootloader (EBT) -> Android
                                     |                                |
                                     +--- secure world (EL3) ---------+--- SMC calls
```

TLK initializes during early boot, installs SMC handlers, then returns
control to the bootloader in non-secure (normal world) mode. Android HALs
later issue SMC instructions to invoke TLK services.

---

## TOS Image Format (NVTOSP)

The TOS partition contains an image with the following header:

```
Offset  Size    Type        Field
------  ------  ----------  ----------------------------------
0x000   7       char[7]     Magic: "NVTOSP\0"
0x007   varies  char[]      Payload size as ASCII decimal + \0
                            (e.g. "1667072\0")
0x010   4       uint32_le   Reserved (zero)
0x014   4       uint32_le   TLK kernel size (code + rodata)
0x018   4       uint32_le   TLK kernel size (duplicate)
0x01c   4       uint32_le   Total payload size
0x020   4       uint32_le   Flags / header info (0x50 = 80)
0x024   476     -           Padding (zeros) to 0x200
0x200   ...     -           ARM payload (exception vectors + TLK + TAs)
```

### MiPad TOS image values

| Field               | Value        | Notes                        |
|---------------------|--------------|------------------------------|
| Magic               | `NVTOSP\0`   |                              |
| Size string         | `1667072`    | Payload size in ASCII        |
| TLK kernel size     | `0x0000e1dc` | 57,820 bytes (~56KB)         |
| Total payload size  | `0x00197000` | 1,667,072 bytes (~1628KB)    |
| Header size         | `0x00000050` | 80 bytes (or flags)          |
| Total file size     | 1,667,584    | Header (512) + payload       |

---

## Payload Structure

The payload at offset 0x200 contains the TLK kernel followed by
Trusted Applications (TAs), all packed into a single binary.

```
Offset      Size      Content
----------  --------  ------------------------------------------
0x000-0x1FF  512B     NVTOSP header
0x200-0x207  32B      ARM exception vector table
                        0x200: b  reset_handler      (0x220)
                        0x204: b  undef_handler       (0xad0)
                        0x208: b  smc_handler         (0xafc)
                        0x20c: b  prefetch_abort      (0xb3c)
                        0x210: b  data_abort          (0xb58)
                        0x214: b  hyp_handler         (0xb74)
                        0x218: b  irq_handler         (0xb78)
                        0x21c: b  fiq_handler         (0xbd4)
0x200-0xe3db  ~56KB   TLK kernel
                        - Platform init (fuse, tz, clocks)
                        - PSCI implementation (psci_t124.c)
                        - Task scheduler
                        - Memory manager
                        - SMC dispatcher
0xe3dc-0x197200 ~1571KB  Trusted Applications (packed)
```

### Exception vector table (ARM32)

At offset 0x200, standard ARM exception vectors with branch instructions:

```
0x200: 0xea000006  b  -> 0x220   (reset)
0x204: 0xea000231  b  -> 0xad0   (undef)
0x208: 0xea00023b  b  -> 0xafc   (SWI/SMC)
0x20c: 0xea00024a  b  -> 0xb3c   (prefetch abort)
0x210: 0xea000250  b  -> 0xb58   (data abort)
0x214: 0xea000256  b  -> 0xb74   (hyp)
0x218: 0xea000256  b  -> 0xb78   (IRQ)
0x21c: 0xea00026c  b  -> 0xbd4   (FIQ)
```

---

## Memory Layout

### Virtual address space

TLK uses an internal MMU to remap physical TZDRAM to a fixed virtual base:

```
Physical (TZDRAM):      0xFFC00000 (bootloader loads here)
                              |
                              v  MMU remap by TLK
Virtual (TLK internal): 0x48000000  Base
                        0x48000200  Exception vectors / code entry
                        0x4800e1dc  End of TLK kernel (~56KB)
                        0x4800e1dc  Start of Trusted Applications
                        0x48197000  End of payload (~1628KB)
                        0x4819c000  Stacks / heap (runtime)
```

Evidence from literal pool at file offset 0x490:

| Pool offset | Value        | Meaning                            |
|-------------|-------------|------------------------------------|
| 0x49c       | `0xcafebabe` | Placeholder for physical TZDRAM base (patched by bootloader) |
| 0x4a0       | `0x48000000` | TLK virtual base                   |
| 0x4a4       | `0x48198000` | End of mapped region               |
| 0x4a8       | `0x4819c000` | Stack/heap region start            |
| 0x4ac       | `0x48000268` | Early code entry point             |
| 0x4b0       | `0x48197000` | End of payload (matches header)    |
| 0x4b4       | `0x480002b8` | Secondary code entry               |

### Physical addresses referenced

| Address      | File offset  | Usage                              |
|-------------|-------------|------------------------------------|
| `0xfff00000` | `0x459b`    | Secure base (1MB config)           |
| `0xffc00000` | `0x112a6`   | Secure base (4MB config)           |
| `0xffc00000` | `0x1977a`   | Secure base reference              |

### TZDRAM configuration

Current U-Boot config (Linux-only, no TLK):

```c
CONFIG_ARMV7_SECURE_BASE         = 0xfff00000  // top 1MB
CONFIG_ARMV7_SECURE_RESERVE_SIZE = 0x00100000  // 1MB
```

Required for TLK loading:

```c
CONFIG_ARMV7_SECURE_BASE         = 0xffc00000  // top 4MB
CONFIG_ARMV7_SECURE_RESERVE_SIZE = 0x00400000  // 4MB
```

Rationale: TLK payload is 1.59MB, mapped region extends to 0x19c000
(1.61MB), plus heap/stack runtime allocation needs. 4MB provides margin.

Memory Controller security registers enforce the carveout:

```c
mc_security_cfg0 = CONFIG_ARMV7_SECURE_BASE              // base
mc_security_cfg1 = CONFIG_ARMV7_SECURE_RESERVE_SIZE >> 20 // size in MB
```

---

## Embedded Trusted Applications

The TOS image bundles 7 TAs after the TLK kernel. All compiled with
`Android clang version 3.8.256229` and linked with BoringSSL.

| TA Name                    | Purpose                                | Android HAL          |
|----------------------------|----------------------------------------|----------------------|
| `oemcrypto_secure_service` | Widevine L1 DRM content decryption     | MediaDrm             |
| `crypto_service`           | General-purpose crypto, key derivation | Keymaster (partial)  |
| `secure_otf`               | On-the-fly media decryption (OTF)      | MediaCodec           |
| `secure_rtc`               | Secure real-time clock                 | -                    |
| `hdcp_secure_service`      | HDMI content protection (HDCP)         | -                    |
| `hwkeystore`               | Hardware-backed key storage            | Keymaster            |
| `tlkstoragedemo_task`      | Secure storage demo                    | -                    |

### TA capabilities (from strings analysis)

**oemcrypto_secure_service:**
- RSA key loading and signing
- AES content encryption/decryption (CENC)
- Usage table management (license tracking)
- Widevine key box loading and decryption
- Secure data path enforcement

**crypto_service:**
- Platform secret decryption
- Vudu platform key derivation
- AES/RSA/CMAC operations via BoringSSL
- SE (Security Engine) hardware RNG

**hwkeystore:**
- RSA/DSA/EC key generation
- Key import/export
- Data signing and verification
- Source: `tegra/ote/storage/hwkeystore/` in JXD vendor source

---

## Boot Protocol

### TLK initialization sequence (from strings)

```
1. "Welcome to TLK"
2. "starting platform early init (TLK <version>)"
3. "bootstrap"                                      -- kernel init
4. "NS PHYS buffer support available"               -- normal world buffers
5. Task loading (each TA loaded into secure memory)
6. "TLK initialization complete. Jumping to non-secure world"
```

### Bootloader -> TLK handoff

The bootloader must:

1. Load TLK payload (file offset 0x200+) into TZDRAM
2. Patch `0xcafebabe` at payload offset 0x29c with physical TZDRAM base
3. Configure MC security registers
4. Jump to TZDRAM base in secure mode (SVC/MON)

TLK then:

1. Sets up MMU (physical TZDRAM -> VA 0x48000000)
2. Initializes TrustZone (fuses, clocks, TZRAM)
3. Loads embedded TAs into task memory
4. Installs SMC vector table
5. Returns to normal world (bootloader continues)

### 0xcafebabe patching

The literal pool at offset 0x29c (relative to payload start at 0x200,
i.e. file offset 0x49c) contains `0xcafebabe`. This is a placeholder
that the bootloader must replace with the physical TZDRAM base address
before jumping to TLK.

TLK uses this value to set up its physical-to-virtual MMU mapping.

---

## TLK Communication (Runtime)

### SMC interface

Android HALs invoke TLK services via ARM SMC (Secure Monitor Call)
instructions. The SMC handler is at exception vector offset +8
(file offset 0x208 -> handler at 0xafc).

### TLK daemon (normal world)

TLK cannot access the filesystem from secure world. A userspace daemon
(`tlk_daemon`) handles file I/O on its behalf via `/dev/tlk_device`:

```c
// Daemon main loop:
ioctl(dev_fd, TE_IOCTL_FILE_NEW_REQ, req);       // get request from TLK
ioctl(dev_fd, TE_IOCTL_FILE_FILL_BUF, req);      // data transfer (writes)
ioctl(dev_fd, TE_IOCTL_FILE_REQ_COMPLETE, req);   // signal completion
```

Request types:
- `OTE_FILE_REQ_READ` — TLK requests a file read
- `OTE_FILE_REQ_WRITE` — TLK requests a file write
- `OTE_FILE_REQ_DELETE` — TLK requests a file deletion
- `OTE_FILE_REQ_SIZE` — TLK queries file size

Storage location: `/data/tlk/` (managed by daemon)

Source: `tegra/ote/daemon/tlk_daemon.c` in JXD vendor source.

### EKS (Encrypted Key Storage)

The EKS partition (~80KB) contains encrypted keys used by Widevine
and HDCP TAs. The `eks.dat` file is loaded separately from TOS.
Without EKS, DRM and HDCP TAs will fail to initialize their key material.

---

## eMMC Partition Layout (TrustZone-relevant)

From `android_fastboot_nvtboot_dtb_emmc_full.cfg`:

| Partition | ID | Size    | Filename       | Purpose                       |
|-----------|----|---------|----------------|-------------------------------|
| EBT       | 4  | 6 MB    | bootloader.bin | Main bootloader (or U-Boot)   |
| NVC       | 5  | 256 KB  | nvtboot.bin    | NVIDIA microloader             |
| TOS       | 8  | ~2.1 MB | tos.img        | TLK (Trusted OS)              |
| EKS       | 9  | ~80 KB  | eks.dat        | Encrypted key storage         |
| WB0       | 11 | 4 KB    | nvtbootwb0.bin | Warmboot (LP0 resume)         |

---

## Loading TLK from U-Boot (Implementation Plan)

### Required changes

1. **Expand secure carveout** in `include/configs/mocha.h`:
   ```c
   #define CONFIG_ARMV7_SECURE_BASE         0xffc00000
   #define CONFIG_ARMV7_SECURE_RESERVE_SIZE 0x00400000
   ```

2. **Read TOS partition** from eMMC (partition ID 8)

3. **Parse NVTOSP header:**
   - Verify magic == `"NVTOSP\0"`
   - Read payload size from offset 0x1c
   - Payload starts at offset 0x200

4. **Copy payload to TZDRAM** at `CONFIG_ARMV7_SECURE_BASE`

5. **Patch `0xcafebabe`:**
   - Scan payload for `0xcafebabe` (at offset 0x29c in payload)
   - Replace with `CONFIG_ARMV7_SECURE_BASE`

6. **Configure MC security** (already done in `ap.c:protect_secure_section`)

7. **Jump to TZDRAM base** in secure mode

### Risks and unknowns

| Risk | Severity | Notes |
|------|----------|-------|
| Additional boot parameters via registers | High | Bootloader may pass info in r0-r3 or shared memory; needs experimentation |
| Secure boot chain validation | High | If SBK fuses are burned, TLK may reject non-NVIDIA caller |
| EKS loading | Medium | TAs may crash without EKS data; needs separate loading |
| TZDRAM size insufficient | Medium | 4MB should suffice but TLK runtime allocation is unknown |
| `0xcafebabe` not the only patch point | Medium | There may be other relocations; needs testing |
| LP0/LP1 resume path | Low | Warmboot (WB0) may need TLK re-init; not needed for initial bringup |

### What works without TLK (Linux boot)

U-Boot already implements its own minimal PSCI in `arch/arm/mach-tegra/psci.S`
for CPU on/off. This is sufficient for Linux. TLK is only needed for Android
secure services.

---

## PSCI Implementation

### Current state (U-Boot)

U-Boot implements a minimal PSCI for Tegra124 in `arch/arm/mach-tegra/psci.S`.
The generic ARMv7 PSCI framework in `arch/arm/cpu/armv7/psci.S` provides the
SMC dispatch table and registers all PSCI 0.1 / 0.2 / 1.0 function IDs.
Platform-specific functions are `.weak` symbols — override them to implement.

| Function            | FID (0.2)    | Status      | Notes                          |
|---------------------|-------------|-------------|--------------------------------|
| `PSCI_VERSION`      | `0x84000000` | **Stub** → -1 | Needs: `return ARM_PSCI_VER_0_2` |
| `CPU_SUSPEND`       | `0x84000001` | **Stub** → -1 | Needs: LP2/LP1 via Flow Controller |
| `CPU_OFF`           | `0x84000002` | **Done**    | Flow Controller + WFI          |
| `CPU_ON`            | `0x84000003` | **Done**    | Flow Controller wake + reset vector |
| `AFFINITY_INFO`     | `0x84000004` | **Stub** → -1 | Needs: read Flow Controller CSR |
| `MIGRATE_INFO_TYPE` | `0x84000006` | **Stub** → -1 | Needs: `return 2` (no trusted OS) |
| `SYSTEM_OFF`        | `0x84000008` | **Stub** → -1 | Needs: PMC power off           |
| `SYSTEM_RESET`      | `0x84000009` | **Stub** → -1 | Needs: PMC main reset (code exists in `pmc.c`) |
| `PSCI_FEATURES`     | `0x8400000a` | **Stub** → -1 | Needs: report supported functions |
| `CPU_FREEZE`        | `0x8400000b` | **Stub** → -1 | Optional                       |
| `SYSTEM_SUSPEND`    | `0x8400000e` | **Stub** → -1 | Needs: LP0 entry (SDRAM off)   |

### PSCI 0.2 implementation plan

PSCI 0.2 is achievable with ~80 lines of C, using STM32MP's `psci.c` as
template. All hardware primitives already exist:

```
psci_version()           → return ARM_PSCI_VER_0_2;
psci_features()          → switch on supported FIDs, return 0 or NI
psci_affinity_info()     → read Flow Controller CSR for target CPU
psci_system_reset()      → PMC_CNTRL |= MAIN_RST (already in pmc.c:reset_cpu)
psci_system_off()        → PMC power off sequence
psci_migrate_info_type() → return 2 (no trusted OS in U-Boot mode)
```

Reference implementation: `arch/arm/mach-stm32mp/psci.c` (200 lines, full
PSCI 1.0 for a dual-core Cortex-A7 — structurally identical to what Tegra124
needs).

### PSCI cpu_suspend — power states

Tegra124 has three CPU low-power states, managed via Flow Controller
(`0x60007000`) and PMC (`0x7000E400`):

```
LP2 (cpu idle):     Single CPU core power gated
                    DRAM active, clocks active
                    Resume: Flow Controller IRQ/FIQ → instant wakeup
                    Complexity: LOW — just Flow Controller halt + WFI

LP1 (cluster off):  All CPUs off, CPU/core clocks gated
                    DRAM in self-refresh
                    Resume: Flow Controller → re-enable clocks → exit self-refresh
                    Complexity: MEDIUM — needs EMC self-refresh sequence

LP0 (deep sleep):   Everything off except PMC + PMIC + DRAM self-refresh
                    Resume: BootROM → warmboot binary in IRAM → reinit SDRAM
                    Complexity: HIGH — needs WB0, IRAM code, full SDRAM reinit
```

### Low-level sleep code: sources and T124 differences

The same SDRAM self-refresh / Flow Controller halt logic exists in three
independent copies, each adapted for its execution context:

```
Source                              Context              MMU    Secure
──────────────────────────────────  ───────────────────  ─────  ──────
kernel sleep-t30.S                  Linux (non-secure)   On     No
JXD nvbllp0/sleep.S                 Bootloader           Off    Yes
TLK psci_t124.c (compiled)          TrustZone secure     Own    Yes
```

**CRITICAL:** JXD `nvbllp0/sleep.S` contains only T30 code paths despite
being in a T124-era source tree. It lacks all `TEGRA_12x_SOC` ifdefs.
The kernel `sleep-t30.S` is the correct and complete reference — it covers
T30, T114, T148, and T124 via `CONFIG_ARCH_TEGRA_12x_SOC` ifdefs.

#### Detailed comparison: JXD sleep.S vs kernel sleep-t30.S

**File sizes:** JXD: 485 lines (T30 only) vs Kernel: 1322 lines (T30 + T114 + T124)

##### Shared macros (identical in both)

All low-level macros are byte-for-byte identical:
`emc_device_mask`, `emc_timing_update`, `wait_for_us`, `wait_until`,
`cpu_to_halt_reg`, `cpu_to_csr_reg`, `cpu_id`, `mov32`

##### Macros only in kernel

| Macro | Purpose | Why absent from JXD |
|-------|---------|---------------------|
| `pll_enable` | Re-enable PLL during LP1 resume | JXD does PLL restart in C code (`nvbl_lp0.c`) |
| `pll_locked` | Wait for PLL lock | Same |
| `pll_iddq_exit/entry` | T114/T124 PLL IDDQ power saving | T30 has no IDDQ |
| `set_voltage` | I2C DVC core voltage change in LP1 | JXD handles in C code |

##### Function mapping

| JXD | Kernel | Notes |
|-----|--------|-------|
| `NvBlLp0CoreSuspend` | `tegra3_sleep_core_finish` | Same logic, different names |
| `tegra_turn_off_mmu` | `tegra_turn_off_mmu` (in `sleep.S`) | JXD: inline; kernel: separate file |
| `tegra_shut_off_mmu` | `tegra_shut_off_mmu` (in `sleep.S`) | Identical |
| `g_NvBlLp0TearDownCore` | `tegra3_tear_down_core` | Same: calls self-refresh then enter_sleep |
| `tegra3_enter_sleep` | `tegra3_enter_sleep` | **Differs** — see below |
| `tegra3_sdram_self_refresh` | `tegra3_sdram_self_refresh` | **Major differences** — see below |
| `NvBlLp0CoreResume` | `tegra3_lp1_reset` | **Completely different** — see below |
| — | `tegra30_hotplug_shutdown` | Only in kernel (CPU hotplug) |
| — | `tegra30_cpu_shutdown` | Only in kernel (LP2 per-CPU) |
| — | `tegra3_sleep_cpu_secondary_finish` | Only in kernel (secondary CPU LP2) |
| — | `tegra3_stop_mc_clk_finish` | Only in kernel (MC clock stop for LP1.1) |
| — | `emc_exit_selfrefresh` | Only in kernel (~60 lines T124 DPD3 recovery) |

##### `tegra3_enter_sleep` — differences

```
Step                    JXD                            Kernel
────                    ───                            ──────
Timestamp save          (none)                         str r1, [r4, #PMC_SCRATCH38]

Flow Controller CSR:    Identical setup                Identical setup

Halt register:
  T30 path              HALT_CPU_IRQ | HALT_CPU_FIQ    HALT_CPU_IRQ | HALT_CPU_FIQ
  T11x path             ifdef TEGRA_11x_SOC only       ifdef CONFIG_ARCH_TEGRA_11x_SOC
  T124 path             MISSING — falls into T30!      CONFIG_ARCH_TEGRA_12x_SOC:
                                                         HALT_LIC_IRQ | HALT_LIC_FIQ
                        ^^^ WRONG for T124             ^^^ REQUIRED for T124
                        (uses HALT_CPU not HALT_LIC)   (Legacy Interrupt Controller)

WFI/WFE:               WFI always                     T124/T114: WFI; T30: WFE

Debug lock:             Identical                      Identical
```

**Critical:** On T124, using `HALT_CPU_IRQ/FIQ` (JXD) instead of
`HALT_LIC_IRQ/FIQ` (kernel) means the CPU may not wake up from LP1/LP0
because the interrupt routing is different on T124.

##### `tegra3_sdram_self_refresh` — line-by-line comparison

```
Step  Operation               JXD (T30)                   Kernel (T124 path)
────  ─────────               ─────────                   ──────────────────
1     EMC base                EMC_PA_BASE (ifdef, ok)     TEGRA_EMC_BASE

2     EMC_SEL_DPD_CTRL        ABSENT                      ldr/orr #0x1FF/str
      (reg 0x3d8)                                         Enables selective DPD
                              ^^^ MISSING                 ^^^ REQUIRED for T124

3     ZCAL/AUTO_CAL off       Identical                   Identical

4     EMC_CFG disable         bic #(1<<28) only           bic #(1<<28)
      DYN_SELF_REF                                        bic #(1<<29)
                              ^^^ MISSING bit 29          ^^^ T124 needs both bits

5     Timing update           Identical                   Identical

6     AUTO_CAL_ACTIVE wait    COMMENTED OUT!              Active wait loop
                              /* emc_wait_audo_cal:       emc_wait_audo_cal:
                                 ... */                     ldr/tst/bne
                              ^^^ DANGEROUS               ^^^ Required: if cal is active
                              Cal may be running when       during self-refresh entry,
                              entering self-refresh         EMC state is undefined

7     Stall DRAM requests     Identical (REQ_CTRL = 3)    Identical

8     Wait EMC idle           Identical (status bit 2)    Identical

9     Enter self-refresh      Identical (SELF_REF = 1)    Identical

10    Wait self-refresh done  Identical (device mask)     Identical

11    XM2VTTGEN DRVUP/DN      Identical (mask 0xF8F8FFFF) Identical

12    XM2VTTGENPADCTRL2       orr r1, r1, #7             orr r1, r1, #0x3f
      E_NO_VTTGEN             ^^^ 3 bits (T30)           ^^^ 6 bits (T124)
                              Wrong for T124: only 3 of   All 6 VTTGEN pad groups
                              6 pad groups disabled        disabled correctly

13    Timing update           Identical                   Identical

14    PMC LP0 check           Identical (tst PMC_CTRL)    Identical

15    PMC_POR_DPD_CTRL        ABSENT                      orr #0x80000003
      (reg 0x264)                                         Enables DPD override
                              ^^^ MISSING                 ^^^ Required before DPD entry

16    PMC_IO_DPD_REQ          mov32 0x8EC00000            mov32 0x80400000
                              ^^^ T30 value               ^^^ T124 value (different pads)
                              Wrong pads for T124

17    DPD_STATUS wait         ABSENT                      wait bit 22
                              ^^^ No confirmation         ^^^ Required

18    PMC_IO_DPD3_REQ         ABSENT (register doesn't    0x830DFFFF (func pads)
      func pads               exist on T30)               wait DPD3_STATUS bit 18

19    PMC_IO_DPD3_REQ         ABSENT                      0x8CD00000 (VTTGEN pads)
      VTTGEN pads                                         wait DPD3_STATUS bit 20

20    PMC_IO_DPD3_REQ         ABSENT                      0x80020000 (BGBIAS pads)
      BGBIAS pads                                         wait DPD3_STATUS bit 17

                              Steps 15-20 ENTIRELY        4 sequential DPD3 steps
                              ABSENT from JXD             with status polling
```

##### Resume path — completely different

| Aspect | JXD `NvBlLp0CoreResume` | Kernel `tegra3_lp1_reset` |
|--------|-------------------------|---------------------------|
| CPU mode init | Clears SPSR in all modes (SVC/FIQ/IRQ/ABT/UND/SYS) | Handled by kernel resume framework |
| Cache | `nvaosConfigureCache` (NVIDIA bootloader function) | Kernel cache init |
| FPU | `initFpu` (NVIDIA function) | Not needed in this path |
| SCTLR | Explicit bic/orr for MMU/cache/branch predictor | Kernel framework handles |
| PLL restart | Not in ASM — delegated to `NvBlLp0StartResume` (C) | **Full ASM:** PLLM/PLLC/PLLX/PLLP enable + IDDQ exit (T124) + lock wait |
| EMC exit self-refresh | Not in ASM — delegated to C code | **Full ASM:** 60+ lines for T124 DPD3 pad recovery (BGBIAS → VTTGEN → func → IO, each with status wait) |
| Core voltage | Not in ASM | `set_voltage` macro: I2C DVC transaction for LP1 low voltage restore |
| Clock restore | Not in ASM | SCLK/CCLK burst restore, MSELECT, PLLP reshift, PLLX_DIV2 |
| L2 cache | Not in ASM | L2 powergate toggle + unclamping |

##### Pad save area

JXD saves 8 registers:
```
EMC_CFG, EMC_ZCAL_INTERVAL, EMC_AUTO_CAL_INTERVAL,
EMC_XM2VTTGENPADCTRL, EMC_XM2VTTGENPADCTRL2,
PMC_IO_DPD_STATUS, CLK_SOURCE_MSELECT, SCLK_BURST
```

Kernel (T124) saves the same 8 plus:
```
CLK_RESET_CCLK_BURST  (includes PLLX_DIV2 state)
PMC_IO_DPD3_STATUS    (for DPD3 pad recovery during resume)
```

##### Summary: 8 critical differences that break T124

| # | Issue | JXD | Kernel T124 | T124 consequence |
|---|-------|-----|-------------|------------------|
| 1 | `EMC_SEL_DPD_CTRL` | Absent | Enables selective DPD (0x1FF) | Pads don't enter low-power; possible hang or excess current |
| 2 | `EMC_CFG bit 29` | Not cleared | `bic #(1<<29)` | DYN_SELF_REF may not fully disable |
| 3 | `AUTO_CAL wait` | Commented out | Active wait loop | Calibration may be active during self-refresh entry — undefined EMC state |
| 4 | `E_NO_VTTGEN` mask | `#7` (3 bits) | `#0x3f` (6 bits) | Only 3 of 6 VTTGEN pad groups disabled; current leak |
| 5 | `PMC_IO_DPD_REQ` value | `0x8EC00000` (T30) | `0x80400000` (T124) | Wrong pads enter DPD; possible hang on resume |
| 6 | `PMC_IO_DPD3` | Entirely absent | 4 sequential steps with status waits | DPD3 pads unmanaged — **hang on resume guaranteed** |
| 7 | `PMC_POR_DPD_CTRL` | Absent | `orr/bic #0x80000003` on entry/resume | DPD override not configured |
| 8 | Flow Controller halt | `HALT_CPU_IRQ/FIQ` | `HALT_LIC_IRQ/FIQ` | CPU may not wake from LP1/LP0 — wrong interrupt routing |

**Conclusion:** JXD `sleep.S` cannot be used for T124 suspend/resume.
The kernel `sleep-t30.S` with `CONFIG_ARCH_TEGRA_12x_SOC` paths is the
only correct and complete reference for implementing PSCI cpu_suspend
and system_suspend on Tegra124.

#### Source file map for sleep/suspend code

| File | Content | T124? |
|------|---------|-------|
| `kernel: arch/arm/mach-tegra/sleep-t30.S` | Full LP0/LP1/LP2 entry + resume + self-refresh | **Yes** — complete with all T124 ifdefs |
| `kernel: arch/arm/mach-tegra/sleep.S` | Common sleep helpers, cache flush, MMU off | Yes — generic |
| `kernel: arch/arm/mach-tegra/sleep.h` | Macros: `cpu_to_halt_reg`, `cpu_to_csr_reg`, etc. | Yes |
| `JXD: core/system/fastboot/nvbllp0/sleep.S` | Bootloader LP0 entry + self-refresh | **No** — T30 only paths |
| `JXD: core/system/fastboot/nvbllp0/nvbl_lp0.c` | C-level LP0 management (PMC, pinmux, PMIC) | Yes — T124 ifdefs |
| `JXD: core/system/nvaboot/nvaboot_warmboot_avp_t124.S` | AVP warmboot (SDRAM reinit from LP0) | **Yes** — T124 specific |
| `JXD: hwinc/t12x/aremc.h` | EMC register definitions for T124 | Yes |
| `JXD: hwinc/t12x/arflow_ctlr.h` (if exists) | Flow Controller register definitions | Yes |

### Why the same code exists in three places

The same hardware operation (e.g., SDRAM self-refresh) must be performed
by different software components depending on the system configuration:

```
                          Who owns secure world?
                         ┌──────────┬──────────┐
Suspend trigger:         │  TLK     │  U-Boot  │
─────────────────────────┼──────────┼──────────┤
Linux cpu_suspend SMC    │  TLK     │  U-Boot  │
Linux system_suspend SMC │  TLK     │  U-Boot  │
Bootloader LP0 (charge)  │  N/A     │ Bootloader│
                         └──────────┴──────────┘

Each runs in a different context:
  - Linux copy:      runs with Linux MMU/page tables, non-secure mode
  - TLK copy:        runs with TLK MMU (VA 0x48000000), secure mode
  - Bootloader copy: runs with no MMU, physical addresses, secure mode
```

The register sequences are identical but the surrounding environment
(MMU state, security mode, interrupt handling, return path) differs,
making a shared library impractical. This duplication is standard practice
in ARM firmware (ARM Trusted Firmware / OP-TEE have the same pattern).

### OP-TEE as alternative to TLK

OP-TEE (Open Portable Trusted Execution Environment) is an open-source
Trusted OS that could replace the stock TLK. It already supports ARMv7
and NVIDIA Tegra210 (Jetson TX1).

Porting to T124 requires ~4000 lines of platform code:

| Component | LOC | Difficulty | Reference |
|-----------|-----|------------|-----------|
| Platform init (main.c, conf.mk) | ~300 | Easy | OP-TEE T210 platform |
| PSCI (cpu_on/off/suspend) | ~400 | Easy | U-Boot `psci.S` + kernel `sleep-t30.S` |
| Flow Controller driver | ~200 | Easy | Both U-Boot and kernel |
| Security Engine driver (AES/RNG) | ~500 | Medium | JXD `hwinc/t12x/arse.h` |
| GIC-400 setup | ~200 | Easy | Standard ARM GIC |
| MC/SMMU TrustZone config | ~300 | Easy | U-Boot `ap.c` |
| UART debug output | ~200 | Easy | Standard Tegra UART |
| Kernel TEE driver integration | ~500 | Medium | Existing OP-TEE driver |
| Fuse reading | ~200 | Easy | JXD `hwinc/t12x/nvboot_fuse.h` |
| Testing/debugging | — | Hard | No JTAG = printf only |

Advantages over stock TLK:
- Full source code, auditable
- Upstream community support
- Standard GlobalPlatform TEE API
- Keymaster/Gatekeeper TAs available from Android

Disadvantage:
- Widevine L1 requires Google-signed OEMCrypto TA, which only exists for stock TLK

---

## Widevine DRM

### Security level

The MiPad TOS supports **Widevine L1** (highest security level).

Evidence from TOS binary strings:
- `oemcrypto_secure_service` TA runs inside TLK (secure world) — this is
  the defining characteristic of L1 vs L3
- `"Selected key require secure data path"` — secure data path (SDP) is
  exclusive to L1; decrypted video frames never leave secure memory
- `DecryptCENC` + `DecryptCTR` in secure world — content decryption in TEE
- `RewrapDeviceRsaKey`, `GenericSign`, `GenericVerify`, `LoadDeviceRsaKey`,
  `ForceDeleteUsageEntry`, security_patch_level checks — OEMCrypto v12/v13 API

### OEMCrypto version

OEMCrypto API version can be determined from the function set in the TOS:

| Function | Introduced in |
|----------|---------------|
| `DecryptCENC` | v9 |
| `LoadDeviceRsaKey` | v9 |
| `RewrapDeviceRsaKey` | v9 |
| `GenericSign` / `GenericVerify` | v9 |
| `UpdateUsageTable` | v10 |
| `ForceDeleteUsageEntry` | v12 |
| `security_patch_level` check | v11+ |
| `decrypt_cenc_flags` | v11+ |

Conclusion: **OEMCrypto v12 or v13** (consistent with September 2017 build date).

### L1 chain of trust

For Widevine L1 to report correctly, the **entire chain** must be intact:

```
Netflix/YouTube app
      │
MediaDrm API (Android framework)
      │
libwvdrmengine.so (Widevine CDM plugin)
      │
liboemcrypto.so (OEMCrypto HAL, userspace) ← links to TLK via /dev/tlk_device
      │  ioctl → kernel tlk driver → SMC
oemcrypto_secure_service (TA inside TLK, secure world)
      │
EKS partition (per-device Widevine keybox, encrypted with hardware key)
```

If **any** component is missing → fallback to L3 (software-only).

### Common cause of L3 on custom ROMs

The most likely cause is **missing `liboemcrypto.so`** in the build:

- `proprietary_vendor_nvidia/shield/widevine/lib/liboemcrypto.so` (42KB)
  exists in the NVIDIA vendor repo and is included via `widevine.mk`
- `android_vendor_xiaomi_mocha/` does **not** contain `liboemcrypto.so`
- Custom ROMs using only the Xiaomi vendor tree will lack this blob

Dependencies of `liboemcrypto.so`:
```
liboemcrypto.so
  → libnvavp.so               (NVIDIA audio/video processor)
  → libnvos.so                (NVIDIA OS abstraction)
  → libnvrm.so                (NVIDIA resource manager)
  → libtlk_secure_hdcp_up.so  (TLK secure interface)
  → liblog.so, libc++.so, libdl.so, libc.so, libm.so
```

All dependencies are present in both vendor trees.

Other causes of L3:
- `tlk_daemon` not running (no init.rc entry or SELinux denial)
- `/dev/tlk_device` missing (kernel `CONFIG_OTE_PROTOCOL` not enabled)
- EKS partition empty or wiped (keybox invalid)
- Keybox revoked by Google

### EKS (Encrypted Key Storage)

The EKS partition (~80KB) contains encrypted Widevine and HDCP keyboxes.

**Encryption:** Hardware-derived key via Security Engine (reads SBK/SSK from
fuses). The same `eks.dat` only works on the device it was dumped from.

**Format:** NVIDIA blob with indexed keybox entries. TLK reads EKS through
`tlk_daemon`, decrypts via SE hardware.

**Dump and preserve** (from running device with root):
```bash
adb shell "su -c 'dd if=/dev/block/platform/sdhci-tegra.3/by-name/EKS of=/sdcard/eks.dat'"
adb pull /sdcard/eks.dat
```

**Portability:**

| Scenario | Works? |
|----------|--------|
| Same device, different ROM | Yes — keys tied to fuses, not firmware |
| Different MiPad (different chip) | No — different fuse values |
| After full eMMC wipe | Yes — if written back to same partition |

**Without EKS:**

| Component | Status |
|-----------|--------|
| TLK boot | Works |
| PSCI / CPU management | Works |
| Android Keymaster (hwkeystore) | Works — does not depend on EKS |
| Gatekeeper | Works |
| Disk encryption (FDE/FBE) | Works — uses hwkeystore |
| Widevine L1 | **Fails** → L3 |
| HDCP | **Fails** |

---

## Source References

### Kernel (SmokeR24.1-kernel)
- `arch/arm/mach-tegra/sleep-t30.S` — **Primary reference** for LP0/LP1/LP2 sleep code, T124-complete with all ifdefs
- `arch/arm/mach-tegra/sleep.S` — Common sleep helpers (cache flush, MMU off)
- `arch/arm/mach-tegra/sleep.h` — Macros: `cpu_to_halt_reg`, `cpu_to_csr_reg`, timing helpers

### U-Boot (u-boot-mocha)
- `arch/arm/mach-tegra/psci.S` — Tegra124 PSCI: `cpu_on`, `cpu_off` (only functions implemented)
- `arch/arm/mach-tegra/ap.c` — MC security carveout setup, SMMU enable
- `arch/arm/mach-tegra/pmc.c` — `reset_cpu()` — PMC system reset (reusable for `psci_system_reset`)
- `arch/arm/cpu/armv7/psci.S` — Generic ARMv7 PSCI SMC dispatch table (all 0.1/0.2/1.0 FIDs registered)
- `arch/arm/cpu/armv7/virt-v7.c` — Secure → non-secure transition, GIC setup
- `arch/arm/mach-stm32mp/psci.c` — Reference: full PSCI 1.0 implementation for dual-core ARM (~200 lines)
- `include/configs/mocha.h` — TZDRAM configuration (`CONFIG_ARMV7_SECURE_BASE`)
- `arch/arm/include/asm/psci.h` — All PSCI function IDs and return codes

### JXD vendor source (vendor_nvidia_jxd_src)
- `tegra/core/system/fastboot/nvbllp0/sleep.S` — Bootloader LP0 entry/resume (T30 paths only, not T124-safe)
- `tegra/core/system/fastboot/nvbllp0/nvbl_lp0.c` — C-level LP0 management (PMC, pinmux, PMIC) with T124 ifdefs
- `tegra/core/system/nvaboot/nvaboot_warmboot_avp_t124.S` — AVP warmboot: SDRAM reinit after LP0 (T124-specific)
- `tegra/ote/daemon/tlk_daemon.c` — TLK file I/O daemon (MIT license)
- `tegra/ote/storage/hwkeystore/task/hwkeystore_task.c` — Keymaster TA source
- `tegra/core/include/nvsecureservices.h` — SecureImageHeader struct
- `tegra/hwinc/t12x/aremc.h` — EMC register definitions for T124
- `tegra/hwinc/t12x/arse.h` — Security Engine register definitions
- `tegra/hwinc/t12x/arapb_misc_secure_regs.h` — Security register definitions
- `tegra/hwinc/t12x/armc.h` — Memory Controller registers
- `tegra/odm/ardbeg/nvflash/android_fastboot_nvtboot_dtb_emmc_full.cfg` — Partition layout

### Proprietary blobs (proprietary_vendor_nvidia)
- `shield/security/bin32/tlk_daemon` — Prebuilt TLK daemon
- `shield/security/lib/libtlk_secure_hdcp_up.so` — TLK HDCP interface
- `shield/widevine/lib/liboemcrypto.so` — OEMCrypto HAL (42KB, required for Widevine L1)
- `shield/widevine/lib/mediadrm/libwvdrmengine.so` — Widevine CDM plugin
- `shield/widevine/widevine.mk` — Build integration (PRODUCT_PACKAGES += liboemcrypto)

### TOS image
- Dumped from MiPad TOS partition (September 2017 build)
- Compiled with `Android clang version 3.8.256229 (based on LLVM 3.8.256229)`
