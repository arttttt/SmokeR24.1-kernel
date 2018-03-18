/*
 * BQ27x00 battery driver
 *
 * Copyright (C) 2008 Rodolfo Giometti <giometti@linux.it>
 * Copyright (C) 2008 Eurotech S.p.A. <info@eurotech.it>
 * Copyright (C) 2010-2011 Lars-Peter Clausen <lars@metafoo.de>
 * Copyright (C) 2011 Pali Rohár <pali.rohar@gmail.com>
 * Copyright (C) 2011 NVIDIA Corporation.
 *
 * Based on a previous work by Copyright (C) 2008 Texas Instruments, Inc.
 * Copyright (C) 2016 XiaoMi, Inc.
 * Copyright (C) 2018 Artyom Bambalov <artem-bambalov@yandex.ru>
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 */

/*
 * Datasheets:
 * http://focus.ti.com/docs/prod/folders/print/bq27000.html
 * http://focus.ti.com/docs/prod/folders/print/bq27500.html
 * http://www.ti.com/product/bq27425-g1
 */

#include <linux/module.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/idr.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/firmware.h>
#include <asm/unaligned.h>
#include <linux/switch.h>
#include <linux/interrupt.h>
#include <linux/power/battery-charger-gauge-comm.h>
#include <linux/of_gpio.h>

#include <linux/power/bq27x00_battery.h>
#include <linux/mfd/palmas.h>

#define DRIVER_VERSION			"1.3.0"

#define G3_FW_VERSION			0x0324
#define G4_FW_VERSION			0x0329
#define L1_600_FW_VERSION		0x0600
#define L1_604_FW_VERSION		0x0604

#define CONTROL_CMD			0x00
/* Subcommands of Control() */
#define DEV_TYPE_SUBCMD			0x0001
#define FW_VER_SUBCMD			0x0002
#define BAT_INSERT_SUBCMD		0x000D
#define DF_VER_SUBCMD			0x001F
#define IT_ENABLE_SUBCMD		0x0021
#define RESET_SUBCMD			0x0041

#define INVALID_REG_ADDR		0xFF

#define DEBUG_1HZ_MAX_COUNT			15

#define SYSDOWN_BIT             (1<<1)

#define DEBUG_UPDATE_COMMAND 1
#define UPDATE_CMD_BAT_INS 2
#define MAX_DATA_LEN 200
#define CTL_CMD_DELAY 100

enum bq27x00_reg_index {
	BQ27x00_REG_TEMP = 0,
	BQ27x00_REG_INT_TEMP,
	BQ27x00_REG_VOLT,
	BQ27x00_REG_AI,
	BQ27x00_REG_FLAGS,
	BQ27x00_REG_TTE,
	BQ27x00_REG_TTF,
	BQ27x00_REG_TTES,
	BQ27x00_REG_TTECP,
	BQ27x00_REG_NAC,
	BQ27x00_REG_LMD,
	BQ27x00_REG_CC,
	BQ27x00_REG_AE,
	BQ27x00_REG_RSOC,
	BQ27x00_REG_ILMD,
	BQ27x00_REG_SOC,
	BQ27x00_REG_DCAP,
	BQ27x00_REG_CTRL,
	BQ27x00_REG_AR,
	BQ27x00_REG_ARTE,
	BQ27x00_REG_FAC,
	BQ27x00_REG_RM,
	BQ27x00_REG_FCC,
	BQ27x00_REG_STBYI,
	BQ27x00_REG_SOH,
	BQ27x00_REG_INSTI,
	BQ27x00_REG_RSCLE,
	BQ27x00_REG_OC,
	BQ27x00_REG_TRUECAP,
	BQ27x00_REG_TRUEFCC,
	BQ27x00_REG_TRUESOC,
/* TI L1 firmware (v6.03) extra registers */
	BQ27x00_REG_MAX_CURRENT,
	BQ27x00_REG_QPASSED_HIRES_INT,
	BQ27x00_REG_QPASSED_HIRES_FRACTION,
};

enum{
	CMD = 0,
	LEN,
	ADDR,
	REG
};

/* TI G3 Firmware (v3.24) */
static u8 bq27x00_fw_g3_regs[] = {
	0x06,
	0x36,
	0x08,
	0x14,
	0x0A,
	0x16,
	0x18,
	0x1c,
	0x26,
	0x0C,
	0x12,
	0x2A,
	0x22,
	0x0B,
	0x76,
	0x2C,
	0x3C,
	0x00,
	0xFF,
	0x0E,
	0x10,
	0x12,
	0x28,
	0x2A,
	0x3A,
	0xFF,
	0xFF,
	0xFF,
};

/* TI G4 Firmware (v3.24) */
static u8 bq27x00_fw_g4_regs[] = {
	0x06,
	0x28,
	0x08,
	0x14,
	0x0A,
	0x16,
	0xFF, /* TTF */
	0x1A,
	0xFF, /* TTECP */
	0x0C,
	0xFF, /* LMD */
	0x1E,
	0xFF, /* AE */
	0xFF, /* RSOC */
	0xFF, /* ILMD */
	0x20,
	0x2E,
	0x00,
	0x02,
	0x04,
	0x0E,
	0x10,
	0x12,
	0x18,
	0x1C,
	0x22,
	0x2A,
	0x2C,
	0x6C,
	0x70,
	0x74,
	0xFF, /* BQ27x00_REG_MAX_CURRENT */
	0xFF, /* BQ27x00_REG_QPASSED_HIRES_INT */
	0xFF, /* BQ27x00_REG_QPASSED_HIRES_FRACTION */
};

/*
 * TI L1 firmware (v6.00)
 * Some of the commented registers are missing in this fw.
 * Mark them as 0xFF for being invalid
 */
static u8 bq27x00_fw_l1_regs[] = {
	0x06,
	0x28,
	0x08,
	0x14,
	0x0A,
	0x16,
	0xFF, /* TTF */
	0x1A,
	0xFF, /* TTECP */
	0x0C,
	0xFF, /* LMD */
	0x1E,
	0xFF, /* AE */
	0xFF, /* RSOC */
	0xFF, /* ILMD */
	0x20,
	0x2E,
	0x00,
	0x02,
	0x04,
	0x0E,
	0x10,
	0x12,
	0x18,
	0x1C,
	0x22,
	0x2A,
	0x2C,
	0x6C,
	0x70,
	0x74,
/* TI L1 firmware (v6.04) extra registers */
	0x76, /* BQ27x00_REG_MAX_CURRENT */
	0x24, /* BQ27x00_REG_QPASSED_HIRES_INT */
	0x26, /* BQ27x00_REG_QPASSED_HIRES_FRACTION */

};

#define BQ27000_FLAG_CHGS		BIT(7)
#define BQ27000_FLAG_FC			BIT(5)

#define BQ27500_FLAG_DSC		BIT(0)
#define BQ27500_FLAG_FC			BIT(9)

#define BQ27000_RS			20 /* Resistor sense */

#define LG_ID_MIN 850
#define LG_ID_MAX 1100
#define ATL_ID_MIN 100
#define ATL_ID_MAX 300

struct bq27x00_device_info;
struct bq27x00_access_methods {
	int (*read)(struct bq27x00_device_info *di, u8 reg, bool single);
	int (*write)(struct bq27x00_device_info *di, u8 reg, int value,
			bool single);
};

