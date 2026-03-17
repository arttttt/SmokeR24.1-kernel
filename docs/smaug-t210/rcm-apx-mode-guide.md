# Google Pixel C (Smaug/Dragon, Tegra X1 T210) — RCM/APX Mode Guide

## Device Info

| Field | Value |
|-------|-------|
| Device | Google Pixel C (2015) |
| Codename | dragon (AOSP), smaug (coreboot/firmware) |
| SoC | NVIDIA Tegra X1 (T210), ARM Cortex-A57 + A53 |
| Bootloader | Coreboot + Depthcharge (NOT standard Android bootloader) |
| eMMC | Samsung KLMBG4GEND-B031, 32GB, BGA-153 (11.5x13mm) |
| RAM | 2x Samsung K4F2E304HMMGCH (3GB LPDDR4 total) |
| USB | Type-C |
| iFixit repairability | 4/10 (screen glued, motherboard glued to rear case) |

## What is RCM/APX Mode?

RCM (Recovery Mode) aka APX is an emergency USB recovery protocol implemented in
the Tegra Boot ROM (IROM). It is **burned into silicon** and cannot be patched.

When entered:
- SoC enables USB1 port in **device mode**
- Accepts commands via **Tegra RCM protocol**
- Allows downloading code into IRAM for execution on boot CPU
- Screen remains **black** (no display output)
- Host PC sees USB device **`0955:7721`** (NVIDIA Corp. Tegra X1 APX)

SDRAM is typically **uninitialized** in RCM (no valid BCT processed), so downloaded
code must fit in IRAM first.

## Boot ROM RCM Entry Conditions

The T210 Boot ROM enters RCM if **any** of these conditions is met:

1. **No valid BCT found** in boot memory (or BCT hash fails)
2. **No valid bootloader found** (or bootloader hash fails)
3. **Recovery Mode Strap** (GPIO) asserted — unconditional entry
4. **PMC SCRATCH0 bit 1** set at power-up — unconditional entry

