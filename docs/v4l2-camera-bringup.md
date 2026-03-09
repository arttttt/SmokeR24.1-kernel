# V4L2 Camera Bringup for Xiaomi Mi Pad 1 (mocha) / Tegra K1 (T124)

## Goal

Bring up cameras (IMX179 rear, OV5693 front) via the clean V4L2 stack instead
of the proprietary NVIDIA NVC, using the open-source Android Camera HAL from Antmicro.

## Overall Architecture

```
+---------------------------+     +----------------------------+
|  Android Camera Framework |     |  Antmicro Camera HAL 3.0   |
+---------------------------+     |  (github: antmicro/         |
            |                     |   android-camera-hal)       |
            v                     +----------------------------+
+-----------------------------------------------------------+
|  /dev/video0, /dev/video1    (standard V4L2 ioctls)       |
+-----------------------------------------------------------+
            |
+-----------------------------------------------------------+
|  soc_camera framework (kernel)                            |
|  drivers/media/platform/soc_camera/tegra_camera/          |
|    common.c  - V4L2 videobuf, format negotiation          |
|    vi2.c     - Tegra VI2 host driver (CSI/DMA)            |
+-----------------------------------------------------------+
            |
+-----------------------------------------------------------+
|  V4L2 sensor subdev drivers  (I2C)                        |
|  drivers/media/i2c/soc_camera/                            |
|    imx179_v4l2.c   <-- NEEDS TO BE WRITTEN                |
|    ov5693_v4l2.c   <-- already exists                     |
+-----------------------------------------------------------+
            |
+-----------------------------------------------------------+
|  Hardware: MIPI CSI -> VI (Video Input) -> DMA -> RAM     |
+-----------------------------------------------------------+
```

## Current State

### What exists
- vi2.c supports T124 (`compatible = "nvidia,tegra124-vi"`, T124-specific CIL/PHY functions)
- Board file `arch/arm/mach-tegra/board-ardbeg-sensors.c` contains soc_camera infrastructure (IMX135 + AR0261 as template)
- Board file is NOT broken (revert `7eeb5338106` restored the original)
- NVC driver IMX179 (`drivers/media/platform/tegra/imx179.c`, 959 lines) contains register tables and power sequence
- V4L2 driver IMX135 (`drivers/media/i2c/soc_camera/imx135_v4l2.c`, 2426 lines) — template for IMX179
- V4L2 driver OV5693 (`drivers/media/i2c/soc_camera/ov5693_v4l2.c`) — already exists
- DT camera description in `arch/arm/boot/dts/tegra124-platforms/tegra124-mocha-camera.dtsi`
- nvhost/host1x infrastructure works (display is functional)

### What is disabled
- `camera-pcl` node in DT: `status = "disabled"` (NVIDIA proprietary stack)
- `CONFIG_SOC_CAMERA` not enabled in defconfig
- `CONFIG_VIDEO_TEGRA` (soc_camera host) not enabled in defconfig
- `CONFIG_VIDEO_IMX179` (NVC) not enabled

### What is missing
- V4L2 sensor driver for IMX179 (`imx179_v4l2.c`)
- soc_camera board info for IMX179 and OV5693 specific to mocha (BOARD_E1780)

---

## Tasks

### 1. Kernel config (defconfig)

File: `arch/arm/configs/tegra12_android_defconfig`

Add:
```
CONFIG_SOC_CAMERA=y
CONFIG_VIDEO_TEGRA=y
CONFIG_VIDEO_TEGRA_VI2=y
```

`VIDEO_TEGRA_VI2` is enabled by default when `VIDEO_TEGRA=y`, but better to be explicit.

Dependencies (already enabled in defconfig):
- `CONFIG_VIDEO_DEV=y` — present
- `CONFIG_VIDEO_V4L2_SUBDEV_API=y` — present
- `CONFIG_ARCH_TEGRA_12x_SOC=y` — present (in tegra12 base)
- `CONFIG_HAS_DMA=y` — present
- `CONFIG_HAVE_CLK=y` — present

