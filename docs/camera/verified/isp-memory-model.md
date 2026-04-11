# ISP T124 Memory Model — IOVA, nvmap, relocs

## Overview

ISP accesses memory through SMMU (IOMMU). All buffer addresses in ISP gathers
must be SMMU-mapped IOVAs, not physical addresses. There are two ways to
provide IOVAs: nvmap handles with relocs (userspace) or nvmap kernel API (kernel).

## Userspace Path (stock HAL via libnvrm.so)

### How stock HAL provides IOVAs

1. **Allocate**: `nvmap_create` + `nvmap_alloc` → nvmap handle
2. **Get IOVA**: `nvmap_pin` → returns SMMU IOVA (e.g. 0x8A400000)
3. **Build gather**: write IOVA placeholder (0) in cmdbuf
4. **Reloc**: add `nvhost_reloc` entry: `{cmdbuf_handle, offset, target_handle, target_offset}`
5. **Submit**: kernel resolves relocs — replaces placeholder with actual IOVA

### Reloc details (from stock trace)

```
RELOC[0] cmdbuf=0x41d+0x1860 -> target=0x483+0x540000 phys=0x8ad00000
```
- `cmdbuf=0x41d+0x1860`: nvmap handle 0x41d, byte offset 0x1860 in cmdbuf
- `target=0x483+0x540000`: nvmap handle 0x483, offset 0x540000 (U plane)
- `phys=0x8ad00000`: resolved IOVA (for tracing only)

Stock submits have 1 reloc per frame (output Y IOVA). U/V offsets are fixed
relative to Y (U = Y + 0x540000). The kernel patches the cmdbuf word at the
reloc offset with the target's IOVA + target_offset.

### Why stock uses relocs

Output buffers change each frame (multi-buffering). Relocs let the HAL
reuse the same gather template and only change which buffer handle to use.
The gather itself doesn't contain the IOVA — the kernel fills it in.

## Kernel Driver Path (isp_t124.c)

### No relocs — direct IOVA in cmdbuf

The kernel driver builds gathers in DMA-coherent memory and writes IOVAs
directly. There are no reloc entries in the submit. This works because:

1. **nvmap kernel API**: `nvmap_alloc` + `nvmap_pin_ids` → dma_addr_t (IOVA)
2. **dma_alloc_coherent**: returns both CPU vaddr and DMA addr (IOVA)
3. The DMA address IS the IOVA when SMMU is enabled

```c
/* Example from isp_t124.c */
cmd[n++] = (u32)out_dma;  /* dma_addr_t = IOVA directly */
```

### Buffer allocation options in kernel

#### Option 1: dma_alloc_coherent (simplest)
```c
void *cpu = dma_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL);
/* dma_addr is SMMU IOVA, cpu is kernel virtual */
/* Works for cmdbuf, stats, work buffer */
/* Problem: may not be compatible with nvmap-backed ISP channel */
```

#### Option 2: nvmap kernel API (recommended for ISP)
```c
struct nvmap_handle_ref *ref;
ref = nvmap_alloc(nvmap_client, size, align, flags, heap_mask);
phys_addr_t iova = nvmap_pin(nvmap_client, ref);
void *cpu = nvmap_mmap(ref);  /* or nvmap_kmap for kernel mapping */
```

ISP channel opened via nvhost expects nvmap handles. The host1x CDMA
engine may validate that gather addresses belong to pinned nvmap handles.
Using dma_alloc_coherent for I/O buffers may cause SMMU faults if the
ISP SMMU context doesn't map those pages.

#### Option 3: nvmap for I/O, dma_alloc for cmdbuf
```c
/* cmdbuf: dma_alloc_coherent (host1x always maps these) */
cmd = dma_alloc_coherent(host1x_dev, size, &cmd_phys, GFP_KERNEL);

/* I/O buffers: nvmap (ISP SMMU context maps these) */
out_ref = nvmap_alloc(client, out_size, 4096, 0, NVMAP_HEAP_IOVMM);
out_iova = nvmap_pin(client, out_ref);
```

### SMMU contexts

Tegra K1 has separate SMMU contexts for different engines:
- **host1x**: maps cmdbufs (gathers)
- **isp**: maps I/O buffers accessed by ISP DMA
- **vi**: maps capture buffers

An nvmap handle pinned to the ISP channel's SMMU context is visible to ISP.
A buffer from dma_alloc_coherent on host1x_dev may NOT be in the ISP context.

### Common pitfalls

1. **Using physical address instead of IOVA**: ISP always goes through SMMU,
   physical addresses cause MC decode errors or silent wrong data.

2. **Buffer not pinned to ISP SMMU**: nvmap_pin must happen with a client
   associated to the ISP device, not a generic client.

3. **Cmdbuf not mapped for host1x**: gathers must be accessible by host1x.
   dma_alloc_coherent on host1x parent device handles this.

4. **U/V stride mismatch**: ISP writes to all 3 surfaces even for RGBA format.
   U/V must point to valid memory with valid stride or MC errors occur.

5. **Reloc vs direct IOVA**: stock HAL uses relocs because it reuses gather
   templates with different buffers. Kernel driver can write IOVAs directly
   since it rebuilds the gather each frame.

## Buffer Sizes (OV5693 2592×1944)

| Buffer | Size | Notes |
|--------|------|-------|
| Y plane | 2624 × 1944 = ~5.1 MB | stride = (W+63)&~63 |
| U plane | 1344 × 972 = ~1.3 MB | stride = (W/2+63)&~63, H/2 |
| V plane | 1344 × 972 = ~1.3 MB | same as U |
| Total YUV420 | ~7.7 MB | Y + U + V |
| RGBA output | 2592 × 4 × 1944 = ~20 MB | for reprocess with 0x43 |
| Stats | 256 KB | AE/AWB/AF statistics |
| Work buffer | 512 KB | ISP internal working memory |
| Cmdbuf | 16-32 KB | host1x gather commands |
| Raw input | 2592 × 2 × 1944 = ~10 MB | BG10 16-bit LE |
