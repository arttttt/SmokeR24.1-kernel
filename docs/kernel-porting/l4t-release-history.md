# NVIDIA L4T (Linux for Tegra) Release History

## Complete Version Table

| L4T Version | JetPack | Kernel | SoCs Supported | Notes |
|-------------|---------|--------|----------------|-------|
| R19.x | — | 3.10 | T124 (TK1) | Initial TK1 support |
| R21.x | JP 1.x/2.x | 3.10 | T124 (TK1) | Last official TK1 BSP |
| R23.x | JP 2.0 | 3.10 | T210 (TX1) | First TX1 support |
| **R24.x** | JP 2.1-2.3.1 | **3.10** | T210 + T124 | **Our working BSP** |
| R27.x | JP 3.0 | **4.4** | T186 (TX2) | **First 4.4 kernel. T124 DROPPED** |
| R28.x | JP 3.1-3.3.4 | 4.4 | T210 + T186 | Unified TX1+TX2 |
| R31.x | JP 4.1.1 | **4.9** | T194 (Xavier) | First Xavier |
| R32.x | JP 4.2-4.6.6 | 4.9 | Xavier + TX2 + TX1 + Nano | Longest-running series |
| R34.x | JP 5.0 DP | **5.10** | Orin + Xavier | Developer Preview |
| R35.x | JP 5.0.1-5.1.6 | 5.10 | Orin + Xavier | Production |
| R36.x | JP 6.0-6.2.2 | **5.15** | Orin | Current (as of 2024) |

---

## Key Transitions

### 3.10 → 4.4 (R24 to R27)
- **Timing:** Between R24 and R27
- **Significance:** First major kernel version jump
- **Impact:** T124 (Tegra K1) support **DROPPED**
- **Work:** First time NVIDIA ported entire driver stack to new kernel

### 4.4 → 4.9 (R28 to R31)
- **Timing:** Between R28 and R31
- **Significance:** Xavier (T194) introduction
- **Impact:** Continued TX1/TX2 support
- **Work:** Moderate API adaptations

### 4.9 → 5.10 (R32 to R34)
- **Timing:** Between R32 and R34
- **Significance:** Orin introduction
- **Impact:** Major restructuring for new architecture
- **Work:** Significant driver rewrites

---

## Kernel Source Repositories

### Official NVIDIA Git (nv-tegra.nvidia.com)

```
linux-3.10.git    # R21/R24 branches
linux-4.4.git     # R27/R28 branches
linux-4.9.git     # R31/R32 branches
linux-5.10.git    # R34/R35 branches
linux-5.15.git    # R36 branches
```

### GitHub Mirrors

| Repository | Description |
|------------|-------------|
| OE4T/linux-tegra-4.9 | R32.x with OpenEmbedded patches |
| NVIDIA/linux-tegra | Official mirror (limited history) |
| NVIDIA/nvgpu | GPU driver (archived June 2021) |
| Various community forks | Re-uploads with full history |

### Important: NVIDIA/nvgpu Repository

- Contains GK20A (Tegra K1) GPU code
- Archived by NVIDIA in June 2021
- Last version supports R32.x series
- Still valuable for reference

---

## Critical Note on R24.1

**R24.1 is NOT an official NVIDIA release for T124.**

### What R24.1 Actually Is
- Primarily a **T210 (Tegra X1)** BSP
- Happens to include **T124 (Tegra K1)** support
- Released for Shield TV and Shield Tablet K1

### Why This Matters

The R24.1 kernel + userspace blobs from Shield/JXD devices represent:
- The **ONLY** working proprietary driver stack for T124 on kernel 3.10
- The **most complete** driver implementation for T124
- The **last** NVIDIA BSP to support T124

**No newer proprietary stack exists for T124.** Once R24.1, always R24.1 for T124 support.

---

## Open-Source GPU Driver Status

### NVIDIA Open GPU Kernel Modules (2022+)

| Attribute | Details |
|-----------|---------|
| Minimum version | R515+ |
| Architecture support | Turing+ (20-series, 30-series, 40-series) |
| T124/Kepler support | **NO** |
| License | Dual MIT/GPL-2.0 |

**Conclusion:** Not applicable for Tegra K1 projects.

### Nouveau

| Attribute | Details |
|-----------|---------|
| GK20A support | Partial in Mesa |
| Firmware | Available in linux-firmware |
| Performance | Limited (no reclocking) |
| Status | Not recommended for this project |

### Grate-driver

| Attribute | Details |
|-----------|---------|
| Supported SoCs | T20, T30 |
| T124 support | **NO** |
| Status | Community reverse-engineering project |

---

## JetPack Version Mapping

JetPack is NVIDIA's SDK that includes L4T plus additional tools.

```
JetPack 1.x → L4T R21.x (TK1 only)
JetPack 2.x → L4T R21.x - R24.x (TK1, TX1)
JetPack 3.x → L4T R27.x - R28.x (TX1, TX2) [TK1 dropped]
JetPack 4.x → L4T R31.x - R32.x (Xavier, TX2, TX1, Nano)
JetPack 5.x → L4T R34.x - R35.x (Orin, Xavier)
JetPack 6.x → L4T R36.x (Orin)
```

---

## Support Lifecycle

| L4T Series | Initial Release | End of Support |
|------------|-----------------|----------------|
| R21.x | 2014 | 2017 |
| R24.x | 2016 | 2018 |
| R28.x | 2017 | 2020 |
| R32.x | 2019 | 2025 (extended) |
| R35.x | 2022 | Active |
| R36.x | 2023 | Active |

---

## Summary for T124 Projects

For Tegra K1 (T124) based projects:

1. **R24.1 is the final BSP** — No newer official support exists
2. **Kernel 3.10 is the target** — All blobs compiled for this version
3. **Porting required for newer kernels** — No upstream path from NVIDIA
4. **Open source alternatives limited** — No viable open GPU driver

The R24.1 BSP represents the culmination of NVIDIA's T124 support and serves as the foundation for any modern kernel porting efforts.
