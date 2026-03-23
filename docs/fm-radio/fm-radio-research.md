# FM Radio on Xiaomi Mi Pad 1 (mocha) — Research

## Hardware

- **SoC**: NVIDIA Tegra K1 (T124)
- **WiFi/BT/FM chip**: Broadcom BCM4354 (802.11ac + BT 4.0 + FM receiver)
  - Confirmed from: `CONFIG_BCM4354=y` in defconfig, DTS `bcmdhd_wlan` node, bcmdhd driver
  - Connected via SDIO (WiFi) to SDMMC1, UART (BT/FM) to ttyHS
- **Audio codec**: Realtek RT5671 (I2C address 0x1c on i2c@7000c000)
- **Speaker amplifiers**: 2x NXP TFA9890 (I2C 0x34 left, 0x37 right)
- **FM on stock firmware**: confirmed working (Xiaomi MIUI, Android 4.4)

### FM Audio Path (hardware level)

```
BCM4354 FM I2S output pins → RT5671 AIF4 (I2S4 interface, codec-mastered)
    → RT5671 internal digital mixer → DAC
    → HPL/HPR (headphones) or SPKL/SPKR (speakers via TFA9890)
```

- FM audio goes **directly** from BCM4354 to RT5671 codec — Tegra AHUB is **not involved**
- RT5671 AIF4 is configured as I2S, codec-mastered (CBM_CFM), stereo, 8-48kHz
- No dedicated Tegra I2S port is used for FM — `spdif-dit.3` is a dummy ASoC DAI placeholder
- Headphones likely serve as FM antenna (standard for BCM FM receivers)

### FM Control Path

```
FM commands → HCI Vendor-Specific Commands (opcode 0xFC15) → shared UART → BCM4354 firmware
```

FM is controlled via HCI VSC register read/write commands sent over the same UART as BT.

---

## Kernel State (SmokeR24.1-kernel)

### Broadcom V4L2 FM Driver — present in tree

Added manually (not from vendor BSP) in two commits:
1. `ce5ec5c` (2016-06-04, Olivier Karasangabo/CyanogenMod): "bluetooth: Add Broadcom V4L2 ANT/BT/FM device drivers" — 30 files
2. `40b7498` (2018-06-16, arttttt): "drivers: bluetooth: broadcom: I2S as a default audio path" — changed `DEF_V4L2_FM_AUDIO_PATH` from `FM_AUDIO_DAC` to `FM_AUDIO_I2S`, removed Sony-specific config

**Status: never worked on mocha.** Was an attempt to bring up FM that did not succeed (exact failure unknown).

### File listing

```
drivers/bluetooth/broadcom/
├── Kconfig                                     # 4 tristate options
├── Makefile                                    # routes to subdirs
├── include/
│   ├── brcm_ldisc_sh.h                        # shared transport API, proto types
│   ├── fm.h                                    # FM packet structs, CH8 constants
│   ├── ant.h                                   # ANT+ packet structs
│   ├── v4l2_target.h                           # debug/snoop enable flags
│   └── v4l2_logs.h                             # log flag bits
├── line_discipline_driver/
│   ├── brcm_sh_ldisc.c                        # shared UART transport, ldisc N_BRCM_HCI=26
│   ├── brcm_hci.c                             # HCI UART framing, CH8 mux/demux
│   ├── brcm_hci_uart.h                        # ldisc structs, proto IDs
│   ├── brcm_bluesleep.c                       # LPM/sleep control
│   └── Makefile
├── bt_protocol_driver/
│   ├── brcm_bt_drv.c                          # BT char device /dev/brcm_bt_drv
│   ├── brcm_bt_drv.h
│   └── Makefile
├── v4l2_fm_driver/
│   ├── fmdrv_main.c                           # FM core: cmd send/recv, interrupt handling, RDS parsing
│   ├── fmdrv_main.h                           # FM register map, command structs, constants
│   ├── fmdrv_v4l2.c                           # V4L2 ioctl handlers, /dev/radio registration
│   ├── fmdrv_v4l2.h                           # V4L2 init/deinit prototypes
│   ├── fmdrv_rx.c                             # FM RX: tune, seek, volume, mute, RDS, band config
│   ├── fmdrv_rx.h                             # RX function prototypes
│   ├── fmdrv.h                                # device context struct, state enums, bit definitions
│   ├── fmdrv_config.h                         # compile-time defaults (region, audio path, thresholds)
│   ├── fm_public.h                            # public constants (regions, audio modes, freq macros)
│   └── Makefile                               # BUG: uses CONFIG_V4L2_ANT_DRIVER instead of FM
└── v4l2_ant_driver/
    ├── antdrv_main.c, antdrv_v4l2.c, antdrv.h, antdrv_main.h, antdrv_v4l2.h
    └── Makefile
```