Source: [NVIDIA Tegra Boot Flow](https://http.download.nvidia.com/tegra-public-appnotes/tegra-boot-flow.html)

> "If Tegra PMC register scratch0 bit 2 is set at power-up, recovery mode will
> be entered. This register bit is not cleared when Tegra resets, so any software
> may set this bit, then reboot, to request recovery mode."

(Note: NVIDIA docs say "bit 2" counting from 0, kernel code uses `BIT(1)` —
same thing, value `0x02`.)

---

## Method 1: Software — PMC SCRATCH0 Register Write (Device Boots)

### Prerequisites
- Device boots to Android (or any OS with /dev/mem access)
- Root access (su)
- `busybox` with `devmem` applet, or Python3, or direct `/dev/mem` access

### PMC Register Map

| Address | Offset | Register | Purpose |
|---------|--------|----------|---------|
| `0x7000E400` | `0x00` | PMC_CNTRL | Power management control (write `0x10` = trigger reset) |
| `0x7000E450` | `0x50` | PMC_SCRATCH0 | Boot mode flags, persists across reset |

### PMC_SCRATCH0 Bit Layout

| Bit | Value | Boot Mode |
|-----|-------|-----------|
| 1 | `0x00000002` | **Forced Recovery (RCM/APX)** |
| 30 | `0x40000000` | Bootloader (fastboot) |
| 31 | `0x80000000` | Android Recovery |

Source: [ARM: tegra: Support reboot modes — kernel patch by Thierry Reding](https://patchwork.kernel.org/project/linux-arm-kernel/patch/1399388651-12819-1-git-send-email-thierry.reding@gmail.com/)

### Procedure

#### Option A: busybox devmem

```bash
# Read current value:
adb shell su -c "busybox devmem 0x7000E450 32"

# Set RCM bit:
adb shell su -c "busybox devmem 0x7000E450 32 0x00000002"

# Reboot — device enters RCM:
adb reboot
```

#### Option B: Python3 via /dev/mem

```bash
adb shell su -c "python3 -c \"
import mmap, os, struct
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 4096, offset=0x7000E000)
m[0x450:0x454] = struct.pack('<I', 2)
m.close()
os.close(fd)
\""
adb reboot
```

#### Option C: dd via /dev/mem

```bash
# Write 0x02000000 (little-endian 0x00000002) to offset 0x7000E450:
adb shell su -c "printf '\\x02\\x00\\x00\\x00' | dd of=/dev/mem bs=1 seek=$((0x7000E450)) conv=notrunc"
adb reboot
```

### What Happens

1. Software writes `0x2` to PMC_SCRATCH0
2. System reboots
3. Boot ROM reads PMC_SCRATCH0, sees bit 1 set
4. Boot ROM **skips** normal boot, enables USB1 device mode
5. RCM protocol starts, screen stays black
6. Host PC sees `0955:7721` on USB

### Important Notes

- `adb reboot forced-recovery` does **NOT** work on Pixel C — depthcharge
  does not support this command. Only `fastboot reboot` and
  `fastboot reboot-bootloader` are available. Direct register write is required.
  Source: [GitHub tegra30_debrick issue #7](https://github.com/tofurky/tegra30_debrick/issues/7)
- The PMC_SCRATCH0 bit is **not cleared** by reset — if you get stuck in RCM,
  you need to either run software that clears it, or let RCM protocol clear it
  on successful communication.

---

## Method 2: Software — U-Boot Chainload (Fastboot Available)

### Prerequisites
- Fastboot mode accessible (device shows "Waiting for fastboot command...")
- U-Boot binary compiled for T210

### Procedure

```bash
# On host — boot U-Boot via fastboot:
fastboot boot u-boot-dtb.bin

# In U-Boot console:
mw 0x7000E450 0x00000002   # PMC_SCRATCH0 = RCM bit
mw 0x7000E400 0x00000010   # PMC_CNTRL = trigger reset
```

Device reboots into RCM.

Source: [GitHub issue #7](https://github.com/tofurky/tegra30_debrick/issues/7) —
pgwipeout: *"If you chainload u-boot you can do it manually with a pair of mm
commands to the pmu block."*

---

## Method 3: Hardware — eMMC Pin Shorting (Bricked Device)

This is the **primary method for completely bricked devices** where no software
is running.

### Principle

Boot ROM tries to read BCT (Boot Configuration Table) from eMMC. If the read
**fails** (CRC error, bus error, timeout) → Boot ROM enters RCM.

Shorting an eMMC data line to GND during power-on **sabotages** the read,
forcing RCM entry.

### eMMC BGA-153 Pinout (JEDEC Standard)

The Samsung KLMBG4GEND in the Pixel C uses the standard JEDEC eMMC BGA-153
ball assignment. This pinout is identical across all manufacturers.

Verified from 4 independent datasheets:
- SanDisk iNAND Ultra eMMC 4.51 (80-36-03494)
- Exascend EM300 eMMC 5.1
- UMT Korea GSE32GAABI eMMC 5.1
- Transcend EMC210 eMMC 4.51

#### Key Pins

| Ball | Signal | Type | Description |
|------|--------|------|-------------|
| **A3** | **DAT0** | I/O | **Data line 0 — primary target for shorting** |
| A4 | DAT1 | I/O | Data line 1 |
| A5 | DAT2 | I/O | Data line 2 |
| **A6** | **VSS (GND)** | Supply | **Nearest ground to DAT0** |
| B2 | DAT3 | I/O | Data line 3 |
| B3 | DAT4 | I/O | Data line 4 |
| B4 | DAT5 | I/O | Data line 5 |
| B5 | DAT6 | I/O | Data line 6 |
| B6 | DAT7 | I/O | Data line 7 |
| **M5** | **CMD** | I/O | Command line (alternative short target) |
| **M6** | **CLK** | Input | Clock (alternative short target) |
| K5 | RST_n | Input | Hardware reset (active low) |
| H5 | DS | Output | Data strobe (HS400 mode) |
| C2 | VDDi | — | Internal regulator (connect 0.1uF cap to GND) |

#### Power and Ground Pins

| Balls | Signal | Description |
|-------|--------|-------------|
| E6, F5, J10, K9 | VCC | Core power supply (2.7-3.6V) |
| C6, M4, N4, P3, P5 | VCCQ | I/O power supply (1.7-3.3V) |
| A6, E7, G5, H10, J5, K8 | VSS | Core ground |
| C4, N2, N5, P4, P6 | VSSQ | I/O ground |

#### BGA-153 Ball Map (Top View Through Package / Ball Side Down)

```
       1  2  3  4  5  6  7  8  9 10 11 12 13 14
  A:  nc nc D0 D1 D2 GN nc nc nc nc nc nc nc nc
  B:  nc D3 D4 D5 D6 D7 nc nc nc nc nc nc nc nc
  C:  nc Vi nc GQ nc VQ nc nc nc nc nc nc nc nc
  D:  nc nc nc nc                nc nc nc
  E:  nc nc nc    VC GN nc nc nc    nc nc nc
  F:  nc nc nc    VC             nc    nc nc nc
  G:  nc nc nc    GN             nc    nc nc nc
  H:  nc nc nc    DS            GN    nc nc nc
  J:  nc nc nc    GN            VC    nc nc nc
  K:  nc nc nc    RS nc nc GN VC nc    nc nc nc
  L:  nc nc nc                        nc nc nc
  M:  nc nc nc VQ CM CL nc nc nc nc nc nc nc nc
  N:  nc GQ nc VQ GQ nc nc nc nc nc nc nc nc nc
  P:  nc nc VQ GQ VQ GQ nc nc nc nc nc nc nc nc

Legend:
  D0-D7 = DAT0-DAT7        CM = CMD           CL = CLK
  VC = VCC (power)          VQ = VCCQ (I/O power)
  GN = VSS (ground)         GQ = VSSQ (I/O ground)
  RS = RST_n                Vi = VDDi          DS = Data Strobe
  nc = No Connect
```

### What To Short

**Recommended: DAT0 (A3) → VSS/GND (A6)**

These balls are in the same row, 3 positions apart. On the PCB, traces from
these balls run to pads/vias that may be accessible without direct BGA contact.

| Target | Ball → GND | Effect | Time to APX |
|--------|-----------|--------|-------------|
| DAT0 | A3 → A6/E7 | Data read fails | **Instant** |
| CMD | M5 → N5 | Command fails | ~100 seconds (retry timeout) |
| CLK | M6 → K8 | No clock | **Instant** but harder to solder |
| Random pin | Any → GND | Various errors | ~100 seconds |

Source: [Yifan Lu — Unbricking SHIELD TV (2015)](https://yifan.lu/2022/06/17/unbricking-shield-tv-2015-with-a-bootrom-exploit/)
— *"I used a pin to short one of them to the shielding near it and saw the APX
device show up on my computer."* Different pins: *"With a randomly picked pin it
took about 100 seconds [...] a different pin to make it instant but that one is
harder to solder."*

### Required Tools

- Heat gun or hair dryer (~80°C for screen adhesive softening)
- Suction cup + plastic pry tool / guitar pick (for screen removal)
- Precision screwdriver set
- **Thin needle or fine-tip tweezers** (for temporary shorting)
- **Or**: 0.1-0.3mm enameled wire + fine-tip soldering iron + flux (for reliable repeatable contact)
- USB-C cable
- Multimeter (for finding testpoints via continuity check)
- Magnifying glass or microscope (BGA pads are 0.5mm pitch)

### Step-by-Step Procedure

#### Step 1: Disassemble the Pixel C

1. Power off the device completely
2. Heat the screen edges to ~80°C with heat gun (softens adhesive)
3. Apply suction cup near edge, pull slightly while inserting plastic pry tool
4. Slowly work around the perimeter to separate screen from frame
5. **WARNING**: Display flex cable is short — do not yank the screen off
6. Disconnect display and battery flex cables
7. Remove screws holding the motherboard shield/frame

Reference: [iFixit Pixel C Teardown](https://www.ifixit.com/Teardown/Google+Pixel+C+Teardown/62277)

#### Step 2: Locate the eMMC Chip

The Samsung KLMBG4GEND is a BGA-153 chip (11.5mm x 13mm) located on the
motherboard near the Tegra X1 SoC. Look for markings:
```
SAMSUNG
KLMBG4GEND-B031
(or similar)
```

#### Step 3: Find Testpoints

Most PCBs have testpoints (small exposed pads) connected to key eMMC signals.
Use a multimeter in continuity mode:

1. Identify pin 1 / orientation mark on the eMMC BGA
2. Based on the ball map, A3 (DAT0) is in the top-left area
3. Probe nearby exposed pads — beep = connected to that signal
4. Mark the DAT0 testpoint and a nearby GND point

**Tip**: If no testpoints exist, look for vias (small plated holes) in the
traces running from the BGA pads. These can be soldered to.

**Finding signals without testpoints** (voltage method):
- VCC pads: ~3.3V when powered
- CLK: ~half supply voltage (toggling)
- CMD: ~full supply voltage (pulled high)
- DAT0: ~full supply voltage (pulled high)
- GND: 0V (continuity to metal shielding/frame)

#### Step 4: Short DAT0 to GND

**Temporary method (needle/tweezers):**
1. Touch one tip to DAT0 testpoint
2. Touch other tip to GND (metal shielding, ground pad, frame screw hole)
3. Hold steady

**Permanent method (solder wire):**
1. Tin both pads with flux + solder
2. Solder thin enameled wire from DAT0 pad to GND pad
3. This allows repeatable RCM entry by connecting/disconnecting the wire

#### Step 5: Power On While Shorted

1. Connect USB-C cable to host PC
2. **While holding the short**, press and hold Power button
3. Keep holding for:
   - DAT0/CLK short: ~5-10 seconds (instant RCM)
   - CMD/random pin short: up to **100 seconds** (wait for timeout)

#### Step 6: Verify RCM on Host

```bash
# Linux:
lsusb | grep "0955:7721"
# Expected output:
# Bus 00X Device 00Y: ID 0955:7721 NVIDIA Corp. Tegra X1 (APX)

# Watch dmesg in real time:
sudo dmesg -w | grep -i "nvidia\|tegra\|apx\|rcm"

# macOS:
system_profiler SPUSBDataType 2>/dev/null | grep -A5 -i "nvidia\|0955"

# Windows:
# Device Manager → "APX" device under "NVIDIA USB Boot recovery driver"
```

#### Step 7: Release the Short

Once the host detects the APX USB device, release the short (remove
needle/tweezers or disconnect the wire). The device stays in RCM until power
is cycled or a payload resets it.

---

## Method 4: Hardware — Direct eMMC Programming (Bypass RCM Entirely)

This method does **not** use RCM at all. Instead, it directly reads/writes
the eMMC flash chip via an external programmer. Use when eMMC shorting fails
or when you need to restore the bootloader partition directly.

### Option A: ISP (In-System Programming) — Without Removing the Chip

Solder 5 wires from eMMC testpoints on the PCB to an SD card reader.

#### Wiring

| Wire | eMMC Signal | BGA-153 Ball | SD Card Pin | SD Pin Name |
|------|-------------|-------------|-------------|-------------|
| 1 | DAT0 | A3 | Pin 7 | DAT0 |
| 2 | CMD | M5 | Pin 2 | CMD |
| 3 | CLK | M6 | Pin 5 | CLK |
| 4 | VCC | E6 (any VCC) | Pin 4 | VDD (3.3V) |
| 5 | GND | A6 (any VSS) | Pin 3 or 6 | VSS |

**IMPORTANT**: Disconnect the battery before soldering. The Tegra SoC must
NOT be driving the eMMC bus while the external reader is connected.

#### Compatible SD Card Readers

Not all readers work — the reader must support **1-bit MMC mode**:

| Reader | Chipset | Works? |
|--------|---------|--------|
| Transcend TS-RDF5 (USB 3.0) | RTS5129 | **Yes** |
| Generic Realtek RTS5139/5129 | Realtek | **Yes** |
| Most cheap USB readers | Various | Maybe |
| Internal laptop SD slots | Various | Usually no |

Source: [BlackHat — Hacking Hardware With A $10 SD Card Reader](https://blackhat.com/docs/us-17/wednesday/us-17-Etemadieh-Hacking-Hardware-With-A-$10-SD-Card-Reader-wp.pdf)

#### Reading/Writing

```bash
# Linux — device appears as block device:
dmesg | tail
# [xxx.xxx] mmc0: new high speed MMC card at address 0001
# [xxx.xxx] mmcblk0: mmc0:0001 032G4a 29.1 GiB

# Full dump:
sudo dd if=/dev/mmcblk0 of=pixel_c_emmc_full.img bs=4M status=progress

# Write factory image back:
sudo dd if=factory_bootloader.img of=/dev/mmcblk0 bs=4M status=progress

# Or mount partitions:
sudo fdisk -l /dev/mmcblk0
sudo mount /dev/mmcblk0pX /mnt/pixel_c
```

### Option B: Chip-Off — Remove eMMC and Use BGA Socket Adapter

#### Required Tools
- Hot air rework station (300-320°C)
- Flux (no-clean, liquid or paste)
- Kapton tape (protect surrounding components)
- BGA-153 socket adapter → SD card
  - [ALLSOCKET eMMC153+169 to SD](https://www.amazon.com/ALLSOCKET-eMMC153-Programmer-Kingston-Black-SD/dp/B076HTDH2J)
  - E-Mate Pro eMMC tool
- USB SD card reader
- BGA reballing stencil + solder balls (for reinstallation)

#### Procedure

1. **Protect surroundings**: Cover nearby components with Kapton tape
2. **Apply flux**: Around the eMMC chip edges
3. **Reflow**: Hot air at 300-320°C, circular motion, 10-15 seconds until chip
   lifts with tweezers
4. **Clean pads**: Remove old solder from both chip and PCB pads with flux +
   wick
5. **Insert into adapter**: Place chip in BGA-153 socket, close clamp
6. **Connect to PC**: Via USB SD reader, Linux mounts automatically
7. **Read/write**: dd, parted, or forensic tools
8. **Reball** (if reinstalling): Apply flux, place BGA stencil, apply solder
   balls, reflow
9. **Reflow back onto PCB**: Align chip, reflow at 300-320°C

#### Option C: CH341A Programmer

The CH341A is a $3-5 USB programmer. It is primarily designed for SPI/I2C
EEPROM chips and does **not** directly interface with eMMC BGA-153. However:

- If the Pixel C has a **separate SPI flash** for bootloader/firmware (some
  Tegra boards do), CH341A can program that directly
- For eMMC, use the SD card reader method above instead

CH341A + SOIC8 clip is useful for reading/writing SPI NOR flash chips that
may store the BCT or early bootloader stages on some Tegra board designs.

Source: [XDA Forums](https://xdaforums.com/t/bricked-pixel-c-no-recovery-no-os-fastboot-errors.3956596/page-2)
— user Vartom: *"I fixed such a lock by flashing a chip with a bootloader.
To do this, I had to peel off the screen and buy a programmer on Ali."*

---

## After Entering RCM: Using Fusee Gelee

The Tegra X1 Boot ROM has an **unpatchable** vulnerability in its USB RCM
stack (CVE-2018-6242), discovered by Kate Temkin. A buffer overflow in the
RCM protocol allows executing arbitrary unsigned code.

This affects **all T210 devices**: Pixel C, NVIDIA Shield TV (2015/2017),
Nintendo Switch (original), Jetson TX1, etc.

### Host Tools

| Tool | Platform | Link |
|------|----------|------|
| fusee-launcher | Linux, macOS | https://github.com/Qyriad/fusee-launcher |
| TegraRcmGUI | Windows | https://github.com/eliboa/TegraRcmGUI |
| TegraRcmSmash | Windows (CLI) | https://github.com/rajkosto/TegraRcmSmash |
| tegrarcm | Linux (official NVIDIA) | https://github.com/NVIDIA/tegrarcm |
| ShofEL2 | Linux | https://github.com/fail0verflow/shofel2 |
| Web Fusee Launcher | Browser (WebUSB) | https://switch.exploit.fortheusers.org/ |

### Sending a Payload (Linux/macOS)

```bash
# Install dependencies:
pip3 install pyusb

# Clone fusee-launcher:
git clone https://github.com/Qyriad/fusee-launcher
cd fusee-launcher

# Send payload (requires root for USB access):
sudo python3 fusee-launcher.py <payload.bin>
```

### Sending a Payload (Windows)

1. Download [TegraRcmGUI](https://github.com/eliboa/TegraRcmGUI/releases)
2. Install APX drivers (included in the package)
3. Select payload file
4. Click "Inject Payload"

### What Payload To Send

For Pixel C recovery, you need a payload that:
1. Initializes DRAM (using Pixel C BCT/SDRAM parameters)
2. Starts a fastboot server or u-boot console

The ShofEL2 exploit chain uses Pixel C DDR4 memory training code extracted from
the factory coreboot image:
```bash
# Extract from Pixel C factory image:
cbfstool coreboot.rom extract -n fallback/tegra_mtc -f tegra_mtc.bin
```

After the payload runs, the device presents as a fastboot device and standard
flashing can proceed:
```bash
fastboot flash bootloader bootloader-dragon-google_smaug.img
fastboot reboot-bootloader
fastboot flash boot boot.img
fastboot flash system system.img
fastboot flash vendor vendor.img
fastboot reboot
```

### References

- [Fusee Gelee disclosure (Kate Temkin / PDF)](https://misc.ktemkin.com/fusee_gelee_nvidia.pdf)
- [ShofEL2 — fail0verflow blog post](https://fail0verflow.com/blog/2018/shofel2/)
- [XDA: Tegra X1 vulnerability affects Pixel C and Shield](https://www.xda-developers.com/nvidia-tegra-x1-google-pixel-c-nvidia-shield/)

---

## Pixel C GPIO Reference

From the upstream kernel device tree `tegra210-smaug.dts`:

| Button | GPIO | Active Level |
|--------|------|-------------|
| Power | TEGRA_GPIO(X, 5) | LOW |
| Volume Down | TEGRA_GPIO(X, 7) | LOW |
| Volume Up | TEGRA_GPIO(M, 4) | LOW |
| Lid Switch | TEGRA_GPIO(B, 4) | — (magnetic) |
| Tablet Mode | TEGRA_GPIO(Z, 2) | — |

Source: [tegra210-smaug.dts gpio-keys patch](https://patchwork.ozlabs.org/comment/1286927/)

**Note**: The **Recovery Mode Strap GPIO** for Pixel C is **not publicly
documented**. On the Nintendo Switch (same T210 SoC), it is Pin 10 on the
right Joy-Con rail connector, shorted to GND. On the Pixel C, this strap pin
is likely not routed to any external connector, making button-based RCM
entry impossible without hardware modification.

---

## Method Comparison

| Device State | Method | Difficulty | Confirmed? | Time |
|-------------|--------|-----------|-----------|------|
| Android boots + root | PMC SCRATCH0 write | Easy | HW-level mechanism | Seconds |
| Fastboot works | U-Boot chainload | Medium | Proposed, not confirmed | Minutes |
| Complete brick | eMMC DAT0 short | Hard (disassembly) | Yes (Shield TV, same SoC) | Seconds-100s |
| Complete brick | ISP via SD reader | Hard (disassembly + soldering 5 wires) | Yes | Minutes |
| Complete brick | Chip-off + BGA adapter | Very hard (reflow) | Yes (XDA) | Hours |

---

## Source Links

### Primary Technical Sources
- [NVIDIA Tegra Boot Flow (official)](https://http.download.nvidia.com/tegra-public-appnotes/tegra-boot-flow.html)
- [ARM: tegra: Support reboot modes — kernel patch](https://patchwork.kernel.org/project/linux-arm-kernel/patch/1399388651-12819-1-git-send-email-thierry.reding@gmail.com/)
- [Fusee Gelee vulnerability disclosure (PDF)](https://misc.ktemkin.com/fusee_gelee_nvidia.pdf)
- [ShofEL2 — Tegra X1 exploit (fail0verflow)](https://fail0verflow.com/blog/2018/shofel2/)
- [Unbricking SHIELD TV (2015) with Bootrom Exploit — Yifan Lu](https://yifan.lu/2022/06/17/unbricking-shield-tv-2015-with-a-bootrom-exploit/)

### Community / Forums
- [Tegra X1 Pixel C debrick discussion — GitHub](https://github.com/tofurky/tegra30_debrick/issues/7)
- [Bricked Pixel C — XDA Forums](https://xdaforums.com/t/bricked-pixel-c-no-recovery-no-os-fastboot-errors.3956596/page-2)
- [Pixel C bootloader unlock guide — XDA Forums](https://xdaforums.com/t/guide-unlock-bootloader-install-custom-recovery-and-root-the-pixel-c-2-4-2016.3307183/)
- [APX Mode / RCM documentation — Open Surface RT](https://open-rt.gitbook.io/open-surfacert/common/boot-sequence/tegra-soc-boot-process/fusee-gelee/apx-mode-usb-debugging)

### Tools
- [fusee-launcher (Python)](https://github.com/Qyriad/fusee-launcher)
- [TegraRcmGUI (Windows)](https://github.com/eliboa/TegraRcmGUI)
- [TegraRcmSmash (Windows CLI)](https://github.com/rajkosto/TegraRcmSmash)
- [NVIDIA tegrarcm (official)](https://github.com/NVIDIA/tegrarcm)
- [BGA-153 DAT0 Breakout PCB](https://github.com/Pheeeeenom/BGA153-DAT0-Breakout)

### Hardware Reference
- [iFixit Pixel C Teardown](https://www.ifixit.com/Teardown/Google+Pixel+C+Teardown/62277)
- [pixelc-linux documentation](https://github.com/pixelc-linux/documentation)
- [Samsung KLMBG4GEND-B031 datasheet](https://semiconductor.samsung.com/estorage/emmc/emmc-5-0/klmbg4gend-b031/)
- [eMMC data recovery guide — Dangerous Payload](https://dangerouspayload.com/2018/10/24/emmc-data-recovery-from-damaged-smartphone/)
- [Hacking Hardware With A $10 SD Card Reader — BlackHat](https://blackhat.com/docs/us-17/wednesday/us-17-Etemadieh-Hacking-Hardware-With-A-$10-SD-Card-Reader-wp.pdf)
- [tegra210-smaug.dts — upstream kernel](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/nvidia/tegra210-smaug.dts)
- [coreboot-smaug](https://github.com/ttefke/coreboot-smaug)