`CONFIG_VIDEOBUF2_DMA_CONTIG` will be auto-selected via `select` in Kconfig.

Optionally, to verify the stack via Test Pattern Generator:
```
CONFIG_VIDEO_TEGRA_VI_BYPASS=y
```

### 2. Board file: register soc_camera devices

File: `arch/arm/mach-tegra/board-ardbeg-sensors.c`

In `ardbeg_camera_init()` (line 1311) add soc_camera device registration
for mocha (BOARD_E1780).

#### 2.1 Create header: `include/media/imx179_v4l2.h`

A separate platform_data struct is needed for the V4L2 version (without NVC ioctls):

```c
#ifndef __IMX179_V4L2_H__
#define __IMX179_V4L2_H__

struct imx179_v4l2_power_rail {
    struct regulator *dvdd;
    struct regulator *avdd;
    struct regulator *iovdd;
    struct regulator *ext_reg1;
    struct regulator *ext_reg2;
};

struct imx179_v4l2_platform_data {
    const char *mclk_name;
    unsigned int cam1_gpio;
    unsigned int reset_gpio;
    unsigned int af_gpio;
    bool ext_reg;
    int (*power_on)(struct imx179_v4l2_power_rail *pw);
    int (*power_off)(struct imx179_v4l2_power_rail *pw);
};

#endif
```

#### 2.2 Add soc_camera device for IMX179

In `board-ardbeg-sensors.c` add a block following the IMX135 pattern (lines 134-171):

```c
#if IS_ENABLED(CONFIG_SOC_CAMERA_IMX179)
static int mocha_imx179_power(struct device *dev, int enable)
{
    return 0; /* power is managed by the sensor driver */
}

struct imx179_v4l2_platform_data mocha_imx179_data;

static struct i2c_board_info mocha_imx179_camera_i2c_device = {
    I2C_BOARD_INFO("imx179_v4l2", 0x10),
    .platform_data = &mocha_imx179_data,
};

static struct tegra_camera_platform_data mocha_imx179_camera_platform_data = {
    .flip_v         = 0,
    .flip_h         = 0,
    .port           = TEGRA_CAMERA_PORT_CSI_A,
    .lanes          = 4,        /* from DT: imx179 uses 4 CSI lanes */
    .continuous_clk = 0,
};

static struct soc_camera_link imx179_iclink = {
    .bus_id         = 0,
    .board_info     = &mocha_imx179_camera_i2c_device,
    .module_name    = "imx179_v4l2",
    .i2c_adapter_id = 2,        /* I2C bus 2 (from DT: busnum = <2>) */
    .power          = mocha_imx179_power,
    .priv           = &mocha_imx179_camera_platform_data,
};

static struct platform_device mocha_imx179_soc_camera_device = {
    .name   = "soc-camera-pdrv",
    .id     = 0,
    .dev    = {
        .platform_data = &imx179_iclink,
    },
};
#endif
```

Key parameters from DT (`tegra124-mocha-camera.dtsi`):
- IMX179: I2C bus 2, addr 0x10, GPIOs: 219 (reset), 221 (pwdn), 223 (af_pwdn)
- OV5693: I2C bus 2, addr 0x36, GPIOs: 222, 225, 223

#### 2.3 Register in `ardbeg_camera_init()`

```c
static int ardbeg_camera_init(void)
{
    struct board_info board_info;
    tegra_get_board_info(&board_info);

    tegra_io_dpd_enable(&csia_io);
    tegra_io_dpd_enable(&csib_io);
    tegra_io_dpd_enable(&csie_io);

    if (board_info.board_id == BOARD_E1780) {
        /* Mocha (Mi Pad 1) cameras */
#if IS_ENABLED(CONFIG_SOC_CAMERA_IMX179)
        platform_device_register(&mocha_imx179_soc_camera_device);
#endif
        /* TODO: OV5693 soc_camera device */
        return 0;
    }

    /* ... other boards as before ... */
}
```