static int bq27x00_control_cmd(struct bq27x00_device_info *di, u16 cmd);
static void bq27x00_reset_registers(struct bq27x00_device_info *di);
static int bq27x00_battery_read_control_reg(struct bq27x00_device_info *di);
static int bq27x00_read_block_i2c(struct bq27x00_device_info *di, u8 reg,
	unsigned char *buf, size_t len);
static int bq27x00_battery_temperature(struct bq27x00_device_info *di,
	union power_supply_propval *val);

enum bq27x00_chip { BQ27000, BQ27500 };

struct bq27x00_reg_cache {
	int temperature;
	int internal_temp;
	int time_to_empty;
	int time_to_empty_avg;
	int time_to_full;
	int charge_full;
	int cycle_count;
	int capacity;
	int raw_capacity;
	int flags;
	int control;
	int voltage;
	int full_avail_cap;
	int remain_cap;
	int full_charge_cap;
	int average_i;
	int state_of_health;
	int state_of_charge;
	int instant_i;
	int r_scale;
	int true_cap;
	int true_fcc;
	int true_soc;
	int nom_avail_cap;
	int current_now;
	short q_max;
	short q_passed;
	unsigned short DOD0;
	short q_start;
	struct timespec timestamp;
	unsigned short DODfinal;
	short delta_v;
	unsigned short max_current;
	unsigned short q_passed_hires_int;
	unsigned short q_passed_hires_fraction;
	short max_dod_diff;
	short ambient_temp;
	unsigned short regr_dod;
	short regr_res;
	short rnew;
	short dod_diff;
	unsigned short sleeptime;
	short sim_temp;

};

struct bq27x00_partial_data_flash {
	struct timespec timestamp;
	char data_ram[200];
	char subclass_0x52[150];
	char subclass_0x57[120];
	char subclass_0x58[120];
	char subclass_0x5b[120];
	char subclass_0x5c[120];
	char subclass_0x5d[120];
	char subclass_0x5e[120];
};


struct bq27x00_device_info {
	int			id;
	enum bq27x00_chip	chip;

	struct bq27x00_platform_data	*pdata;
	struct i2c_client		*client;

	struct bq27x00_reg_cache cache;
	int charge_design_full;
	int fake_battery;

	int (*translate_temp)(int temperature);

	unsigned long last_update;
	struct delayed_work work;

	struct power_supply	bat;

	struct bq27x00_access_methods bus;

	struct bq27x00_partial_data_flash partial_df;

	struct mutex lock;

	u8 *regs;
	int fw_ver;
	int df_ver;

	struct switch_dev sdev;
	struct battery_gauge_dev	*bg_dev;
	int bat_status;
	int is_rom_mode;
};

struct bq27x00_firmware_data {
	int vendor;
	const u8 *data;
	int size;
};

static enum power_supply_property bq27x00_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_ENERGY_NOW
};

static unsigned int poll_interval = 60;
module_param(poll_interval, uint, 0644);
MODULE_PARM_DESC(poll_interval, "battery poll interval in seconds - " \
				"0 disables polling");
/*
 * Common code for BQ27x00 devices
 */

static int is_between(int left, int right, int value)
{
	if (left >= right && left >= value && value >= right)
		return 1;
	if (left <= right && left <= value && value <= right)
		return 1;

	return 0;
}

static inline int bq27x00_read(struct bq27x00_device_info *di, int reg_index,
		bool single)
{
	int val;

	/* Reports 0 for invalid/missing registers */
	if (!di || !di->regs || di->regs[reg_index] == INVALID_REG_ADDR)
		return 0;

	val = di->bus.read(di, di->regs[reg_index], single);

	return val;
}

static inline int bq27x00_write(struct bq27x00_device_info *di, int reg_index,
		int value, bool single)
{
	if (!di || !di->regs || di->regs[reg_index] == INVALID_REG_ADDR)
		return -EPERM;

	return di->bus.write(di, di->regs[reg_index], value, single);
}

/*
 * Return the battery Raw State-of-Charge
 * Or < 0 if something fails.
 */
static int bq27x00_battery_read_raw_soc(struct bq27x00_device_info *di)
{
	int rsoc;

	rsoc = bq27x00_read(di, BQ27x00_REG_TRUESOC, false);

	if (rsoc < 0)
		dev_err(&di->client->dev, "error reading raw State-of-Charge\n");

	return rsoc;
}

/*
 * Return the battery Relative State-of-Charge
 * Or < 0 if something fails.
 */
static int bq27x00_battery_read_rsoc(struct bq27x00_device_info *di)
{
	int rsoc;

	if (di->chip == BQ27500)
		rsoc = bq27x00_read(di, BQ27x00_REG_SOC, false);
	else
		rsoc = bq27x00_read(di, BQ27x00_REG_RSOC, true);

	if (rsoc < 0)
		dev_err(&di->client->dev, "error reading relative State-of-Charge\n");

	return rsoc;
}

/*
 * Return a battery charge value in µAh
 * Or < 0 if something fails.
 */
static int bq27x00_battery_read_charge(struct bq27x00_device_info *di, u8 reg)
{
	int charge;

	charge = bq27x00_read(di, reg, false);
	if (charge < 0) {
		dev_err(&di->client->dev, "error reading nominal available capacity\n");
		return charge;
	}

	if (di->chip == BQ27500)
		charge *= 1000;
	else
		charge = charge * 3570 / BQ27000_RS;

	return charge;
}

/*
 * Return the battery Nominal available capaciy in µAh
 * Or < 0 if something fails.
 */
static inline int bq27x00_battery_read_nac(struct bq27x00_device_info *di)
{
	return bq27x00_battery_read_charge(di, BQ27x00_REG_NAC);
}

/*
 * Return the battery Last measured discharge in µAh
 * Or < 0 if something fails.
 */
static inline int bq27x00_battery_read_lmd(struct bq27x00_device_info *di)
{
	return bq27x00_battery_read_charge(di, BQ27x00_REG_LMD);
}

/*
 * Return the battery Initial last measured discharge in µAh
 * Or < 0 if something fails.
 */
static int bq27x00_battery_read_ilmd(struct bq27x00_device_info *di)
{
	int ilmd;

	if (di->chip == BQ27500)
		ilmd = bq27x00_read(di, BQ27x00_REG_DCAP, false);
	else
		ilmd = bq27x00_read(di, BQ27x00_REG_ILMD, true);

	if (ilmd < 0) {
		dev_err(&di->client->dev, "error reading initial last measured discharge\n");
		return ilmd;
	}

	if (di->chip == BQ27500)
		ilmd *= 1000;
	else
		ilmd = ilmd * 256 * 3570 / BQ27000_RS;

	return ilmd;
}

/*
 * Return the battery Cycle count total
 * Or < 0 if something fails.
 */
static int bq27x00_battery_read_cyct(struct bq27x00_device_info *di)
{
	int cyct;

	cyct = bq27x00_read(di, BQ27x00_REG_CC, false);
	if (cyct < 0)
		dev_err(&di->client->dev, "error reading cycle count total\n");

	return cyct;
}

/*
 * Read a time register.
 * Return < 0 if something fails.
 */
static int bq27x00_battery_read_time(struct bq27x00_device_info *di, u8 reg)
{
	int tval;

	tval = bq27x00_read(di, reg, false);
	if (tval < 0) {
		dev_err(&di->client->dev, "error reading register %02x: %d\n", reg, tval);
		return tval;
	}

	if (tval == 65535)
		return -ENODATA;

	return tval * 60;
}

