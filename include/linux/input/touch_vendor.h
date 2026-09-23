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

#ifndef _LINUX_INPUT_TOUCH_VENDOR_H
#define _LINUX_INPUT_TOUCH_VENDOR_H

#include <linux/i2c.h>

enum touch_vendor {
	TOUCH_VENDOR_UNKNOWN,
	TOUCH_VENDOR_ATMEL,
	TOUCH_VENDOR_SYNAPTICS,
};

enum touch_vendor touch_vendor_detect(struct i2c_client *client,
		int reset_gpio);
bool touch_vendor_is_other(struct i2c_client *client, int reset_gpio,
		enum touch_vendor mine);

#endif /* _LINUX_INPUT_TOUCH_VENDOR_H */
