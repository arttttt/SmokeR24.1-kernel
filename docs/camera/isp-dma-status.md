# ISP DMA Debug — Complete Status (2026-04-03)

## Current State

ISP DMA works on stock Smoke kernel 1.2 but NOT on SmokeR24.1 (24.1).
Same userspace test, same calibration, same cmdbuf format.

## What Works on 24.1

- ISP power on/off, clock gating
- Host1x channel allocation, syncpoint
- Ping (IMM_INCR_SYNCPT) — OK
- Gather filter disabled for ISP — no bad opcode
- SMMU fixup — ISP attached to SMMU, no MC faults
- Pushbuffer format identical to stock
- All ISP MMIO registers written correctly (confirmed via devmem)
- ISP transitions to streaming state (0x00C=0x04)
- SMMU mapping verified — DMA != physical for all buffers:
  - Gather cmdbuf: host1x IOVA 0x80078000 → phys 0xa6c74000
  - Input: ISP IOVA 0x82100000 → phys 0xa6c7c000 (65 scatter entries)
  - Output: ISP IOVA 0x83100000 → phys 0xa61f7000 (4 entries)
  - Work: ISP IOVA 0x83080000 → phys 0xa55ae000

## What Does NOT Work

- ISP DMA processing — output always zeros
- Timing: 24.1 ~100-600µs vs stock ~1-20ms
- ISP never starts DMA engine despite accepting all methods

## Verified on Stock (without camera HAL)

- ISP DMA works on cold boot without NvCameraIspInitialize
- Stock kernel has same pushbuffer format (SET_CLASS + GATHER)
- Stock reloc addresses: phys 0x82200000 (out), 0x81200000 (in)
- Stock timing: ~1000µs first submit, ~20ms after warmup

## Investigated and Ruled Out

| What | Result |
|------|--------|
| Gather filter | Disabled for ISP, no bad opcode |
| SMMU fixup for ISP | Added, working |
| HC SMMU (host1x behind SMMU) | Removed — no change. Restored — same result |
| ISP iommus in DTS | Removed — no change (fixup table still enables SMMU) |
| ISP SMMU disabled via devmem | Broke ping (pushbuffer IOVA invalid without SMMU) |
| pdata->class (0 vs 0x32) | Both tested, no difference |
| Pushbuffer SET_CLASS | Stock also has it |
| Channel aperture | Identical math on both kernels |
| CDMA init (DMASTART/END/PUT) | Identical registers |
| nvhost_vm | No-op on T124 |
| hwctx save/restore | NOP on stock ISP |
| Cmdbuf contiguity | Carveout tested |
| Buffer allocation device | host1x vs ISP tested |
| All stock MMIO register values | Programmed via cmdbuf |
| Dual/triple trigger sequences | 0x0F→0x05→0x05 |
| 6-gather stock layout with WAIT_SYNCPT | Tested |
| Camera HAL pre-init | Stock works WITHOUT it |
| Physical address hack (all via host1x) | ISP SMMU still on → zeros |
| SMMU page table validity | Pin debug confirms DMA→phys translation works |

## Pin Debug Data (24.1)

```
host1x: pin id=1024 dma=0x80078000 phys=0xa6c74000 nents=1 dev=host1x  ← gather
isp.0:  pin id=1027 dma=0x83080000 phys=0xa55ae000 nents=1 dev=isp.0   ← work (reloc)
isp.0:  pin id=1025 dma=0x82100000 phys=0xa6c7c000 nents=65 dev=isp.0  ← input (reloc)
isp.0:  pin id=1026 dma=0x83100000 phys=0xa61f7000 nents=4 dev=isp.0   ← output (reloc)
```

## Stock Debug Data

```
stock reloc: off=0x1874 phys=0x82200000+0x0=0x82200000      ← out_Y
stock reloc: off=0x18c4 phys=0x81200000+0x0=0x81200000      ← input
stock reloc: off=0x1894 phys=0x82200000+0x9c2700=0x82bc2700  ← out_V
stock reloc: off=0x1884 phys=0x82200000+0x7cec00=0x829cec00  ← out_U
stock PB: 00000c80 20000000 → PB_G: 6000063a 80078000       ← gather
```

## Key Observations

1. Stock reloc "phys" addresses (0x82200000) are actually IOVA too (SMMU enabled on stock)
2. 24.1 ISP IOVA range (0x82-0x83xxxxxx) similar to stock
3. Host1x gather IOVA (0x80078000) same on both kernels
4. Physical addresses differ (24.1: 0xa6c74000, stock: 0x80078000 was likely identity-mapped)
5. SMMU mapping verified working — ISP SMMU enabled (MC_SMMU_ISP2_ASID_0 = 0x80000000)

## Remaining Hypotheses

1. **tegra-smmu.c page table programming** — 24.1 SMMU driver may create mapping in software
   but not flush TLB or program hardware page tables correctly
2. **SMMU ASID mismatch** — ISP and host1x may use different ASIDs that don't share mappings,
   causing ISP to see different address space than what kernel configured
3. **Power domain interaction** — 24.1 generic PM domains may affect ISP state differently
4. **host1x method dispatch** — CDMA reads gather but host1x doesn't route methods to ISP
   (class dispatch broken on 24.1)
5. **ISP internal state** — 24.1 power-on sequence leaves ISP in different state than stock

## Files

| File | Purpose |
|------|---------|
| `drivers/media/platform/tegra/camera/isp_t124.c` | ISP MC driver + kernel dma_test |
| `drivers/media/platform/tegra/camera/isp_t124.h` | ISP defines |
| `drivers/media/platform/tegra/camera/isp_t124_cal.h` | Stock calibration data |
| `tools/camera/isp_dma_test.c` | Userspace ISP DMA test |
| `tools/camera/isp_test_24.c` | Adapted stock isp_test for 24.1 |
| `tools/camera/isp_test_raw.c` | RAW file → ISP processing |
| `tools/camera/isp_test.c` | Original stock isp_test |
| `docs/camera/isp-dma-debug-24.1.md` | Full debug data dump |
| `docs/camera/stock-debug-dmesg.txt` | Stock kernel dmesg with ISP debug |
| `docs/camera/camera-isp-reverse-engineering.md` | Main ISP RE document |
