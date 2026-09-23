/*
 * Copyright (C) 2026 Artem Bambalov
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * Which touch controller is actually on the board.
 *
 * The same tablet ships with one of several Atmel maXTouch parts or one of
 * a couple of Synaptics RMI4 parts, all on the same bus, the same supply,
 * the same reset and the same interrupt line. Both drivers are built in and
 * both get a device to probe, so on every board one of them spends its
 * probe powering the controller up and retrying reads from a chip that is
 * not there -- the Synaptics one alone burns twenty I2C retries and the
 * better part of a second on an Atmel board, and powers the shared supply
 * down again on its way out.
 *
 * The bus answers the question outright: each part responds at its own
 * address and nowhere else. Each driver asks first thing in its probe,
 * before it has claimed the supply or the reset line, and steps aside at
 * once when the answer names the other vendor -- so the one that loses
 * never touches what the other one needs. When nothing answers at all the
 * answer is unknown, and the driver carries on exactly as it did before
 * this existed.
 *
 * Asking is made quick: the supply is up from boot, so the controller
 * usually answers the first read and nothing waits. Only when it does not
 * is it powered and taken out of reset here, and then polled until it
 * answers rather than slept on for the longest boot time any part needs.
 * The whole of it runs under one lock, so two probes cannot interleave
 * their supply and reset handling with this one, and the result is kept
 * once found: there is one controller and it does not change while the
 * kernel runs.
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/input/touch_vendor.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>

#define TOUCH_VENDOR_SUPPLY		"vdd-touch"
#define TOUCH_VENDOR_RESET_ACTIVE_MS	10
#define TOUCH_VENDOR_POLL_MS		5
#define TOUCH_VENDOR_BOOT_MAX_MS	200

struct touch_vendor_address {
	unsigned short addr;
	enum touch_vendor vendor;
	const char *name;
};

/*
 * Atmel first: most boards carry one, and a probe that finds it at the
 * first address costs no failed transfer at all -- every address that does
 * not answer costs a "no acknowledge" line from the controller driver. The
 * bootloader addresses are the ones atmel_mxt_ts pairs with the
 * application addresses under BOOTLOADER_1664_1188: a part without
 * firmware answers there, and it is still an Atmel board that the Atmel
 * driver has to flash.
 */
static const struct touch_vendor_address touch_vendor_addresses[] = {
	{ 0x4a, TOUCH_VENDOR_ATMEL,	"Atmel maXTouch" },
	{ 0x4b, TOUCH_VENDOR_ATMEL,	"Atmel maXTouch" },
	{ 0x26, TOUCH_VENDOR_ATMEL,	"Atmel maXTouch bootloader" },
	{ 0x27, TOUCH_VENDOR_ATMEL,	"Atmel maXTouch bootloader" },
	{ 0x20, TOUCH_VENDOR_SYNAPTICS,	"Synaptics RMI4" },
};

static DEFINE_MUTEX(touch_vendor_lock);
static enum touch_vendor touch_vendor_found = TOUCH_VENDOR_UNKNOWN;

static bool touch_vendor_answers(struct i2c_adapter *adap, unsigned short addr)
{
	u8 byte;
	struct i2c_msg msg = {
		.addr	= addr,
		.flags	= I2C_M_RD,
		.len	= 1,
		.buf	= &byte,
	};

	return i2c_transfer(adap, &msg, 1) == 1;
}

/* One read per address, no retries; the first part to answer is the one. */
static enum touch_vendor touch_vendor_scan(struct i2c_adapter *adap)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(touch_vendor_addresses); i++) {
		const struct touch_vendor_address *entry =
				&touch_vendor_addresses[i];

		if (touch_vendor_answers(adap, entry->addr)) {
			dev_info(&adap->dev, "touch: %s answers at 0x%02x\n",
					entry->name, entry->addr);
			return entry->vendor;
		}
	}

	return TOUCH_VENDOR_UNKNOWN;
}

/*
 * The controller did not answer as it was left, so it is off or held in
 * reset. Power it, pulse reset, and poll until it answers or the longest
 * boot time is up. Everything taken here is given back before returning:
 * the driver that wins sets the supply and the reset line up its own way.
 */
static enum touch_vendor touch_vendor_power_and_scan(struct i2c_client *client,
		int reset_gpio)
{
	enum touch_vendor vendor = TOUCH_VENDOR_UNKNOWN;
	struct regulator *supply;
	bool reset_held = false;
	unsigned long deadline;

	supply = regulator_get(&client->dev, TOUCH_VENDOR_SUPPLY);
	if (IS_ERR(supply))
		supply = NULL;
	else if (regulator_enable(supply)) {
		regulator_put(supply);
		supply = NULL;
	}

	if (gpio_is_valid(reset_gpio) &&
			!gpio_request(reset_gpio, "touch_vendor_reset")) {
		reset_held = true;
		gpio_direction_output(reset_gpio, 0);
		msleep(TOUCH_VENDOR_RESET_ACTIVE_MS);
		gpio_set_value_cansleep(reset_gpio, 1);
	}

	deadline = jiffies + msecs_to_jiffies(TOUCH_VENDOR_BOOT_MAX_MS);
	do {
		msleep(TOUCH_VENDOR_POLL_MS);
		vendor = touch_vendor_scan(client->adapter);
	} while (vendor == TOUCH_VENDOR_UNKNOWN && time_before(jiffies, deadline));

	if (reset_held)
		gpio_free(reset_gpio);
	if (supply) {
		regulator_disable(supply);
		regulator_put(supply);
	}

	return vendor;
}

enum touch_vendor touch_vendor_detect(struct i2c_client *client, int reset_gpio)
{
	enum touch_vendor vendor;

	mutex_lock(&touch_vendor_lock);
	if (touch_vendor_found == TOUCH_VENDOR_UNKNOWN)
		touch_vendor_found = touch_vendor_scan(client->adapter);
	if (touch_vendor_found == TOUCH_VENDOR_UNKNOWN)
		touch_vendor_found =
			touch_vendor_power_and_scan(client, reset_gpio);
	vendor = touch_vendor_found;
	mutex_unlock(&touch_vendor_lock);

	return vendor;
}
EXPORT_SYMBOL_GPL(touch_vendor_detect);

bool touch_vendor_is_other(struct i2c_client *client, int reset_gpio,
		enum touch_vendor mine)
{
	enum touch_vendor vendor = touch_vendor_detect(client, reset_gpio);

	return vendor != TOUCH_VENDOR_UNKNOWN && vendor != mine;
}
EXPORT_SYMBOL_GPL(touch_vendor_is_other);

MODULE_AUTHOR("Artem Bambalov");
MODULE_DESCRIPTION("Touch controller vendor detection by I2C address");
MODULE_LICENSE("GPL v2");