static void bq27x00_update(struct bq27x00_device_info *di)
{
	struct bq27x00_reg_cache cache = {0, };
	bool is_bq27500 = di->chip == BQ27500;
	unsigned char block_data[26];
	unsigned char block_addr;
	int block_len;
	struct timespec ts;
	union power_supply_propval val;
	int temp;

	cache.flags = bq27x00_read(di, BQ27x00_REG_FLAGS, false);

	if (cache.flags >= 0) {
		getnstimeofday(&ts);
		cache.timestamp = ts;
		cache.capacity = bq27x00_battery_read_rsoc(di);
		cache.raw_capacity = bq27x00_battery_read_raw_soc(di);
		cache.temperature = bq27x00_read(di, BQ27x00_REG_TEMP, false);
		cache.internal_temp = bq27x00_read(di, BQ27x00_REG_INT_TEMP, false);
		cache.time_to_empty = bq27x00_battery_read_time(di, BQ27x00_REG_TTE);
		cache.time_to_empty_avg = bq27x00_battery_read_time(di, BQ27x00_REG_TTES);
		cache.time_to_full = bq27x00_battery_read_time(di, BQ27x00_REG_TTF);
		cache.charge_full = bq27x00_battery_read_lmd(di);
		cache.cycle_count = bq27x00_battery_read_cyct(di);
		cache.control = bq27x00_battery_read_control_reg(di);
		cache.voltage = bq27x00_read(di, BQ27x00_REG_VOLT, false);
		cache.nom_avail_cap = bq27x00_read(di, BQ27x00_REG_NAC, false);
		cache.full_avail_cap = bq27x00_read(di, BQ27x00_REG_FAC, false);
		cache.full_charge_cap = bq27x00_read(di, BQ27x00_REG_FCC, false);
		cache.average_i = bq27x00_read(di, BQ27x00_REG_AI, false);
		cache.remain_cap = bq27x00_read(di, BQ27x00_REG_RM, false);
		cache.state_of_health = bq27x00_read(di, BQ27x00_REG_SOH, false);
		cache.instant_i = bq27x00_read(di, BQ27x00_REG_INSTI, false);
		cache.r_scale = bq27x00_read(di, BQ27x00_REG_RSCLE, false);
		cache.true_cap = bq27x00_read(di, BQ27x00_REG_TRUECAP, false);
		cache.true_fcc = bq27x00_read(di, BQ27x00_REG_TRUEFCC, false);
		cache.true_soc = bq27x00_read(di, BQ27x00_REG_TRUESOC, false);

		if (di->fw_ver >= L1_604_FW_VERSION) {
			block_addr = 0x61;
			block_len  = 11;
			if (bq27x00_read_block_i2c(di, block_addr, block_data, block_len) < 0) {
				dev_err(&di->client->dev,
					"error block reading debug registers @ 0x%x\n", block_addr);
			} else {
				cache.q_max = (short) get_unaligned_le16(block_data+1);
				cache.q_passed = (short) get_unaligned_le16(block_data+3);
				cache.DOD0 = get_unaligned_le16(block_data+5);
				cache.q_start = (short) get_unaligned_le16(block_data+7);
				cache.DODfinal = get_unaligned_le16(block_data+9);
			}
		}

		if (di->fw_ver >= L1_604_FW_VERSION) {
			cache.max_current = bq27x00_read(di, BQ27x00_REG_MAX_CURRENT, false);

			block_addr = 0x24;
			block_len  = 26;
			if (bq27x00_read_block_i2c(di, block_addr, block_data, block_len) < 0) {
				dev_err(&di->client->dev,
					"error block reading debug registers @ 0x%x\n", block_addr);
			} else {
				cache.q_passed_hires_int = get_unaligned_le16(block_data);
				cache.q_passed_hires_fraction = get_unaligned_le16(block_data+3);
				cache.max_dod_diff = (short) get_unaligned_le16(block_data+8);
				cache.ambient_temp = (short) get_unaligned_le16(block_data+10);
				cache.delta_v = (short) get_unaligned_le16(block_data+12);
				cache.regr_dod = get_unaligned_le16(block_data+14);
				cache.regr_res = (short) get_unaligned_le16(block_data+16);
				cache.rnew = (short) get_unaligned_le16(block_data+18);
				cache.dod_diff = (short) get_unaligned_le16(block_data+20);
				cache.sleeptime = get_unaligned_le16(block_data+22);
				cache.sim_temp = (short) get_unaligned_le16(block_data+24);
			}
		}

		if (!is_bq27500)
			cache.current_now = bq27x00_read(di, BQ27x00_REG_AI, false);

		/* We only have to read charge design full once */
		if (di->charge_design_full <= 0)
			di->charge_design_full = bq27x00_battery_read_ilmd(di);
	}

	/* Ignore current_now which is a snapshot of the current battery state
	 * and is likely to be different even between two consecutive reads */
	if (memcmp(&di->cache, &cache, sizeof(cache) - sizeof(int)) != 0 ||
		cache.capacity <= 0) {
		di->cache = cache;
		power_supply_changed(&di->bat);
	}

	/* Dump the battery info */
	bq27x00_battery_temperature(di, &val);
	temp = val.intval;

	pr_info("bq27x00 BMS soc %d %d v %d i %d t %d c %d rm %d %d fcc %d %d flag %x\n",
		di->cache.capacity, di->cache.true_soc,
		di->cache.voltage * 1000,
		(int)((s16)di->cache.average_i * (-1000)),
		temp, di->cache.cycle_count,
		di->cache.remain_cap, di->cache.true_cap,
		di->cache.full_charge_cap, di->cache.true_fcc,
		di->cache.flags);

	di->last_update = jiffies;
}

static void bq27x00_battery_poll(struct work_struct *work)
{
	struct bq27x00_device_info *di =
		container_of(work, struct bq27x00_device_info, work.work);
	int delay;

	if (poll_interval > 0) {
		bq27x00_update(di);
		if (di->cache.capacity < 20)
			delay = min((unsigned int)20, poll_interval);
		else
			delay = max((unsigned int)1, poll_interval);
		/* The timer does not have to be accurate. */
		set_timer_slack(&di->work.timer, delay * HZ / 4);
		queue_delayed_work(system_power_efficient_wq,
							&di->work, delay * HZ);
	}

	return;
}


/*
 * Return the battery temperature in tenths of degree Celsius
 * Or < 0 if something fails.
 */
static int bq27x00_battery_temperature(struct bq27x00_device_info *di,
	union power_supply_propval *val)
{
	int temperature;

	if (di->cache.temperature < 0)
		return di->cache.temperature;

	if (di->chip == BQ27500)
		temperature = di->cache.temperature - 2731;
	else
		temperature = ((di->cache.temperature * 5) - 5463) / 2;

	/* let the board translate the thermistor reading if necessary */
	if (di->translate_temp)
		temperature = di->translate_temp(temperature);