### Defconfig (tegra12_android_defconfig)

```
CONFIG_BT=y
CONFIG_BT_HCIUART=y              # standard Linux HCI UART ldisc (N_HCI)
CONFIG_BT_HCIUART_H4=y           # H4 protocol
CONFIG_BLUEDROID_PM=y             # Android bluedroid power management

CONFIG_BT_PROTOCOL_DRIVER=y      # Broadcom BT via shared transport
CONFIG_LINE_DISCIPLINE_DRIVER=y   # Broadcom shared ldisc (N_BRCM_HCI=26)
CONFIG_V4L2_FM_DRIVER=y          # FM V4L2 driver
CONFIG_V4L2_ANT_DRIVER=y         # ANT+ V4L2 driver
```

**Transport conflict**: Both standard `hci_uart` (N_HCI) and Broadcom `brcm_sh_ldisc` (N_BRCM_HCI=26) are compiled in. They are different line disciplines wanting the same physical UART.

### Platform devices (board-ardbeg.c)

Line 186: `&bcm_ldisc_device` — Broadcom shared transport platform device
Line 190: `&fm_dit_device` — FM audio dummy DAI (`spdif-dit.3`)

Both registered in ardbeg platform device array (mocha uses ardbeg board file).

### Audio routing (tegra_rt5671.c)

5 DAI links defined:

| Link | ID | CPU DAI | Codec DAI | Format | Purpose |
|------|----|---------|-----------|--------|---------|
| HIFI | 0 | tegra30-i2s.0 | rt5671-aif1 | I2S, CBS_CFS | Main audio |
| LEFT_SPK | 1 | rt5671-aif2 | tfa98xx.0-0034 | I2S | Left speaker amp |
| RIGHT_SPK | 2 | rt5671-aif2 | tfa98xx.0-0037 | I2S | Right speaker amp |
| BTSCO | 3 | spdif-dit.1 | rt5671-aif3 | DSP_A, CBM_CFM | BT SCO 8kHz mono |
| **FM** | **4** | **spdif-dit.3** | **rt5671-aif4** | **I2S, NB_NF, CBM_CFM** | **FM audio 8-48kHz stereo** |

DAPM widgets and routes:
```c
SND_SOC_DAPM_LINE("FM", NULL)              // line input widget
{"FM Capture", NULL, "FM"}                  // DAPM route
SOC_DAPM_PIN_SWITCH("FM")                   // userspace mixer control
```

Codec conf for FM prefix:
```c
{ .dev_name = "spdif-dit.3", .name_prefix = "FM" }
```

**To enable FM audio**: `amixer cset name="FM Switch" on`

### Tegra I2S port allocation

| I2S | DAP | Pinmux | Used by |
|-----|-----|--------|---------|
| I2S0 | DAP1 | Active, i2s0 | RT5671 AIF1 (HiFi) |
| I2S1 | DAP2 | Tristated | Free |
| I2S2 | DAP3 | Partially pinmuxed, tristated | Free |
| I2S3 | DAP4 | Tristated | Free |
| I2S4 | — | Not pinmuxed | Free |

FM audio does NOT use any Tegra I2S — goes directly BCM→RT5671.

---

## FM Protocol Reference

All information extracted from `fmdrv_main.c`, `fmdrv_main.h`, `fmdrv_rx.c`, `fmdrv.h`, `fm_public.h`.

### HCI Command Format

Every FM command is an HCI Vendor-Specific Command:
- **OGF**: 0x3F (vendor specific)
- **OCF**: 0x0015
- **Opcode**: 0xFC15 (`hci_opcode_pack(0x3F, 0x0015)`)

```
Offset  Size  Field         Value
0       1     pkt_type      0x01 (HCI command)
1-2     2     opcode        0x15 0xFC (little-endian)
3       1     param_len     payload_len + 2
4       1     fm_reg_addr   FM register address (see table below)
5       1     rd_wr         0x00=write, 0x01=read, 0x02=VSC HCI cmd
6+      N     data          register value (16-bit LE for writes)
                            read_length byte (1 or 2) for reads
```

