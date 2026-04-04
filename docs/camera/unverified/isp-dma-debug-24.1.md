# ISP DMA Debug Data — SmokeR24.1 Kernel

## Collected: 2026-04-03

---

## 1. Pushbuffer Dump (from dmesg PB/PB_G)

### Without pushbuffer SET_CLASS (pdata->class=0, matching stock):

```
# Ping (2 words)
PB_G: 60000002 80078000 (iova=0x80078000 off=0)

# Init submit (1587 words = cal 1544 + init regs + trigger 0x0F + syncpt)
PB_G: 60000633 80078000 (iova=0x80078000 off=0)

# Frame submit (54 words = output + surfaces + input + syncpt incrs + trigger 0x05 + syncpt)
PB_G: 60000036 80078000 (iova=0x80078000 off=0)

# Frame 2 submit (same)
PB_G: 60000036 80078000 (iova=0x80078000 off=0)

# Combined submit (1639 words)
PB_G: 60000667 80078000 (iova=0x80078000 off=0)

# 6-gather submit:
PB_G: 60000002 80078000 (iova=0x80078000 off=0)     # G1: syncpt incr
PB_G: 6000002c 80078008 (iova=0x80078000 off=8)     # G2: output+trigger 0x05
PB_G: 60000002 800780b8 (iova=0x80078000 off=184)   # G3: syncpt incr
PB_G: 60000002 800780c0 (iova=0x80078000 off=192)   # G4: WAIT_SYNCPT (was NOOP)
PB_G: 60000002 800780c8 (iova=0x80078000 off=200)   # G5: syncpt incr
PB_G: 6000063b 800780d0 (iova=0x80078000 off=208)   # G6: cal+init regs+0x0F
```

### With pushbuffer SET_CLASS (earlier, pdata->class=0x32):
```
PB: 00000c80 20000000              # SET_CLASS(0x32) + NOOP
PB_G: 00000c80 20000000 (off=0)   # (from nvhost_cdma_push → push_gather)
PB_G: 60000002 80078000 (off=0)   # GATHER(2) @ 0x80078000
```

---

## 2. Gather Physical Address

- cmdbuf nvmap handle pinned through host1x device
- Carveout allocation (physically contiguous): `0x80078000`
- Same address for all submits (same nvmap handle reused)

---

## 3. Cmdbuf Content Dump (from userspace isp_dma_test output)

### Calibration start (first 32 words of combined submit):
```
[000] 0x00000c80  SET_CLASS(0x32, 0, 0)
[001] 0x1d00000a  INCR(0xD00, 10) — lens shading control
[002] 0x00000001
[003] 0x009fd800
[004] 0x004fec00
[005] 0x009fd800
[006] 0x00d4c770
[007] 0x006a9000
[008] 0x00d578e0
[009] 0x06680334
[010] 0x04ce0268
[011] 0x00000021
[012] 0x1d0a0001  INCR(0xD0A, 1) — lens shading enable
[013] 0x00000000
[014] 0x2d0b01e0  NONINCR(0xD0B, 480) — lens shading FIFO
[015] 0x0002e028
[016] 0x0002aca8
[017] 0x0002b518
[018] 0x00028058
[019] 0x000257c0
[020] 0x000232b8
[021] 0x00023730
[022] 0x00020f50
[023] 0x00027668
[024] 0x00024b60
[025] 0x00025740
[026] 0x000225e8
[027] 0x00024d70
[028] 0x000226a0
[029] 0x00023118
[030] 0x00020260
[031] 0x00024d70
```