	/*
	 * If the reading indicates missing/malfunctioning battery thermistor,
	 * fall back on the internal temperature reading.
	 */
	if (temperature < -350) {
		static int once = 0;

		if (!once) {
			dev_warn(&di->client->dev, "Battery thermistor missing or malfunctioning, falling back to "
					"gas gauge internal temp\n");
			once = 1;
		}

		if (di->chip == BQ27500)
			temperature = di->cache.internal_temp - 2731;
		else
			temperature = ((di->cache.internal_temp * 5) - 5463) / 2;

		/*
		 * Offset by 20 C since the board will run hotter than the battery.
		 */
		temperature -= 200;
		di->fake_battery = 1;
	} else {
		/* if we ever get a valid reading we must not have a fake battery */
		di->fake_battery = 0;
	}

	val->intval = temperature;

	return 0;
}

/*
 * Return the battery average current in µA
 * Note that current can be negative signed as well
 * Or 0 if something fails.
 */
static int bq27x00_battery_current(struct bq27x00_device_info *di,
	union power_supply_propval *val)
{
	int curr;

	if (di->chip == BQ27500)
	    curr = bq27x00_read(di, BQ27x00_REG_AI, false);
	else
	    curr = di->cache.current_now;

	if (di->chip == BQ27500) {
		/* bq27500 returns signed value */
		val->intval = (int)((s16)curr) * 1000;
		val->intval = val->intval * (-1);
	} else {
		if (di->cache.flags & BQ27000_FLAG_CHGS) {
			dev_dbg(&di->client->dev, "negative current!\n");
			curr = -curr;
		}

		val->intval = curr * 3570 / BQ27000_RS;
	}

	return 0;
}

static int bq27x00_update_battery_status(struct battery_gauge_dev *bg_dev,
		enum battery_charger_status status)
{
	struct bq27x00_device_info *di = battery_gauge_get_drvdata(bg_dev);

	if (status == BATTERY_CHARGING)
		di->bat_status = POWER_SUPPLY_STATUS_CHARGING;
	else if (status == BATTERY_CHARGING_DONE)
		di->bat_status = POWER_SUPPLY_STATUS_FULL;
	else
		di->bat_status = POWER_SUPPLY_STATUS_DISCHARGING;

	power_supply_changed(&di->bat);

	return 0;
}

static struct battery_gauge_ops bq27x00_bg_ops = {
	.update_battery_status = bq27x00_update_battery_status,
};

static struct battery_gauge_info bq27x00_bgi = {
	.cell_id = 0,
	.bg_ops = &bq27x00_bg_ops,
};

/*
 * Return the battery Voltage in milivolts
 * Or < 0 if something fails.
 */
static int bq27x00_battery_voltage(struct bq27x00_device_info *di,
	union power_supply_propval *val)
{
	int volt;

	volt = bq27x00_read(di, BQ27x00_REG_VOLT, false);
	if (volt < 0)
		return volt;

	val->intval = volt * 1000;

	return 0;
}

/*
 * Return the battery Available energy in µWh
 * Or < 0 if something fails.
 */
static int bq27x00_battery_energy(struct bq27x00_device_info *di,
	union power_supply_propval *val)
{
	int ae;

	ae = bq27x00_read(di, BQ27x00_REG_AE, false);
	if (ae < 0) {
		dev_err(&di->client->dev, "error reading available energy\n");
		return ae;
	}

	if (di->chip == BQ27500)
		ae *= 1000;
	else
		ae = ae * 29200 / BQ27000_RS;

	val->intval = ae;

	return 0;
}

/*
 * Return the coulumb counter in mAh
 * Positive value means charge is going out of the battery
 * Negative value means charge is going into the battery
 */
static int bq27x00_battery_qpassed(struct bq27x00_device_info *di,
	union power_supply_propval *val) {

	unsigned char q_data[10];
	size_t q_size = 10;

	/* This block of data must be read in one shot.
	 * Even though we are only interested 2 registers.
	 */
	bq27x00_read_block_i2c(di, 0x61, q_data, q_size);
	val->intval = (int) get_unaligned_le16(q_data+3);

	return 0;
}

static int bq27x00_battery_hires_qpassed(struct bq27x00_device_info *di,
	unsigned short *i, unsigned short *f) {

	unsigned short last_i;

	if (!i || !f)
		return -EPERM;

	last_i = bq27x00_read(di, BQ27x00_REG_QPASSED_HIRES_INT, false);

	while (1) {
		*i = bq27x00_read(di, BQ27x00_REG_QPASSED_HIRES_INT, false);
		*f = bq27x00_read(di, BQ27x00_REG_QPASSED_HIRES_FRACTION, false);
		if (*i == last_i)
			break;
		last_i = *i;
	}

	return 0;
}


static int bq27x00_simple_value(int value,
	union power_supply_propval *val)
{
	if (value < 0)
		return value;

	val->intval = value;

	return 0;
}

#define to_bq27x00_device_info(x) container_of((x), \
				struct bq27x00_device_info, bat);

