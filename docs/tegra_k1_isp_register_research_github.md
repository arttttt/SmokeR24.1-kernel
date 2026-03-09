# Tegra K1 (T124) ISP Register Documentation - GitHub Research Report

**Research Date:** 2026-03-09
**Researcher:** Claude Code Agent
**Scope:** GitHub search for Tegra ISP register maps, headers, and reverse engineering efforts

---

## Executive Summary

Extensive GitHub search revealed important ISP-related code in multiple Tegra kernel repositories. **No public register-level documentation** for ISP internals was found - NVIDIA has kept this information proprietary. However, key architectural information about ISP base addresses, module IDs, syncpoints, and host1x integration was discovered.

---

## 1. CRITICAL FINDING: ISP Base Addresses

**Source:** `arch/arm/mach-tegra/iomap.h` (Multiple kernel repos)

### T124 (Tegra K1) ISP Base Address:
```c
#if defined(CONFIG_ARCH_TEGRA_2x_SOC) || \
    defined(CONFIG_ARCH_TEGRA_3x_SOC) || \
    defined(CONFIG_ARCH_TEGRA_11x_SOC) || \
    defined(CONFIG_ARCH_TEGRA_14x_SOC)
#define TEGRA_ISP_BASE		0x54100000
#define TEGRA_ISP_SIZE		SZ_256K  /* 0x40000 bytes */
#else
#define TEGRA_ISP_BASE		0x54600000  /* T210+ */
#define TEGRA_ISP_SIZE		SZ_256K

#define TEGRA_ISPB_BASE		0x54680000  /* Second ISP instance */
#define TEGRA_ISPB_SIZE		SZ_256K
#endif
```

**Key Finding:** T124 (Tegra K1) uses **0x54100000** as ISP base address with a **256KB register aperture**.

This is different from T210 (Jetson TX1) which has dual ISPs at 0x54600000 and 0x54680000.

---

## 2. Key Repositories with ISP Code

### Primary T124 Kernel Repositories:

| Repository | URL | Relevance |
|------------|-----|-----------|
| HighwayStar/android_kernel_xiaomi_mocha | https://github.com/HighwayStar/android_kernel_xiaomi_mocha | **Xiaomi Mi Pad (T124) kernel - primary target** |
| antmicro/linux-tk1 | https://github.com/antmicro/linux-tk1 | Jetson TK1 kernel (same T124 SoC) |
| Dronevery/JetsonTK1-kernel | https://github.com/Dronevery/JetsonTK1-kernel | Another TK1 kernel reference |
| OE4T/linux-tegra-4.9 | https://github.com/OE4T/linux-tegra-4.9 | Open Embedded for Tegra (newer) |
| arttttt/SmokeR24.1-kernel | https://github.com/arttttt/SmokeR24.1-kernel | **Our own kernel** |

### ISP Driver Files Located:

```
drivers/video/tegra/host/isp/
├── isp.c          # Main ISP driver (13-17KB)
├── isp.h          # ISP header definitions
├── isp_isr_v1.c   # ISR version 1 implementation
└── isp_isr_v1.h   # ISR version 1 header
```

**File URLs:**
- https://github.com/HighwayStar/android_kernel_xiaomi_mocha/blob/3a331b14caf0394a4b25365cbc2c9b237393ff9c/drivers/video/tegra/host/isp/isp.c
- https://github.com/HighwayStar/android_kernel_xiaomi_mocha/blob/3a331b14caf0394a4b25365cbc2c9b237393ff9c/drivers/video/tegra/host/isp/isp.h

---

## 3. ISP Syncpoints (Hardware Synchronization)

**Source:** `drivers/video/tegra/host/t124/t124.c`

```c
static const char *s_syncpt_names[NV_HOST1X_SYNCPT_NB_PTS] = {
    [NVSYNCPT_ISP_0_0]	= "ispa_memory",
    [NVSYNCPT_ISP_0_1]	= "ispa_stats",
    [NVSYNCPT_ISP_0_2]	= "ispa_stream",
    [NVSYNCPT_ISP_0_3]	= "ispa_loadv",
    [NVSYNCPT_ISP_1_0]	= "ispb_memory",
    [NVSYNCPT_ISP_1_1]	= "ispb_stats",
    [NVSYNCPT_ISP_1_2]	= "ispb_stream",
    [NVSYNCPT_ISP_1_3]	= "ispb_loadv",
    [NVSYNCPT_VI_0_0]	= "vi0_ispa",
    [NVSYNCPT_VI_0_1]	= "vi0_ispb",
    ...
};
```

