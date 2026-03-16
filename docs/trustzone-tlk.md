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

## Source References

### In this project
- `arch/arm/mach-tegra/psci.S` — U-Boot PSCI for Tegra (no TLK)
- `arch/arm/mach-tegra/ap.c` — MC security carveout setup
- `arch/arm/cpu/armv7/psci.S` — Generic ARMv7 PSCI / SMC handler
- `arch/arm/cpu/armv7/virt-v7.c` — Secure -> non-secure transition

### JXD vendor source (vendor_nvidia_jxd_src)
- `tegra/ote/daemon/tlk_daemon.c` — TLK file I/O daemon (MIT license)
- `tegra/ote/storage/hwkeystore/task/hwkeystore_task.c` — Keymaster TA source
- `tegra/core/include/nvsecureservices.h` — SecureImageHeader struct
- `tegra/hwinc/t12x/arapb_misc_secure_regs.h` — Security register definitions
- `tegra/hwinc/t12x/armc.h` — Memory Controller registers
- `tegra/odm/ardbeg/nvflash/android_fastboot_nvtboot_dtb_emmc_full.cfg` — Partition layout

### Proprietary blobs
- `proprietary_vendor_nvidia/shield/security/bin32/tlk_daemon` — Prebuilt daemon
- `proprietary_vendor_nvidia/shield/security/lib/libtlk_secure_hdcp_up.so` — HDCP lib

### TOS image
- Dumped from MiPad TOS partition (September 2017 build)
- Compiled with `Android clang version 3.8.256229 (based on LLVM 3.8.256229)`