static int bq27x00_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	int ret = 0;
	struct bq27x00_device_info *di = to_bq27x00_device_info(psy);

	mutex_lock(&di->lock);
	if (time_is_before_jiffies(di->last_update + 5 * HZ)) {
		cancel_delayed_work(&di->work);
		bq27x00_battery_poll(&di->work.work);
	}
	mutex_unlock(&di->lock);

	if (psp != POWER_SUPPLY_PROP_PRESENT && di->cache.flags < 0)
		return -ENODEV;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = di->bat_status;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		if (bq27x00_battery_temperature(di, val) < 0)
			val->intval = POWER_SUPPLY_HEALTH_UNKNOWN;
		if (val->intval > 550)
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		else if (val->intval < 0)
			val->intval = POWER_SUPPLY_HEALTH_COLD;
		else
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = bq27x00_battery_voltage(di, val);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = di->cache.flags < 0 ? 0 : 1;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = bq27x00_battery_current(di, val);
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		ret = bq27x00_simple_value(di->cache.raw_capacity, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		ret = bq27x00_battery_qpassed(di, val);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (di->fake_battery) {
			/* Force soc to 1 for debugging */
			val->intval = 1;
			ret = 0;
		} else {
			ret = bq27x00_simple_value(di->cache.capacity, val);
			if (di->bat_status == POWER_SUPPLY_STATUS_FULL &&
				(val->intval < 100 && val->intval > 95)) {
				pr_info("bq27x00 move %d to FULL\n", di->cache.capacity);
				val->intval = 100;
			}
		}
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = bq27x00_battery_temperature(di, val);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		ret = bq27x00_simple_value(di->cache.time_to_empty, val);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG:
		ret = bq27x00_simple_value(di->cache.time_to_empty_avg, val);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		ret = bq27x00_simple_value(di->cache.time_to_full, val);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = bq27x00_simple_value(1000 * di->cache.true_cap, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = bq27x00_simple_value(1000 * di->cache.true_fcc, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		ret = bq27x00_simple_value(di->charge_design_full, val);
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		ret = bq27x00_simple_value(di->cache.cycle_count, val);
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		ret = bq27x00_battery_energy(di, val);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static void bq27x00_external_power_changed(struct power_supply *psy)
{
	struct bq27x00_device_info *di = to_bq27x00_device_info(psy);

	cancel_delayed_work(&di->work);
	queue_delayed_work(system_power_efficient_wq,
						&di->work, 0);
}

static int bq27x00_powersupply_init(struct bq27x00_device_info *di)
{
	int ret;

	di->bat.type = POWER_SUPPLY_TYPE_BATTERY;
	di->bat.properties = bq27x00_battery_props;
	di->bat.num_properties = ARRAY_SIZE(bq27x00_battery_props);
	di->bat.get_property = bq27x00_battery_get_property;
	di->bat.external_power_changed = bq27x00_external_power_changed;

	INIT_DELAYED_WORK(&di->work, bq27x00_battery_poll);
	mutex_init(&di->lock);

	/*
	 * Read the battery temp now to prevent races between userspace reading
	 * properties and battery "detection" logic.
	 */
	di->cache.temperature = bq27x00_read(di, BQ27x00_REG_TEMP, false);
	di->cache.internal_temp = bq27x00_read(di, BQ27x00_REG_INT_TEMP, false);

	/*
	 * NOTE: Properties can be read as soon as we register the power supply.
	 */
	ret = power_supply_register(&di->client->dev, &di->bat);
	if (ret) {
		dev_err(&di->client->dev, "failed to register battery: %d\n", ret);
		return ret;
	}

	dev_info(&di->client->dev, "support ver. %s enabled\n", DRIVER_VERSION);

	return 0;
}

static void bq27x00_powersupply_unregister(struct bq27x00_device_info *di)
{
	cancel_delayed_work(&di->work);

	power_supply_unregister(&di->bat);

	mutex_destroy(&di->lock);
}

/* If the system has several batteries we need a different name for each
 * of them...
 */
static DEFINE_IDR(battery_id);
static DEFINE_MUTEX(battery_mutex);

static void bq27x00_is_rom_mode(struct bq27x00_device_info *di)
{
	struct i2c_client *client = di->client;
	struct i2c_msg msg[2];
	unsigned char data[2];
	u8 reg = 0x0;
	int ret;

	if (!client->adapter)
		return;

	msg[0].addr = 0x0b; /* The addr in ROM mode */
	msg[0].flags = 0;
	msg[0].buf = &reg;
	msg[0].len = sizeof(reg);
	msg[1].addr = 0x0b;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = data;
	msg[1].len = 1;

	/* read addr 0x0b, reg 0x0 */
	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret < 0)
		di->is_rom_mode = 0;
	else
		di->is_rom_mode = 1;
		
	return;
}

static int bq27x00_read_i2c(struct bq27x00_device_info *di, u8 reg, bool single)
{
	struct i2c_client *client = di->client;
	struct i2c_msg msg[2];
	unsigned char data[2];
	int ret;

	if (!client->adapter)
		return -ENODEV;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &reg;
	msg[0].len = sizeof(reg);
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = data;
	if (single)
		msg[1].len = 1;
	else
		msg[1].len = 2;

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret < 0)
		return ret;

	if (!single)
		ret = get_unaligned_le16(data);
	else
		ret = data[0];

	return ret;
}

static int bq27x00_write_i2c(struct bq27x00_device_info *di, u8 reg, int value, bool single)
{
	struct i2c_client *client = di->client;
	struct i2c_msg msg[2];
	unsigned char data[2];
	int ret;

	if (!client->adapter)
		return -ENODEV;

	if (!single)
		put_unaligned_le16(value, data);
	else
		data[0] = value;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &reg;
	msg[0].len = sizeof(reg);
	msg[1].addr = client->addr;
	msg[1].flags = 0;
	msg[1].buf = data;
	if (single)
		msg[1].len = 1;
	else
		msg[1].len = 2;

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret < 0)
		return ret;

	return 0;
}

static int bq27x00_control_cmd(struct bq27x00_device_info *di, u16 cmd)
{
	struct i2c_client *client = di->client;
	struct i2c_msg msg[3];
	unsigned char cmd_write[3];
	unsigned char cmd_read[2];
	int ret;

	if (!client->adapter)
		return -ENODEV;

	cmd_write[0] = 0x0;
	put_unaligned_le16(cmd, cmd_write + 1);

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = cmd_write;
	msg[0].len = sizeof(cmd_write);
	msg[1].addr = client->addr;
	msg[1].flags = 0;
	msg[1].buf = cmd_write;
	msg[1].len = 1;
	msg[2].addr = client->addr;
	msg[2].flags = I2C_M_RD;
	msg[2].buf = cmd_read;
	msg[2].len = sizeof(cmd_read);

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret < 0)
		return ret;

	ret = get_unaligned_le16(cmd_read);

	return ret;
}

static int bq27x00_read_block_i2c(struct bq27x00_device_info *di, u8 reg,
		unsigned char *buf, size_t len)
{
	struct i2c_client *client = di->client;
	struct i2c_msg msg[2];
	int ret;

	if (!client->adapter)
		return -ENODEV;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &reg;
	msg[0].len = sizeof(reg);
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = buf;
	msg[1].len = len;

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret < 0)
		return ret;

	return 0;
}

static int bq27x00_battery_reset(struct bq27x00_device_info *di)
{
	dev_info(&di->client->dev, "Gas Gauge Reset\n");

	bq27x00_write_i2c(di, CONTROL_CMD, RESET_SUBCMD, false);

	msleep(10);

	bq27x00_read_i2c(di, CONTROL_CMD, false);

	msleep(10);

	/* Reset register map based on fw version */
	bq27x00_reset_registers(di);

	return 0;
}


static int bq27x00_battery_read_fw_version(struct bq27x00_device_info *di)
{
	bq27x00_write_i2c(di, CONTROL_CMD, FW_VER_SUBCMD, false);

	msleep(10);

	return bq27x00_read_i2c(di, CONTROL_CMD, false);
}

static int bq27x00_battery_read_control_reg(struct bq27x00_device_info *di)
{
	bq27x00_write_i2c(di, CONTROL_CMD, 0 , false);

	msleep(10);

	return bq27x00_read_i2c(di, CONTROL_CMD, false);
}


static int bq27x00_battery_read_device_type(struct bq27x00_device_info *di)
{
	bq27x00_write_i2c(di, CONTROL_CMD, DEV_TYPE_SUBCMD, false);

	msleep(10);

	return bq27x00_read_i2c(di, CONTROL_CMD, false);
}

static int bq27x00_battery_read_dataflash_version(struct bq27x00_device_info *di)
{
	bq27x00_write_i2c(di, CONTROL_CMD, DF_VER_SUBCMD, false);

	msleep(10);

	return bq27x00_read_i2c(di, CONTROL_CMD, false);
}

static irqreturn_t soc_int_irq_threaded_handler(int irq, void *arg)
{
	struct bq27x00_device_info *di = arg;
	int flags;

	dev_info(&di->client->dev, "soc_int\n");

	/* the actual SysDown event is processed in the normal update path */

	mutex_lock(&di->lock);

	flags = bq27x00_read(di, BQ27x00_REG_FLAGS, false);

	if (flags & SYSDOWN_BIT) {
		dev_warn(&di->client->dev, "detected SYSDOWN condition, pulsing poweroff switch\n");
		switch_set_state(&di->sdev, 0);
		switch_set_state(&di->sdev, 1);
		cancel_delayed_work(&di->work);
		queue_delayed_work(system_power_efficient_wq,
							&di->work, 0);
	} else {
		dev_info(&di->client->dev, "SYSDOWN condition not detected\n");
		switch_set_state(&di->sdev, 0);
	}

	mutex_unlock(&di->lock);

	return IRQ_HANDLED;
}