**ISP Syncpoint Definitions (8 total per dual-ISP):**
- `NVSYNCPT_ISP_0_0` - ISPA memory operations
- `NVSYNCPT_ISP_0_1` - ISPA statistics
- `NVSYNCPT_ISP_0_2` - ISPA stream processing
- `NVSYNCPT_ISP_0_3` - ISPA load vector
- `NVSYNCPT_ISP_1_0` to `NVSYNCPT_ISP_1_3` - ISPB equivalents

**VI to ISP Syncpoints:**
- `NVSYNCPT_VI_0_0` = "vi0_ispa" - VI channel 0 to ISPA
- `NVSYNCPT_VI_0_1` = "vi0_ispb" - VI channel 0 to ISPB

---

## 4. ISP Device Configuration (t124.c)

```c
static struct resource isp_resources[] = {
    {
        .name = "regs",
        .start = TEGRA_ISP_BASE,        /* 0x54100000 */
        .end = TEGRA_ISP_BASE + TEGRA_ISP_SIZE - 1,
        .flags = IORESOURCE_MEM,
    }
};

struct nvhost_device_data t124_isp_info = {
    .syncpts         = NV_ISP_0_SYNCPTS,
    .moduleid        = NVHOST_MODULE_ISP,
    .modulemutexes   = {NVMODMUTEX_ISP_0},
    .exclusive       = true,
    .keepalive       = true,
    .powergate_ids   = {TEGRA_POWERGATE_VENC, -1},
    .can_powergate   = true,
    .clockgate_delay = ISP_CLOCKGATE_DELAY,  /* 60 */
    .powergate_delay = ISP_POWERGATE_DELAY,  /* 500 */
    .clocks          = {
        {"isp", UINT_MAX, 0, TEGRA_MC_CLIENT_ISP},
        {"emc", 0, TEGRA_HOST1X_EMC_MODULE_ID},
        {"sclk", 80000000}
    },
    .finalize_poweron = nvhost_isp_t124_finalize_poweron,
    .ctrl_ops         = &tegra_isp_ctrl_ops,
    .alloc_hwctx_handler = nvhost_alloc_hwctx_handler,
};
```

---

## 5. Host1x Class IDs (class_ids.h)

**Important Finding:** ISP does NOT appear in host1x class IDs!

```c
enum {
    NV_HOST1X_CLASS_ID			= 0x1,
    NV_VIDEO_ENCODE_MPEG_CLASS_ID	= 0x20,
    NV_VIDEO_ENCODE_MSENC_CLASS_ID	= 0x21,
    NV_GRAPHICS_3D_CLASS_ID		= 0x60,
    NV_GRAPHICS_GPU_CLASS_ID		= 0x61,
    NV_GRAPHICS_VIC_CLASS_ID		= 0x5D,
    NV_TSEC_CLASS_ID			= 0xE0,
};
```

**This indicates ISP is controlled differently** - likely through direct register access rather than host1x command streams like the 3D engine or VIC.

---

## 6. Hardware Register Definitions (hardware_t124.h)

**Source:** `drivers/video/tegra/host/t124/hardware_t124.h`

Host1x register operation macros for communicating with engines:

```c
/* cdma opcodes */
static inline u32 nvhost_opcode_setclass(
    unsigned class_id, unsigned offset, unsigned mask)
{
    return (0 << 28) | (offset << 16) | (class_id << 6) | mask;
}

static inline u32 nvhost_opcode_incr(unsigned offset, unsigned count)
{
    return (1 << 28) | (offset << 16) | count;
}

static inline u32 nvhost_opcode_nonincr(unsigned offset, unsigned count)
{
    return (2 << 28) | (offset << 16) | count;
}

static inline u32 nvhost_opcode_imm(unsigned offset, unsigned value)
{
    return (4 << 28) | (offset << 16) | value;
}
```

**Syncpoint operations:**
```c
static inline u32 nvhost_class_host_incr_syncpt(
    unsigned cond, unsigned indx)
{
    return host1x_uclass_incr_syncpt_cond_f(cond)
        | host1x_uclass_incr_syncpt_indx_f(indx);
}
```

---