#### 2.4 Kconfig for IMX179 soc_camera driver

File: `drivers/media/i2c/soc_camera/Kconfig` — add:

```
config SOC_CAMERA_IMX179
    tristate "Sony IMX179 V4L2 sensor support"
    depends on SOC_CAMERA && I2C
    help
      This is a V4L2 SoC camera driver for the Sony IMX179 8MP sensor.
```

File: `drivers/media/i2c/soc_camera/Makefile` — add:

```
obj-$(CONFIG_SOC_CAMERA_IMX179)  += imx179_v4l2.o
```

Add to defconfig:
```
CONFIG_SOC_CAMERA_IMX179=y
```

---

### 3. Write V4L2 sensor driver: `imx179_v4l2.c`

File: `drivers/media/i2c/soc_camera/imx179_v4l2.c`

#### 3.1 Structure (based on imx135_v4l2.c template)

```
imx179_v4l2.c
├── Defines and register addresses
│   ├── IMX179_FRAME_LENGTH_ADDR_MSB/LSB  (0x0340/0x0341)
│   ├── IMX179_COARSE_TIME_ADDR_MSB/LSB   (0x0202/0x0203)
│   └── IMX179_GAIN_ADDR                   (0x0205)
├── Mode tables (register sequences)
│   ├── mode_3280x2464 -- TAKE FROM NVC imx179.c (line 76)
│   └── (optionally) reduced-resolution modes
├── Power management
│   ├── imx179_power_on()  -- CSI DPD, regulators, GPIOs
│   ├── imx179_power_off()
│   └── GPIO: 219 (reset), 221 (cam1_pwdn), 223 (af_pwdn)
├── I2C read/write helpers
├── V4L2 subdev ops
│   ├── .s_mbus_fmt   -- set format/resolution
│   ├── .g_mbus_fmt   -- get current format
│   ├── .try_mbus_fmt -- validate format
│   ├── .enum_mbus_fmt -- enumerate formats
│   ├── .s_stream     -- start/stop streaming
│   └── .enum_framesizes
├── V4L2 controls (gain, exposure, frame_length)
├── probe() -- init, power_get, v4l2_i2c_subdev_init()
└── remove()
```

#### 3.2 Sources for register tables

Register tables for IMX179 taken from the NVC driver:
- `drivers/media/platform/tegra/imx179.c` line 76: `mode_3280x2464[]`
- Register addresses match IMX135 (same Sony format)
- Frame length, coarse time, gain — addresses from NVC header

#### 3.3 Pixel format (CRITICAL)

IMX179 is a RAW Bayer sensor. It outputs:
- `V4L2_MBUS_FMT_SRGGB10_1X10` (RAW10 RGGB) — primary
- `V4L2_MBUS_FMT_SRGGB8_1X8` (RAW8 RGGB) — simplified

vi2.c + common.c support both variants and will expose them to userspace as
`V4L2_PIX_FMT_SRGGB10` / `V4L2_PIX_FMT_SRGGB8`.

**PROBLEM**: Antmicro HAL expects **YUV (UYVY/YUYV)**, not Bayer RAW.

**Options:**
1. Modify Antmicro HAL — add Bayer demosaic (software, simple bilinear)
2. Use RAW8 + simple demosaic in HAL
3. Check if sensor supports hardware ISP/YUV mode (IMX179 — no)
4. Try TPG mode in vi2 — it can generate YUV patterns directly

#### 3.4 Power sequence (from DT tegra124-mocha-camera.dtsi)