C struct from `fmdrv_main.h`:
```c
struct fm_cmd_msg_hdr {
    __u8  header;     // 0x01 = HCI_COMMAND
    __u16 cmd;        // hci_opcode_pack(0x3F, 0x0015) = 0xFC15
    __u8  len;        // payload_len + 2
    __u8  fm_opcode;  // FM register address
    __u8  rd_wr;      // 0x00=write, 0x01=read
} __attribute__ ((packed));
```

Special case: VSC HCI command (type=0x02) uses different OCF, e.g. 0xFC61 for PCM pin routing.

### HCI Response Format

**Command Complete (event 0x0E) for opcode 0xFC15:**
```
Offset  Size  Field
0       1     event_code    0x0E
1       1     param_len
2       1     num_hci_pkts
3-4     2     opcode        0x15 0xFC
5       1     status        0x00 = success
6       1     fm_opcode     echo of register address
7       1     rd_wr         echo of read/write
8+      N     data          register value (for reads)
```

**Vendor-Specific Event (0xFF) — FM interrupt:**
```
Offset  Size  Field
0       1     event_code    0xFF
1       1     param_len
2       1     subcode       0x08 = BRCM_VSE_SUBCODE_FM_INTERRUPT
```

On receiving FM interrupt, driver sends a read of `FM_REG_FM_RDS_FLAG (0x12)` to get interrupt status bits.

### FM Register Map (complete)

#### Core control registers

| Register | Addr | R/W | Size | Description |
|----------|------|-----|------|-------------|
| FM_REG_RDS_SYS | 0x00 | RW | 8 | FM ON/OFF and RDS enable. 0x00=OFF, 0x01=FM ON, 0x02=RDS ON, 0x03=FM+RDS |
| FM_REG_FM_CTRL | 0x01 | RW | 8 | Band and stereo control. Bits: [0]=West/East band, [1]=stereo auto, [2]=stereo manual, [3]=stereo switch, [4]=HI/LO injection |
| FM_REG_RDS_CTL0 | 0x02 | RW | 8 | RDS type control. 0x00=RDS, 0x01=RBDS, 0x02=FIFO flush |

#### Audio control registers

| Register | Addr | R/W | Size | Description |
|----------|------|-----|------|-------------|
| FM_REG_AUD_PAUS | 0x04 | RW | 8 | Audio pause control |
| FM_REG_AUD_CTL0 | 0x05 | RW | 16 | Audio control. Bits: [0]=RF mute, [1]=manual mute, [2]=zero-cross mute L off, [3]=zero-cross mute R off, [4]=DAC on, [5]=I2S on, [6]=75us de-emphasis (0=50us), [7]=audio bandwidth |
| FM_REG_AUD_CTL1 | 0x06 | RW | 8 | Audio control 1 |
| FM_REG_VOLUME_CTRL | 0xF8 | RW | 16 | Volume (0-255 range) |
| FM_REG_BLEND_MUTE | 0xF9 | RW | 8 | Blend and soft mute control |
| FM_REG_PCM_ROUTE | 0x4D | RW | 8 | PCM routing. Bit [7] = FM on SCO (route FM audio over BT) |

#### Tuning and search registers

| Register | Addr | R/W | Size | Description |
|----------|------|-----|------|-------------|
| FM_REG_SCH_CTL0 | 0x07 | RW | 8 | Search control: [6:0]=RSSI threshold, [7:4]=search direction mask |
| FM_SEARCH_SNR | 0x08 | RW | 8 | Search SNR threshold (0-31) |
| FM_REG_SCH_TUNE | 0x09 | RW | 8 | Tune/seek trigger. 0=normal scan, 1=preset, 2=seek, 3=AF jump |
| FM_REG_FM_FREQ | 0x0A | RW | 16 | Frequency register (see encoding below) |
| FM_REG_FM_FREQ1 | 0x0B | RW | 16 | Frequency register 1 (extended) |
| FM_REG_SCH_STEP | 0xFD | RW | 8 | Scan step size: 0=50kHz, 1=100kHz, 2=200kHz |
| FM_SEARCH_BOUNDARY | 0xFB | RW | 16 | Custom band boundary frequencies |
| FM_SEARCH_METHOD | 0xFC | RW | 8 | Scan mode for 2048B0 firmware |
| FM_REG_PRESET_MAX | 0xFE | RW | 8 | Maximum preset stations for scan |
| FM_REG_PRESET_STA | 0xFF | R | varies | Preset station list |