### Frame region (offset 6264 in combined submit, after calibration):
```
[000] 0x00000c80  SET_CLASS(0x32)
[001] 0x1e000001  INCR(0xE00, 1) — output width
[002] 0x0ccf0000  (3279 << 16)
[003] 0x1e010001  INCR(0xE01, 1) — output height
[004] 0x099b0000  (2459 << 16)
[005] 0x1e020001  INCR(0xE02, 1) — output format
[006] 0x04fe00e6
[007] 0x1e030001  INCR(0xE03, 1) — color params
[008] 0x00000000
[009] 0x1e040003  INCR(0xE04, 3) — output surface Y
[010] 0x83100000  out_Y IOVA (reloc patched)
[011] 0x00000000
[012] 0x00000d00  stride 3328
[013] 0x1e070003  INCR(0xE07, 3) — output surface U
[014] 0x838cec00  out_U IOVA
[015] 0x00000000
[016] 0x00000680  stride 1664
[017] 0x1e0a0003  INCR(0xE0A, 3) — output surface V
[018] 0x83ac2700  out_V IOVA
[019] 0x00000000
[020] 0x00000680  stride 1664
[021] 0x15000006  INCR(0x500, 6) — processing
[022] 0x00000000
[023] 0x00000000
[024] 0x00000000
[025] 0x00000000
[026] 0x00000000
[027] 0x099c0cd0  (2460 << 16) | 3280
[028] 0x00000c80  SET_CLASS(0x32)
[029] 0x11000004  INCR(0x100, 4) — input buffer
[030] 0x82100000  in IOVA (reloc patched)
[031] 0x00000000
```

### Init regs (G6 tail, after calibration):
```
[028] 0x10530002  INCR(0x053, 2) — ISP enable + work buf
[029] 0x00000001  ISP enable
[030] 0x83080000  work_buf IOVA (reloc patched)
[031] 0x00000c80  SET_CLASS(0x32)
[032] 0x10080001  INCR(0x008, 1) = 0xF000F800
[033] 0xf000f800
[034] 0x100d0001  INCR(0x00D, 1) = 0x00000100
[035] 0x00000100
[036] 0x10140001  INCR(0x014, 1) = 0x00000339
[037] 0x00000339
[038] 0x10150001  INCR(0x015, 1) = 0x04040007
[039] 0x04040007
[040] 0x10180005  INCR(0x018, 5)
[041] 0x0a00500a  0x018
[042] 0x00008089  0x019
[043] 0x013645cb  0x01A
[044] 0x000001e7  0x01B
[045] 0x00000001  0x01C
[046] 0x101d0001  INCR(0x01D, 1) = 0x00000001
[047] 0x00000001
[048] 0x101f0001  INCR(0x01F, 1) = 0x00000001
[049] 0x00000001
[050] 0x10240003  INCR(0x024, 3)
[051] 0xc6bff67c  0x024
[052] 0x70c9a9ea  0x025
[053] 0x33894d2b  0x026
[054] 0x10280003  INCR(0x028, 3)
[055] 0x00000007  0x028
[056] 0x00000007  0x029
[057] 0x00000007  0x02A
[058] 0x10380001  INCR(0x038, 1) = 0x242CB07B
[059] 0x242cb07b
[060] 0x103b0001  INCR(0x03B, 1) = 0x017BAD37
[061] 0x017bad37
[062] 0x103f0001  INCR(0x03F, 1) = 0x00000020
[063] 0x00000020
```

---

## 4. MMIO Readback (after init submit, ISP powered)

### 24.1 after our init submit:
```
008 = 0xF000F800   ← matches stock
00c = 0x00000004   ← streaming state, matches stock
00d = 0x00000100   ← matches stock
014 = 0x00000000   ← NOT written! (stock=0x339, we write 0x339 but readback=0)
015 = 0x04040007   ← matches stock
018 = 0x0A00500A   ← matches stock
019 = 0x00008089   ← matches stock
01a = 0x013645CB   ← matches stock
01b = 0x000001E7   ← matches stock
01c = 0x00000001   ← matches stock
01d = 0x00000001   ← matches stock
01f = 0x00000003   ← DIFFERS! stock=0x01, we write 0x01 but readback=0x03
024 = (not present) ← NOT written! stock=0xC6BFF67C
025 = (not present) ← NOT written! stock=0x70C9A9EA
026 = (not present) ← NOT written! stock=0x33894D2B
028 = (not present) ← NOT written! stock=0x07
029 = (not present) ← NOT written! stock=0x07
02a = (not present) ← NOT written! stock=0x07
038 = 0x242CB07B   ← hardware default (same before and after our submit)
03b = 0x017BAD37   ← hardware default
03f = 0x00000020   ← written OK
051 = 0x017BA537   ← hardware default
053 = 0x00000001   ← ISP enable, OK
054 = 0x83080000   ← work buf addr, OK
05e = 0x00003232   ← OK
05f = 0x00000010   ← OK
```