## 7. Media Controller Common Header (mc_common.h)

**Key ISP-related definitions:**

```c
#define TEGRA_MEM_FORMAT 0
#define TEGRA_ISP_FORMAT 1
```

**Tegra Channel Structure fields:**
```c
struct tegra_channel {
    // ...
    void __iomem *csibase[TEGRA_CSI_BLOCKS];
    bool bypass;
    bool write_ispformat;
    // ...
};
```

---

## 8. VI (Video Input) Base Address

```c
#define TEGRA_VI_BASE		0x54080000
#define TEGRA_VI_SIZE		SZ_256K
```

**VI is at 0x54080000, ISP is at 0x54100000** - they are 512KB apart in address space.

---

## 9. What Was NOT Found (Disappointing)

### No Public ISP Register Documentation:
1. **No ISP register offset definitions** - no `ISP_CTRL`, `ISP_CONFIG`, etc.
2. **No ISP programming sequence** documented
3. **No ISP command formats**
4. **No ISP class ID** - suggesting direct MMIO access rather than host1x commands
5. **No leaked NVIDIA TRM** with ISP chapter

### Likely Reasons:
- ISP is a **highly proprietary** NVIDIA IP block
- ISP programming is done through **firmware blobs** (libnvisp_v3.so)
- NVIDIA never released ISP documentation publicly
- ISP registers are accessed via **direct MMIO** not host1x command stream

---

## 10. Additional Related Register Regions

For context, here are other T124 HOST1X region addresses:

```c
#define TEGRA_HOST1X_BASE       0x50000000  /* Host1x controller */
#define TEGRA_MPE_BASE          0x54040000  /* Video encoder */
#define TEGRA_VI_BASE           0x54080000  /* Video input */
#define TEGRA_ISP_BASE          0x54100000  /* Image Signal Processor */
#define TEGRA_GR2D_BASE         0x54140000  /* 2D graphics */
#define TEGRA_GR3D_BASE         0x54180000  /* 3D graphics */
#define TEGRA_DISPLAY_BASE      0x54200000  /* Display controller */
#define TEGRA_DISPLAY2_BASE     0x54240000  /* Display 2 */
#define TEGRA_HDMI_BASE         0x54280000  /* HDMI */
#define TEGRA_MSENC_BASE        0x544c0000  /* MSENC encoder */
```

---

## 11. Conclusions and Recommendations

### What We Know:
1. **ISP Physical Base:** 0x54100000 (256KB aperture)
2. **ISP Size:** 0x40000 bytes (256KB)
3. **Syncpoints:** 4 per ISP instance (memory, stats, stream, loadv)
4. **VI→ISP connection:** Via syncpoints and memory buffers
5. **Power domain:** TEGRA_POWERGATE_VENC
6. **Clocks:** "isp", "emc", "sclk"

### What We Don't Know:
1. **Internal ISP register offsets**
2. **ISP programming sequence**
3. **ISP command/format structures**

### Recommended Next Steps:
1. **Dump ISP register space** at 0x54100000 during active camera operation
2. **Analyze libnvisp_v3.so** disassembly for register access patterns
3. **Look at camera blob register writes** for ISP configuration sequences
4. **Trace kernel ISP driver** (isp.c) to understand initialization sequence
5. **Search for leaked NVIDIA TRM** documents on other platforms

---

## 12. File References for Further Investigation

### Key Files to Analyze:
```
drivers/video/tegra/host/isp/isp.c          - ISP driver implementation
drivers/video/tegra/host/isp/isp.h          - ISP header
drivers/video/tegra/host/t124/t124.c        - T124 device configuration
arch/arm/mach-tegra/iomap.h                 - Physical address map
include/media/mc_common.h                   - Media controller structures
```

### Register Dump Utility Location:
Consider creating a register dump utility that reads from:
- `/dev/mem` at offset 0x54100000
- Via kernel module using `ioremap(0x54100000, 0x40000)`

---

## Appendix: Search Queries Used

```
"tegra ISP register" language:c
tegra124 isp language:c
nvisp register
nvhost-isp register offset
TEGRA_ISP define
T124_ISP OR t124_isp
tegra_isp_reg
TEGRA_ISP_BASE
TEGRA_HOST1X_CLASS_ISP
NVHOST_MODULE_ISP
```

---

*Report generated by Claude Code Agent for SmokeR24.1-kernel project*