```
Power ON:
1. CAMERA_IND_CLK_SET(10000)     -- enable MCLK 10MHz
2. CAMERA_GPIO_SET(223)          -- AF power up
3. CAMERA_GPIO_CLR(219)          -- reset low
4. CAMERA_GPIO_CLR(221)          -- cam1 power down low
5. CAMERA_WAITUS(10)
6. CAMERA_REGULATOR_ON(0..4)     -- vana, vdig, vif, ext_reg1, ext_reg2
7. CAMERA_WAITMS(5)
8. CAMERA_GPIO_SET(219)          -- reset high
9. CAMERA_WAITUS(300)            -- stabilization

Power OFF:
1. CAMERA_IND_CLK_CLR            -- disable MCLK
2. CAMERA_GPIO_CLR(221)          -- cam1 power down
3. CAMERA_GPIO_CLR(219)          -- reset low
4. CAMERA_GPIO_CLR(223)          -- AF power down
5. CAMERA_WAITUS(10)
6. CAMERA_REGULATOR_OFF(4..0)    -- disable regulators in reverse order
```

Regulators (from DT):
- `vana` — analog power
- `vdig` — digital power
- `vif` — I/O power
- `imx179_reg1`, `imx179_reg2` — additional regulators

---

### 4. DT compatibility check

File: `arch/arm/boot/dts/tegra124-platforms/tegra124-mocha-camera.dtsi`

The `camera-pcl` node with `status = "disabled"` is the NVIDIA proprietary stack.
For V4L2/soc_camera it is **NOT NEEDED**; the binding goes through the board file.

Make sure the main DT (`tegra124-tn8.dtsi` or `tegra124-soc-base.dtsi`)
has the VI node:
```dts
vi {
    compatible = "nvidia,tegra124-vi";
    /* ... */
};
```

This needs to be verified. If the node is missing — add it.

The `tegra-camera-platform` node (compatible = `"nvidia, tegra-camera-platform"`) is
for `tegra_camera_platform.c` (isomgr bandwidth management). Not critical for
basic bringup, but without it vi2 cannot manage EMC bandwidth.

---

### 5. TPG (Test Pattern Generator) — first verification step

vi2.c has a module parameter `tpg_mode`:
```
module_param(tpg_mode, int, 0644);
```

This allows verifying the entire V4L2 stack **WITHOUT a real sensor**:
1. Enable `SOC_CAMERA`, `VIDEO_TEGRA`, `VIDEO_TEGRA_VI2` in defconfig
2. Build the kernel
3. Boot, check if `/dev/videoX` appears
4. Load vi module with `tpg_mode=1`
5. Try capturing a frame via v4l2-ctl or HAL

If TPG works — the stack is alive, a real sensor can be connected.

---

### 6. Antmicro Camera HAL

Repository: https://github.com/antmicro/android-camera-hal

#### Characteristics
- Camera HAL 3.0 / Module 2.3
- Opens `/dev/video0`
- Standard V4L2 ioctls: `VIDIOC_S_FMT`, `VIDIOC_REQBUFS`, `VIDIOC_QBUF/DQBUF`, `VIDIOC_STREAMON/OFF`
- Supports UYVY or YUYV input
- RGBA and JPEG conversion is software-based
- Tested on Tegra K1
- Supports only 1 camera
- Maximum resolution: 1920x1080

#### Limitations for IMX179
- IMX179 outputs RAW Bayer, HAL expects YUV — **modification required**
- Single camera — a second camera (OV5693) requires extending the HAL
- 1920x1080 limit — IMX179 can do 3280x2464, but 1080p is enough for the start
- No parameter control (gain, exposure) — everything is hardcoded in HAL

#### Required HAL modifications
1. Add RAW Bayer pixel format support (`V4L2_PIX_FMT_SRGGB8/10`)
2. Implement Bayer demosaic (RGGB -> RGB) — simple bilinear for the start
3. Change `V4L2DEVICE_PIXEL_FORMAT` in `Android.mk`
4. Optionally: dual camera support (`/dev/video0` + `/dev/video1`)

---

## Work Order