#### Status and signal registers

| Register | Addr | R/W | Size | Description |
|----------|------|-----|------|-------------|
| FM_REG_AF_FREQ0 | 0x0C | RW | 16 | Alternate frequency 0 |
| FM_REG_AF_FREQ1 | 0x0D | RW | 16 | Alternate frequency 1 |
| FM_REG_CARRIER | 0x0E | R | 16 | Carrier frequency offset |
| FM_REG_RSSI | 0x0F | R | 8 | RSSI (2's complement, convert: `(0x80 - val) & ~0x80`) |
| FM_REG_SNR | 0xDF | R | 8 | SNR of currently tuned channel |
| FM_REG_AF_FAILURE | 0x90 | R | 8 | AF jump failure reason code |
| FM_RES_PRESCAN_QUALITY | 0xDE | RW | 8 | Preset scan CO slope threshold |

#### Interrupt registers

| Register | Addr | R/W | Size | Description |
|----------|------|-----|------|-------------|
| FM_REG_FM_RDS_MSK | 0x10 | RW | 16 | Interrupt mask (enable bits) |
| FM_REG_FM_RDS_MSK1 | 0x11 | RW | 16 | Interrupt mask (high byte) |
| FM_REG_FM_RDS_FLAG | 0x12 | R | 16 | Interrupt status flags |
| FM_REG_FM_RDS_FLAG1 | 0x13 | R | 16 | Interrupt status (high byte) |

Interrupt flag bits (FM_REG_FM_RDS_FLAG 0x12):
```
Bit 0:  I2C_MASK_SRH_TUNE_CMPL_BIT    Search/tune complete
Bit 1:  I2C_MASK_SRH_TUNE_FAIL_BIT    Search/tune failed
Bit 2:  I2C_MASK_RSSI_LOW_BIT         RSSI below threshold (for AF switch)
Bit 3:  I2C_MASK_CARR_HI_ERR_BIT      Carrier high error
Bit 4:  I2C_MASK_AUDIO_PAUSE_BIT      Audio paused (threshold/duration met)
Bit 5:  I2C_MASK_STEREO_DETC_BIT      Stereo detected
Bit 6:  I2C_MASK_STEREO_ACTIVE_BIT    Stereo active
Byte 2:
Bit 9:  I2C_MASK_RDS_FIFO_WLINE_BIT   RDS FIFO at/above waterline
Bit 11: I2C_MASK_BB_MATCH_BIT         Block B match found
Bit 12: I2C_MASK_SYNC_LOST_BIT        RDS sync lost
Bit 13: I2C_MASK_PI_MATCH_BIT         PI code match found
```

#### RDS registers

| Register | Addr | R/W | Size | Description |
|----------|------|-----|------|-------------|
| FM_REG_RDS_WLINE | 0x14 | RW | 8 | RDS waterline threshold |
| FM_REG_BB_MAC0/1 | 0x16-0x17 | RW | 16 | Block B match address |
| FM_REG_BB_MSK0/1 | 0x18-0x19 | RW | 16 | Block B match mask |
| FM_REG_PI_MAC0/1 | 0x1A-0x1B | RW | 16 | PI code match address |
| FM_REG_PI_MSK0/1 | 0x1C-0x1D | RW | 16 | PI code match mask |
| FM_REG_RCV_ID | 0x28 | R | 16 | Receiver ID |
| FM_REG_CFG | 0x29 | RW | 8 | Configuration |
| FM_REG_RDS_DATA | 0x80 | R | 3*N | RDS FIFO (3-byte tuples, up to 64 tuples per read) |

RDS tuple format (3 bytes each):
- Byte 1: data MSB + block type in upper nibble (A=0x00, B=0x10, C=0x20, D=0x30, C'=0x40, E=0x50/0x60)
- Byte 2: data LSB
- Byte 3: quality bits [3:2] (0=no error, 1=2-bit, 2=3-bit, 3=unrecoverable)

RDS end marker: `0x7C 0xFF 0xFF`

RDS data elements parsed by driver:
- PI code (Program Identification)
- PTY (Program Type, 0-31, maps to genre strings)
- TP (Traffic Program flag)
- TA (Traffic Announcement flag)
- MS (Music/Speech flag)
- PSN (Program Service Name, 8 chars)
- RT (Radio Text, 64 chars)
- CT (Clock Time: day, hour, minute, second)

### Frequency Encoding

From `fm_public.h`:
```c
#define FM_GET_FREQ(x) ((unsigned short)((x * 10) - 64000))   // kHz/10 → register value
#define FM_SET_FREQ(x) ((unsigned int)((x + 64000) / 10))     // register value → kHz/10
```

Input `x` for `FM_GET_FREQ` is frequency in units of 10kHz (e.g., 8750 for 87.50 MHz).
Output is a 16-bit register value.

Examples:
```
87.5  MHz = 8750  → FM_GET_FREQ(8750)  = (8750*10)-64000  = 23500 = 0x5BCC
101.0 MHz = 10100 → FM_GET_FREQ(10100) = (10100*10)-64000 = 37000 = 0x9088
108.0 MHz = 10800 → FM_GET_FREQ(10800) = (10800*10)-64000 = 44000 = 0xABE0
```

### Region Configurations

From `fmdrv_main.c`:

| Region | ID | Low (MHz) | High (MHz) | De-emphasis | Step (kHz) |
|--------|-----|-----------|------------|-------------|------------|
| Europe | 0 | 87.5 | 108.0 | 50us | 100 |
| Japan | 1 | 76.0 | 108.0 | 50us | 100 |
| North America | 2 | 87.5 | 108.0 | 75us | 200 |
| Russia | 3 | 65.8 | 108.0 | 75us | 100 |
| China | 4 | 76.0 | 108.0 | 75us | 100 |
| Italy | 5 | 87.5 | 108.0 | 50us | 50 |

### Audio Path Configuration

Audio path values from `fm_public.h`:
```c
#define FM_AUDIO_NONE       0x00  // No FM audio
#define FM_AUDIO_DAC        0x01  // Analog output (DAC on chip)
#define FM_AUDIO_I2S        0x02  // Digital I2S output
#define FM_AUDIO_BT_MONO    0x04  // FM over BT SCO mono
#define FM_AUDIO_BT_STEREO  0x08  // FM over BT SCO stereo
```

Audio control bits (FM_REG_AUD_CTL0 = 0x05):
```
Bit 0: FM_RF_MUTE           RF-dependent mute
Bit 1: FM_MANUAL_MUTE       Manual mute on/off
Bit 2: FM_Z_MUTE_LEFT_OFF   Zero-crossing mute left channel off
Bit 3: FM_Z_MUTE_RITE_OFF   Zero-crossing mute right channel off
Bit 4: FM_AUDIO_DAC_ON      Enable DAC output
Bit 5: FM_AUDIO_I2S_ON      Enable I2S output
Bit 6: FM_DEEMPHA_75_ON     De-emphasis: 0=50us (EUR), 1=75us (NA)
Bit 7: FM_AUDIO_BAND_WIDTH  Audio bandwidth control
```

I2S pin routing via VSC 0xFC61 (`VSC_HCI_WRITE_PCM_PINS_OCF`):
```c
// I2S slave mode on PCM pins (host=master):
{0x07, 0x19, 0x18, 0x19, 0x19}
// I2S master mode on PCM pins (host=slave):
{0x05, 0x19, 0x18, 0x18, 0x18}
// PCM/BT slave mode:
{0x01, 0x19, 0x18, 0x19, 0x19}
// PCM/BT master mode:
{0x01, 0x19, 0x18, 0x18, 0x18}
```

Current mocha config: `DEF_V4L2_FM_AUDIO_PATH = FM_AUDIO_I2S` (set by arttttt's commit).

### Init Sequence

From `fmc_turn_fm_on()` and `fmdrv_rx.c`:

1. Register with shared transport: `brcm_sh_ldisc_register(PROTO_SH_FM)` — gets write function pointer
2. Write `FM_REG_RDS_SYS = 0x01` (FM ON)
3. Wait `V4L2_FM_ENABLE_DELAY` (300ms)
4. Set region: write `FM_REG_FM_CTRL` (band, stereo mode)
5. Set audio control: write `FM_REG_AUD_CTL0` (I2S on, de-emphasis, unmute)
6. If `FM_AUDIO_I2S`: send VSC 0xFC61 to configure I2S pin routing
7. Set volume: write `FM_REG_VOLUME_CTRL` (default 150)
8. Set interrupt mask: write `FM_REG_FM_RDS_MSK`
9. Tune to frequency:
   a. Write `FM_REG_FM_FREQ` = `FM_GET_FREQ(freq)`
   b. Write `FM_REG_FM_RDS_MSK` with tune complete + fail bits
   c. Write `FM_REG_SCH_TUNE = 0x01` (preset mode) to trigger tune
   d. Wait for completion (5s timeout) or FM interrupt event

### Seek Sequence

From `fm_rx_seek_station()`:

1. Set `FM_SEARCH_METHOD = 0x00` (normal scan mode)
2. Set `FM_REG_PRESET_MAX = 0` (no preset limit)
3. Set `FM_REG_SCH_CTL0` = RSSI threshold | direction mask
4. Freeze interrupt: set `FM_RDS_FLAG_SCH_FRZ_BIT`
5. Write `FM_REG_FM_FREQ` = start frequency
6. Set interrupt mask for tune complete/fail
7. Write `FM_REG_SCH_TUNE = 0x02` (seek mode)
8. Wait for completion (20s timeout) or FM interrupt
9. On interrupt: read `FM_REG_FM_RDS_FLAG`, check tune complete/fail bits
10. If seek error + wrap enabled: restart from band boundary
11. Read current frequency and RSSI

### RDS Read Sequence

1. Enable RDS: write `FM_REG_RDS_SYS = 0x03` (FM + RDS ON)
2. Set RDS type: write `FM_REG_RDS_CTL0` (RDS=0x00 or RBDS=0x01)
3. Set RDS waterline: write `FM_REG_RDS_WLINE`
4. Enable RDS FIFO interrupt in `FM_REG_FM_RDS_MSK`
5. On RDS interrupt: read `FM_REG_RDS_DATA` (up to 64 tuples × 3 bytes = 192 bytes)
6. Parse tuples into circular buffer (V4L2 `read()` returns 3-byte RDS blocks)
7. Decode group 0A/0B for PSN, group 2A for RT, etc.

### Driver Constants and Timeouts

```c
FM_DRV_TX_TIMEOUT      = 5*HZ   (5 seconds — command response timeout)
FM_DRV_RX_SEEK_TIMEOUT = 20*HZ  (20 seconds — seek timeout)
V4L2_FM_ENABLE_DELAY   = 300    (300ms — FM enable settling time)
FM_DEFAULT_RX_VOLUME   = 150    (default volume, range 0-255)
DEF_V4L2_FM_NFE        = 93     (noise floor estimation default)
DEF_V4L2_FM_RSSI       = 0x55   (RSSI threshold 85 dBm)
DEF_V4L2_FM_SIGNAL_STRENGTH = 105
FM_RDS_FIFO_MAX        = 240    (max bytes per RDS FIFO read)
FM_RDS_UPD_TUPLE       = 64     (tuples per read, 64×3=192 bytes)
```

### V4L2 Capabilities Exposed

```c
V4L2_CAP_HW_FREQ_SEEK | V4L2_CAP_TUNER | V4L2_CAP_RADIO |
V4L2_CAP_MODULATOR | V4L2_CAP_AUDIO | V4L2_CAP_READWRITE | V4L2_CAP_RDS_CAPTURE

V4L2_TUNER_CAP_STEREO | V4L2_TUNER_CAP_LOW | V4L2_TUNER_CAP_RDS
```

Custom ioctls (beyond standard V4L2):
```c
IOCTL_GET_PI_CODE     _IOR(100, 0, char *)
IOCTL_GET_TP_CODE     _IOR(100, 1, void *)
IOCTL_GET_PTY_CODE    _IOR(100, 2, char *)
IOCTL_GET_TA_CODE     _IOR(100, 3, char *)
IOCTL_GET_MS_CODE     _IOR(100, 4, char *)
IOCTL_GET_PS_CODE     _IOR(100, 5, char *)
IOCTL_GET_RT_MSG      _IOR(100, 6, char *)
IOCTL_GET_CT_DATA     _IOR(100, 7, char *)
IOCTL_GET_TMC_CHANNEL _IOR(100, 8, char *)
```

---

## Known Issues in Kernel Driver

### Critical

1. **Makefile bug**: `v4l2_fm_driver/Makefile` uses `obj-$(CONFIG_V4L2_ANT_DRIVER)` instead of `obj-$(CONFIG_V4L2_FM_DRIVER)`. FM builds only because ANT is also `=y`.

2. **Transport conflict**: `CONFIG_BT_HCIUART=y` (N_HCI) and `CONFIG_LINE_DISCIPLINE_DRIVER=y` (N_BRCM_HCI=26) are both enabled. They are different ldisc drivers competing for the same UART.

3. **Requires UIM daemon**: `brcm_sh_ldisc` depends on a userspace daemon to install ldisc on UART, provide firmware path, and BD address via sysfs. No such daemon exists for mocha.

### Serious

4. **mutex in atomic context**: `brcm_sh_ldisc_write()` calls `mutex_lock(&cmd_credit)` which can be invoked from tasklet context (`__send_tasklet`). Sleeping lock in atomic context = kernel BUG.

5. **Missing braces in error handling** (`fmc_prepare()`, `ant_prepare()`):
```c
if (ret < 0)
    V4L2_FM_DRV_ERR("error");
    ret = -EAGAIN;    // always executes!
    return ret;       // always returns -EAGAIN
```

6. **Default region**: `DEF_V4L2_FM_WORLD_REGION = FM_REGION_NA` (North America). Should be Europe or China for mocha.

7. **`brcm_sh_ldisc` ldisc timeout**: `LDISC_TIME = 1500ms` may be too short for slow boot on Tegra K1.

8. **Global write pointer race**: `g_bcm_write` is a global function pointer set in `fmc_prepare()`, read in tasklet without locking.

---

## Proposed Approach: Userspace FM via Raw HCI

Instead of fighting the shared transport ldisc conflict, send FM commands through the existing BT HCI transport from userspace.

```
FM daemon (C, ~500 lines)
  → raw HCI socket (AF_BLUETOOTH, BTPROTO_HCI, HCI_CHANNEL_RAW)
    → HCI VSC 0xFC15 (FM register read/write)
      → BCM4354 firmware handles FM internally

Audio: BCM4354 FM I2S → RT5671 AIF4 → amixer "FM Switch" on → headphones
```

### Advantages

- **Zero kernel changes** for FM control
- **No transport conflict** — reuses existing BT HCI path
- **Audio routing already works** — RT5671 DAI link + DAPM already in kernel
- **Complete FM protocol known** — all registers, commands, sequences documented above

### PoC Test Plan

1. Ensure BT is running: `hciconfig hci0 up`
2. Send FM ON: `hcitool cmd 0x3F 0x0015 0x00 0x00 0x01 0x00`
3. Wait 300ms
4. Set audio path (I2S on): `hcitool cmd 0x3F 0x0015 0x05 0x00 0x20 0x00`
5. Set frequency 101.0 MHz:
   - reg_value = FM_GET_FREQ(10100) = (10100*10)-64000 = 37000 = 0x9088
   - `hcitool cmd 0x3F 0x0015 0x0A 0x00 0x88 0x90`
6. Trigger tune: `hcitool cmd 0x3F 0x0015 0x09 0x00 0x01 0x00`
7. Enable audio: `amixer cset name="FM Switch" on`
8. Set volume: `hcitool cmd 0x3F 0x0015 0xF8 0x00 0x96 0x00` (150)
9. Plug in headphones (FM antenna) and listen

### Open Questions

- Does bluedroid pass through vendor-specific events (0xFF subcode 0x08)?
- Can raw HCI socket coexist with Android BT stack?
- Headphone as antenna — required or optional on mocha?
- What happens to FM if BT is turned off by user?

---

## External Reference Sources

- `drivers/bluetooth/broadcom/v4l2_fm_driver/` — complete Broadcom FM protocol implementation
- `sound/soc/tegra/tegra_rt5671.c` — audio routing, DAI links, DAPM
- `sound/soc/codecs/rt5671.c` — RT5671 AIF4 codec driver
- `/Users/artem/Projects/packages_apps_bluetooth_bplus/` — Broadcom BT+FM Android app (reference, not confirmed as mocha source). Contains `FmNativehandler.java` (2766 lines) with state machine and all FM commands via `btfm_interface_t` HAL
- `arttttt/android_hardware_broadcom_fm` — V4L2 userspace HAL (Sony origin, tried, didn't work)
- HighwayStar mocha kernel commit `a3a59d14` — same driver code as in SmokeR24.1 tree