### Stock MMIO (during camera streaming, from raw dump):
```
008 = 0xF000F800
00c = 0x00000004
00d = 0x00000100
014 = 0x00000339
015 = 0x04040007
018 = 0x0A00500A
019 = 0x00008089
01a = 0x013645CB
01b = 0x000001E7
01c = 0x00000001
01d = 0x00000001
01f = 0x00000001
024 = 0xC6BFF67C
025 = 0x70C9A9EA
026 = 0x33894D2B
028 = 0x00000007
029 = 0x00000007
02a = 0x00000007
038 = 0x242CB07B
03b = 0x017BAD37
03f = 0x00000020
051 = 0x017BA537
053 = 0x00000001
054 = 0x00585B18
05e = 0x00003232
05f = 0x00000010
```

### MMIO differences (24.1 vs stock):
```
0x014: 0x00000000 vs 0x00000339  ← write via cmdbuf doesn't stick
0x01F: 0x00000003 vs 0x00000001  ← write via cmdbuf doesn't stick
0x024: missing    vs 0xC6BFF67C  ← write via cmdbuf doesn't stick
0x025: missing    vs 0x70C9A9EA  ← write via cmdbuf doesn't stick
0x026: missing    vs 0x33894D2B  ← write via cmdbuf doesn't stick
0x028: missing    vs 0x00000007  ← write via cmdbuf doesn't stick
0x029: missing    vs 0x00000007  ← write via cmdbuf doesn't stick
0x02A: missing    vs 0x00000007  ← write via cmdbuf doesn't stick
```

NOTE: These registers also don't write via direct MMIO writel (tested).
They may be read-only on 24.1, or written by camera HAL on stock (not by isp_test).
Stock isp_test worked WITHOUT writing these (cold boot, no camera).

---

## 5. Timing

### Userspace test (carveout cmdbuf, relocs, class_id=0):
```
Ping:      88-156 us
Init 0x0F: 98-140 us  (1587 words)
Frame 0x05: 102-183 us (54 words)
Frame 2:    118-202 us
Combined:   81-262 us  (1639 words)
6-gather:   154-460 us (1653 words total)
```

### Kernel dma_test (host1x device cmdbuf):
```
Phase 1 0x0F: 151 us
Phase 2 0x05: 171 us
Phase 3 0x05: 109 us
```

### All outputs: 0 bytes changed from sentinel

---

## 6. Buffer Addresses

```
Userspace (nvmap pin IOVAs):
  in:   0x82100000 (or 0x83d00000)
  out:  0x83100000 (or 0x84d00000)
  work: 0x83080000
  cmdbuf: 0x80078000 (carveout, physical)

Kernel (dma_alloc_coherent):
  in:   0x82100000
  out:  0x83100000
  work: 0x83080000
  cmdbuf: 0x8005a000 (via host1x device, physical)
```

---

## 7. Errors

- No MC errors (mcerr table empty)
- No SMMU faults
- No bad opcode (gather filter disabled for ISP)
- CDMA stalls on method 0xD0B (lens shading FIFO) in 6-gather
  after trigger 0x05 — fixed with WAIT_SYNCPT in G4
- cdma_timeout from MMIO readback after ISP power-gated (cosmetic)

---

## 8. Kernel Config

```
pdata->class = 0 (removed, matching stock)
pdata->resource_policy = RESOURCE_PER_DEVICE (default 0)
pdata->isolate_contexts = false
pdata->serialize = false
pdata->push_work_done = false
pdata->modulemutexes = {NVMODMUTEX_ISP_0}
Gather filter: DISABLED for ISP (read-modify-write channelctrl bit 2)
SMMU fixup: "isp" → ISP2|ISP2B, "isp.1" → ISP2B
```