### Phase 1: Stack verification (TPG)
1. [ ] Enable `SOC_CAMERA`, `VIDEO_TEGRA`, `VIDEO_TEGRA_VI2` in defconfig
2. [ ] Verify DT node for `nvidia,tegra124-vi` exists
3. [ ] Build kernel
4. [ ] Boot, check dmesg for vi2 probe
5. [ ] Verify `/dev/videoX` exists
6. [ ] Test TPG mode

### Phase 2: IMX179 V4L2 driver
1. [ ] Create `include/media/imx179_v4l2.h`
2. [ ] Create `drivers/media/i2c/soc_camera/imx179_v4l2.c` based on `imx135_v4l2.c`
3. [ ] Port register tables from NVC `drivers/media/platform/tegra/imx179.c`
4. [ ] Adapt power sequence from DT `tegra124-mocha-camera.dtsi`
5. [ ] Add Kconfig + Makefile entries
6. [ ] Add soc_camera_link in `board-ardbeg-sensors.c` for BOARD_E1780
7. [ ] Add `CONFIG_SOC_CAMERA_IMX179=y` to defconfig
8. [ ] Build and verify probe in dmesg
9. [ ] Test frame capture via v4l2-ctl

### Phase 3: Antmicro HAL
1. [ ] Build HAL in Android tree
2. [ ] Modify HAL for Bayer RAW (or test with TPG YUV)
3. [ ] Test preview in Camera app
4. [ ] Debug format/resolution

### Phase 4 (optional): OV5693
1. [ ] Add soc_camera_link for OV5693 in board file
2. [ ] Enable `CONFIG_SOC_CAMERA_OV5693=y`
3. [ ] Extend HAL for second camera

---

## Key Files

| File | Purpose |
|------|---------|
| `arch/arm/configs/tegra12_android_defconfig` | Kernel config |
| `arch/arm/mach-tegra/board-ardbeg-sensors.c` | Board file, soc_camera registration |
| `drivers/media/platform/soc_camera/tegra_camera/vi2.c` | VI2 host driver (CSI/DMA) |
| `drivers/media/platform/soc_camera/tegra_camera/common.c` | soc_camera videobuf/format |
| `drivers/media/i2c/soc_camera/imx135_v4l2.c` | Template V4L2 sensor driver |
| `drivers/media/platform/tegra/imx179.c` | NVC IMX179 (source of register tables) |
| `include/media/imx179.h` | NVC IMX179 header (platform_data, power_rail) |
| `arch/arm/boot/dts/tegra124-platforms/tegra124-mocha-camera.dtsi` | Camera DT (GPIOs, regulators, power sequence) |
| `include/media/tegra_v4l2_camera.h` | CSI port enum, tegra_camera_platform_data |
| `drivers/video/tegra/camera/tegra_camera_platform.c` | Camera bandwidth manager (isomgr) |

## Hardware Parameters for Mocha Cameras

### IMX179 (rear, 8MP)
- I2C bus: 2, addr: 0x10
- CSI port: A (presumably, 4 lanes)
- Resolution: 3280x2464
- Format: RAW10 Bayer RGGB
- GPIOs: 219 (reset), 221 (cam1_pwdn), 223 (af_pwdn)
- Regulators: vana, vdig, vif, imx179_reg1, imx179_reg2
- Clock: mclk (10 MHz from DT)
- Detect register: addr 0x0002, expect 0x8179 (mask 0xFFFF)
- Device ID: 0x0179

### OV5693 (front, 5MP)
- I2C bus: 2, addr: 0x36
- CSI port: B (presumably)
- GPIOs: 222, 225, 223
- Regulators: dvdd, avdd_ov5693, vdd_cam_1v2
- Clock: mclk2
- Detect register: addr 0x300A, expect 0x5690 (mask 0xFFFF)
- Device ID: 0x5693

### AD5823 (autofocus actuator for IMX179)
- I2C bus: 2, addr: 0x0C
- Regulators: vdd, vdd_i2c
- Device ID: 0x5823