static ssize_t show_firmware_version(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bq27x00_device_info *di = dev_get_drvdata(dev);
	int ver;

	ver = bq27x00_battery_read_fw_version(di);

	return sprintf(buf, "%d\n", ver);
}

static ssize_t show_dataflash_version(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bq27x00_device_info *di = dev_get_drvdata(dev);
	int ver;

	ver = bq27x00_battery_read_dataflash_version(di);

	return sprintf(buf, "%d\n", ver);
}

static ssize_t show_device_type(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bq27x00_device_info *di = dev_get_drvdata(dev);
	int dev_type;

	dev_type = bq27x00_battery_read_device_type(di);

	return sprintf(buf, "%d\n", dev_type);
}

static ssize_t show_reset(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bq27x00_device_info *di = dev_get_drvdata(dev);

	bq27x00_battery_reset(di);

	return sprintf(buf, "okay\n");
}

static ssize_t show_qpassed(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bq27x00_device_info *di = dev_get_drvdata(dev);
	int count;

	mutex_lock(&di->lock);

	count = sprintf(buf, "%ld.%ld,%d\n",
		di->cache.timestamp.tv_sec,
		di->cache.timestamp.tv_nsec/100000000,
		di->cache.q_passed);

	mutex_unlock(&di->lock);

	return count;
}

static ssize_t show_hires_qpassed(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bq27x00_device_info *di = dev_get_drvdata(dev);
	int count;
	int rc;
	unsigned short i, f;

	mutex_lock(&di->lock);
	rc = bq27x00_battery_hires_qpassed(di, &i, &f);
	if (rc == -1) {
		count = sprintf(buf, "error\n");
	} else {
		count = sprintf(buf, "%ld.%ld,%04x,%04x\n",
				di->cache.timestamp.tv_sec,
				di->cache.timestamp.tv_nsec/100000000,
				i, f);
	}
	mutex_unlock(&di->lock);
	return count;
}

static ssize_t show_battery_details(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bq27x00_device_info *di = dev_get_drvdata(dev);
	int count;
	unsigned short DOD0;
	short q_max;
	short q_passed;
	int DODfinal;
	int true_fcc;
	int true_cap;
	struct timespec timestamp;

	/* Read a bunch of data from the cache */
	mutex_lock(&di->lock);
	timestamp = di->cache.timestamp;
	DOD0 = di->cache.DOD0;
	DODfinal = di->cache.DODfinal;
	q_max = di->cache.q_max;
	q_passed = di->cache.q_passed;
	true_fcc = di->cache.true_fcc;
	true_cap = di->cache.true_cap;
	mutex_unlock(&di->lock);

	count = sprintf(buf, "%ld.%ld,%u,%u,%d,%d,%d,%d\n",
			timestamp.tv_sec,
			timestamp.tv_nsec/100000000,
			DOD0, DODfinal, q_max, q_passed,
			true_fcc, true_cap);
	return count;
}

static int load_firmware(struct bq27x00_device_info *di,
		const struct bq27x00_firmware_data *firmware)
{
	struct i2c_client *client = di->client;
	struct i2c_msg msg;
	int i, j, firmware_size = 0;
	short *firmware_data;
	char command_char;
	char *sp_str1, *sp_str2, *token, *subtoken;
	unsigned char data[MAX_DATA_LEN-2];
	int err;

	if (firmware == NULL)
		return 0;

	firmware_data = (short *)vmalloc(sizeof(short)*firmware->size);
	if (firmware_data == NULL)
		return -ENOMEM;

	for (i = 0, sp_str1 = (char *)firmware->data; ; i += firmware_data[i+LEN]+2) {
		if (((unsigned int)sp_str1-(unsigned int)firmware->data) >= firmware->size) {
			break;
		}
		token = strsep(&sp_str1, "\n");
		if (token == NULL)
			break;

		if (token[0] == ';') {
			firmware_data[i+LEN] = -2;
			dev_dbg(&di->client->dev, "filter the comment line %d\n", i);
			continue;
		}
		for (j = 0, sp_str2 = token; ; j++) {
			subtoken = strsep(&sp_str2, " ");
			if (subtoken == NULL) {
				firmware_data[i+LEN] = j-1;
				break;
			}

			if (j == 0) {
				firmware_data[i+j] = *(int *)subtoken;
				command_char = (char)firmware_data[i+j];
				if (command_char != 'W' && command_char != 'R' &&
						command_char != 'C' && command_char != 'X') {
					break;
				}
			} else {
				if (command_char == 'X')
					firmware_data[i+j+1] = simple_strtol(subtoken, 0, 10);
				else
					firmware_data[i+j+1] = simple_strtol(subtoken, 0, 16);
				firmware_size = i+j+2;
			}
		}
	}

	pr_info("firmware_data=0x%x, firmware_size = %d\n", (unsigned int)firmware_data, firmware_size);

	for (i = 0; i < firmware_size; i += firmware_data[i+LEN]+2) {
		command_char = (char)firmware_data[i];
		if (command_char == 'W' || command_char == 'R' ||
				command_char == 'C' || command_char == 'X') {
			switch (command_char) {
			case 'W':
				#if DEBUG_UPDATE_COMMAND
				printk("W command:");
				for (j = 1; j < firmware_data[i+LEN]+2; j++)
					printk("%x ", firmware_data[i+j]);
				printk("\n");
				#endif
				if (firmware_data[i+LEN] <= 1)
					goto error;

				for (j = 0; j < firmware_data[i+LEN]-1; j++) {
					data[j] = firmware_data[i+j+3];
				}

				msg.addr = firmware_data[i+ADDR]>>1;
				msg.flags = 0;
				msg.len = firmware_data[i+LEN]-1;
				msg.buf = data;
				if (di->is_rom_mode && msg.addr != 0x0B) {
					printk("skip\n");
					break;
				}
				if (i2c_transfer(client->adapter, &msg, 1) < 0)
					goto error;
				break;
			case 'R':
			case 'C':
				#if DEBUG_UPDATE_COMMAND
				printk("C command:");
				for (j = 1; j < firmware_data[i+LEN]+2; j++)
					printk("%x ", firmware_data[i+j]);
				printk("\n");
				#endif
				msg.addr = firmware_data[i+ADDR]>>1;
				msg.flags = 0;
				msg.len = 1;
				msg.buf = data;
				if (di->is_rom_mode && msg.addr != 0x0B) {
					printk("skip\n");
					break;
				}

				data[0] = firmware_data[i+REG];

				err =  i2c_transfer(client->adapter, &msg, 1);
				if (err < 0)
					goto error;

				msg.len = firmware_data[i+LEN]-2;
				msg.flags = I2C_M_RD;

				i2c_transfer(client->adapter, &msg, 1);
				if (err < 0)
					goto error;

				if (firmware_data[i] == 'R')
					break;

				for (j = 0; j < firmware_data[i+LEN]-2; j++) {
					if (data[j] != firmware_data[i+4+j]) {
						printk("'C' command failed, exit upgrade firmware\n");
						printk("get %x, expect %x\n", data[j], firmware_data[i+4+j]);
						goto error;
					}
				}
				break;
			case 'X':
				#if DEBUG_UPDATE_COMMAND
				printk("X command: delay %d ms\n", firmware_data[i+2]);
				#endif
				mdelay(firmware_data[i+2]);
				if (firmware_data[i+2] == 0x4000)
					printk("last 16 S delay, i=%d\n", i);
				break;
			default:
				printk("the save_data value is invalid!!!\n");
			}
		} else {
			printk("i = %d, not hit the command, error, quit the update\n", i);
			goto error;
		}
	}

	/*Reset BQ27x00 and enable the Impedance Track algorithm*/
	bq27x00_control_cmd(di, RESET_SUBCMD);
	mdelay(CTL_CMD_DELAY);
	bq27x00_control_cmd(di, IT_ENABLE_SUBCMD);

	vfree(firmware_data);
	return 0;
error:
	vfree(firmware_data);
	return -EINVAL;
}

static ssize_t _update_firmware(struct device *dev, const char *firmware_filename)
{
	struct bq27x00_firmware_data firmware;
	struct bq27x00_device_info *di = dev_get_drvdata(dev);
	const struct firmware *fw;
	int error = 0;

	dev_info(dev, "update firmware start:%s\n", firmware_filename);

	error =  request_firmware(&fw, firmware_filename, dev);
	if (!error) {
		dev_info(dev, "request_firmware success, size=%d\n", fw->size);
	} else {
		dev_info(dev, "request_firmware failed\n");
		return error;
	}
	firmware.data = fw->data;
	firmware.size = fw->size;

	cancel_delayed_work(&di->work);

	mutex_lock(&battery_mutex);
	error = load_firmware(di, &firmware);
	if (!error) {
		dev_info(dev, "Update firmware success\n");
	} else {
		dev_info(dev, "Update firmware failed\n");
	}
	mutex_unlock(&battery_mutex);

	queue_delayed_work(system_power_efficient_wq,
						&di->work, 5 * HZ);

	release_firmware(fw);

	return error;
}

static int update_firmware(struct bq27x00_device_info *di)
{
	struct i2c_client *client = di->client;
	struct bq27x00_platform_data *pdata = di->pdata;
	unsigned long version;
	int bat_id;
	int type, error = 0;

	/* Check if it is rom mode */
	bq27x00_is_rom_mode(di);
	if (di->is_rom_mode) {
		dev_info(&client->dev, "device is in ROM mode, load fw\n");
	}

	/* Update firmware */
	bat_id = palmas_gpadc_read_physical(PALMAS_ADC_CH_IN0);
	if (bat_id < 0) {
		dev_err(&client->dev, "can't read batter id err = %d\n", bat_id);
		return -EINVAL;
	} else if (is_between(LG_ID_MIN, LG_ID_MAX, bat_id)) {
		dev_info(&client->dev, "lg battery IC %d\n", bat_id);
		type = BQ27X00_BATT_LGC;
	} else if (is_between(ATL_ID_MIN, ATL_ID_MAX, bat_id)) {
		dev_info(&client->dev, "atl battery IC %d\n", bat_id);
		type = BQ27X00_BATT_ATL;
	} else {
		dev_warn(&client->dev, "invalid battery id %d, using default\n", bat_id);
		type = BQ27X00_BATT_LGC;
	}

	version = simple_strtoul(pdata->fw_name[type], NULL, 16);
	dev_info(&di->client->dev, "id %d ver %lu, df_ver %x rom %d\n",
		type, version, di->df_ver, di->is_rom_mode);
	if (di->is_rom_mode)
		error = _update_firmware(&di->client->dev, pdata->fw_name[type]);
	else
		bq27x00_update(di);

	if (di->is_rom_mode && error == 0) {
		bq27x00_reset_registers(di);
		di->is_rom_mode = 0;
	}

	return error;
}

static DEVICE_ATTR(fw_version, S_IRUGO, show_firmware_version, NULL);
static DEVICE_ATTR(df_version, S_IRUGO, show_dataflash_version, NULL);
static DEVICE_ATTR(device_type, S_IRUGO, show_device_type, NULL);
static DEVICE_ATTR(reset, S_IRUGO, show_reset, NULL);
static DEVICE_ATTR(qpassed, S_IRUGO, show_qpassed, NULL);
static DEVICE_ATTR(qpassed_hires, S_IRUGO, show_hires_qpassed, NULL);
static DEVICE_ATTR(battery_details, S_IRUGO, show_battery_details, NULL);

static struct attribute *bq27x00_attributes[] = {
	&dev_attr_fw_version.attr,
	&dev_attr_df_version.attr,
	&dev_attr_device_type.attr,
	&dev_attr_reset.attr,
	&dev_attr_qpassed.attr,
	&dev_attr_qpassed_hires.attr,
	&dev_attr_battery_details.attr,
	NULL
};

static const struct attribute_group bq27x00_attr_group = {
	.attrs = bq27x00_attributes,
};

static void bq27x00_reset_registers(struct bq27x00_device_info *di)
{
	/* Get the fw version to determine the register mapping */
	di->fw_ver = bq27x00_battery_read_fw_version(di);
	di->df_ver = bq27x00_battery_read_dataflash_version(di);
	dev_info(&di->client->dev,
		"Gas Gauge fw version 0x%04x; df version 0x%04x\n",
		di->fw_ver, di->df_ver);

	if (di->fw_ver == L1_600_FW_VERSION || di->fw_ver == L1_604_FW_VERSION)
		di->regs = bq27x00_fw_l1_regs;
	else if (di->fw_ver == G3_FW_VERSION)
		di->regs = bq27x00_fw_g3_regs;
	else if (di->fw_ver == G4_FW_VERSION)
		di->regs = bq27x00_fw_g4_regs;
	else {
		dev_err(&di->client->dev,
			"Unkown Gas Gauge fw version: 0x%04x\n", di->fw_ver);
		di->regs = bq27x00_fw_g4_regs;
	}
}

static void of_bq27x00_parse_platform_data(struct i2c_client *client,
				struct bq27x00_platform_data *pdata)
{
	char const *pstr;
	struct device_node *np = client->dev.of_node;

	if (!of_property_read_string(np, "ti,fw-lgc-name", &pstr))
		pdata->fw_name[0] = pstr;
	else
		dev_warn(&client->dev, "Failed to read fw-lgc-name\n");

	if (!of_property_read_string(np, "ti,fw-atl-name", &pstr))
		pdata->fw_name[1] = pstr;
	else
		dev_warn(&client->dev, "Failed to read fw-atl-name\n");

	if (!of_property_read_string(np, "ti,tz-name", &pstr))
		pdata->tz_name = pstr;
	else
		dev_warn(&client->dev, "Failed to read tz-name\n");
}

static int bq27x00_battery_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
	char *name;
	struct bq27x00_device_info *di;
	int num;
	int retval = 0;

	pr_info("%s: start\n", __func__);
	/* Get new ID for the new battery device */
	retval = idr_pre_get(&battery_id, GFP_KERNEL);
	if (retval == 0)
		return -ENOMEM;
	mutex_lock(&battery_mutex);
	retval = idr_get_new(&battery_id, client, &num);
	mutex_unlock(&battery_mutex);
	if (retval < 0)
		return retval;

	name = kasprintf(GFP_KERNEL, "%s-%d", id->name, num);
	if (!name) {
		dev_err(&client->dev, "failed to allocate device name\n");
		retval = -ENOMEM;
		goto batt_failed_1;
	}

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di) {
		dev_err(&client->dev, "failed to allocate device info data\n");
		retval = -ENOMEM;
		goto batt_failed_2;
	}

	if (client->dev.of_node) {
		di->pdata = devm_kzalloc(&client->dev,
					sizeof(struct bq27x00_platform_data), GFP_KERNEL);
		if (!di->pdata)
			return -ENOMEM;
		of_bq27x00_parse_platform_data(client, di->pdata);
	} else {
		di->pdata = client->dev.platform_data;
	}

	di->client = client;
	di->id = num;
	di->chip = id->driver_data;
	di->bat.name = "battery";
	di->bus.read = &bq27x00_read_i2c;
	di->bus.write = &bq27x00_write_i2c;
	di->bat_status = POWER_SUPPLY_STATUS_DISCHARGING;

	if (di->pdata && di->pdata->translate_temp)
		di->translate_temp = di->pdata->translate_temp;
	else
		dev_warn(&client->dev, "fixup func not set, using default thermistor behavior\n");

	bq27x00_reset_registers(di);

	/* use switch dev reporting to tell userspace to poweroff gracefully */
	di->sdev.name = "poweroff";
	retval = switch_dev_register(&di->sdev);
	if (retval) {
		dev_err(&client->dev, "error registering switch device: %d\n", retval);
		goto batt_failed_3;
	}

	if (bq27x00_powersupply_init(di))
		goto batt_failed_3;

	i2c_set_clientdata(client, di);

	if (di->client->irq >= 0) {
		retval = devm_request_threaded_irq(&client->dev, di->client->irq, 
				NULL, soc_int_irq_threaded_handler, 
				IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
				dev_name(&client->dev), di);

		if (retval) {
			dev_err(&client->dev, "failed to request threaded irq for soc_int: %d\n", retval);
			goto batt_failed_3;
		}
	}

	bq27x00_bgi.tz_name = di->pdata->tz_name;

	di->bg_dev = battery_gauge_register(&client->dev, &bq27x00_bgi, di);
	if (IS_ERR(di->bg_dev)) {
		retval = PTR_ERR(di->bg_dev);
		dev_err(&client->dev, "battery gauge register failed: %d\n", retval);
		goto batt_failed_3;
	}

	device_set_wakeup_capable(&client->dev, 1);
	device_wakeup_enable(&client->dev);

	retval = sysfs_create_group(&client->dev.kobj, &bq27x00_attr_group);
	if (retval)
		dev_err(&client->dev, "could not create sysfs files\n");

	/* Update firmware */
	update_firmware(di);

	return 0;

batt_failed_3:
	kfree(di);
batt_failed_2:
	kfree(name);
batt_failed_1:
	mutex_lock(&battery_mutex);
	idr_remove(&battery_id, num);
	mutex_unlock(&battery_mutex);

	return retval;
}

static int bq27x00_battery_remove(struct i2c_client *client)
{
	struct bq27x00_device_info *di = i2c_get_clientdata(client);

	bq27x00_powersupply_unregister(di);

	kfree(di->bat.name);

	mutex_lock(&battery_mutex);
	idr_remove(&battery_id, di->id);
	mutex_unlock(&battery_mutex);

	switch_dev_unregister(&di->sdev);

	kfree(di);

	return 0;
}

static int bq27x00_battery_dump_qpassed(struct bq27x00_device_info *di, char *buf, size_t size)
{
	union power_supply_propval val;
	unsigned short i, f;
	int cnt;

	if (!di || !buf || size == 0)
		return -EPERM;

	bq27x00_battery_qpassed(di, &val);

	cnt = scnprintf(buf, size, "%dmAh", val.intval);

	/* Read hi resolution version if available */
	if (di->fw_ver >= L1_604_FW_VERSION) {
		bq27x00_battery_hires_qpassed(di, &i, &f);
		scnprintf(buf+cnt, size-cnt, " (0x%04x,0x%04x)", i, f);
	}
	return 0;
}

static void bq27x00_shutdown(struct i2c_client *client)
{
	struct bq27x00_device_info *di = i2c_get_clientdata(client);

	if (di->client->irq)
		disable_irq(di->client->irq);

	cancel_delayed_work(&di->work);
	dev_err(&di->client->dev, "At shutdown Voltage %dmV\n",
			di->cache.voltage);
}

static int bq27x00_suspend(struct device *dev)
{
	char buf[100];
	struct bq27x00_device_info *di = dev_get_drvdata(dev);

	mutex_lock(&di->lock);

	bq27x00_battery_dump_qpassed(di, buf, sizeof(buf));
	cancel_delayed_work(&di->work);

	mutex_unlock(&di->lock);

	if (device_may_wakeup(&di->client->dev) && di->client->irq)
		enable_irq_wake(di->client->irq);

	dev_err(&di->client->dev, "Qpassed suspend: %s. Current voltage: %dmV\n", 
				buf, di->cache.voltage);

	return 0;
}

static int bq27x00_resume(struct device *dev)
{
	char buf[100];
	struct bq27x00_device_info *di = dev_get_drvdata(dev);

	if (device_may_wakeup(&di->client->dev) && di->client->irq)
		disable_irq_wake(di->client->irq);

	mutex_lock(&di->lock);

	bq27x00_battery_dump_qpassed(di, buf, sizeof(buf));
	queue_delayed_work(system_power_efficient_wq, &di->work, 0);

	mutex_unlock(&di->lock);

	dev_err(&di->client->dev, "Qpassed resume: %s. Current voltage: %dmV\n", 
				buf, di->cache.voltage);

	return 0;
}

static SIMPLE_DEV_PM_OPS(bq27x00_pm_ops, bq27x00_suspend, bq27x00_resume);

static const struct of_device_id bq27520_dt_match[] = {
	{ .compatible = "ti,bq27520" },
	{ },
};

static const struct i2c_device_id bq27x00_id[] = {
	{ "bq27200", BQ27000 },	/* bq27200 is same as bq27000, but with i2c */
	{ "bq27500", BQ27500 },
	{ "bq27520", BQ27500 },
	{},
};
MODULE_DEVICE_TABLE(i2c, bq27x00_id);

static struct i2c_driver bq27x00_battery_driver = {
	.driver = {
		.name = "bq27x00-battery",
		.of_match_table = of_match_ptr(bq27520_dt_match),
		.pm = &bq27x00_pm_ops,
	},
	.probe = bq27x00_battery_probe,
	.remove = bq27x00_battery_remove,
	.id_table = bq27x00_id,
	.shutdown	= bq27x00_shutdown,
};

static inline int bq27x00_battery_i2c_init(void)
{
	int ret = i2c_add_driver(&bq27x00_battery_driver);
	if (ret)
		printk(KERN_ERR "Unable to register BQ27x00 i2c driver\n");

	return ret;
}

static inline void bq27x00_battery_i2c_exit(void)
{
	i2c_del_driver(&bq27x00_battery_driver);
}

/*
 * Module stuff
 */

static int __init bq27x00_battery_init(void)
{
	int ret;

	ret = bq27x00_battery_i2c_init();
	if (ret)
		return ret;

	return ret;
}
module_init(bq27x00_battery_init);

static void __exit bq27x00_battery_exit(void)
{
	bq27x00_battery_i2c_exit();
}
module_exit(bq27x00_battery_exit);

MODULE_AUTHOR("Rodolfo Giometti <giometti@linux.it>");
MODULE_DESCRIPTION("BQ27x00 battery monitor driver");
MODULE_LICENSE("GPL");
