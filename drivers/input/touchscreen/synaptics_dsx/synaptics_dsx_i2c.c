/*
 * Synaptics DSX touchscreen driver
 *
 * Copyright (C) 2012 Synaptics Incorporated
 *
 * Copyright (C) 2012 Alexandra Chin <alexandra.chin@tw.synaptics.com>
 * Copyright (C) 2012 Scott Lin <scott.lin@tw.synaptics.com>
 * Copyright (C) 2016 XiaoMi, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/input/synaptics_dsx.h>
#include "synaptics_dsx_core.h"
#include <linux/of_gpio.h>

#define SYN_I2C_RETRY_TIMES 10

static int synaptics_rmi4_i2c_set_page(struct synaptics_rmi4_data *rmi4_data,
		unsigned short addr)
{
	int retval;
	unsigned char retry;
	unsigned char buf[PAGE_SELECT_LEN];
	unsigned char page;
	struct i2c_client *i2c = to_i2c_client(rmi4_data->pdev->dev.parent);

	page = ((addr >> 8) & MASK_8BIT);
	if (page != rmi4_data->current_page) {
		buf[0] = MASK_8BIT;
		buf[1] = page;
		for (retry = 0; retry < SYN_I2C_RETRY_TIMES; retry++) {
			retval = i2c_master_send(i2c, buf, PAGE_SELECT_LEN);
			if (retval != PAGE_SELECT_LEN) {
				dev_err(rmi4_data->pdev->dev.parent,
						"%s: I2C retry %d\n",
						__func__, retry + 1);
				msleep(20);
			} else {
				rmi4_data->current_page = page;
				break;
			}
		}
	} else {
		retval = PAGE_SELECT_LEN;
	}

	return retval;
}

static int synaptics_rmi4_i2c_read(struct synaptics_rmi4_data *rmi4_data,
		unsigned short addr, unsigned char *data, unsigned short length)
{
	int retval;
	unsigned char retry;
	unsigned char buf;
	struct i2c_client *i2c = to_i2c_client(rmi4_data->pdev->dev.parent);
	struct i2c_msg msg[] = {
		{
			.addr = i2c->addr,
			.flags = 0,
			.len = 1,
			.buf = &buf,
		},
		{
			.addr = i2c->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = data,
		},
	};

	buf = addr & MASK_8BIT;

	mutex_lock(&rmi4_data->rmi4_io_ctrl_mutex);

	retval = synaptics_rmi4_i2c_set_page(rmi4_data, addr);
	if (retval != PAGE_SELECT_LEN) {
		retval = -EIO;
		goto exit;
	}

	for (retry = 0; retry < SYN_I2C_RETRY_TIMES; retry++) {
		if (i2c_transfer(i2c->adapter, msg, 2) == 2) {
			retval = length;
			break;
		}
		dev_err(rmi4_data->pdev->dev.parent,
				"%s: I2C retry %d\n",
				__func__, retry + 1);
		msleep(20);
	}

	if (retry == SYN_I2C_RETRY_TIMES) {
		dev_err(rmi4_data->pdev->dev.parent,
				"%s: I2C read over retry limit\n",
				__func__);
		retval = -EIO;
	}

exit:
	mutex_unlock(&rmi4_data->rmi4_io_ctrl_mutex);

	return retval;
}

static int synaptics_rmi4_i2c_write(struct synaptics_rmi4_data *rmi4_data,
		unsigned short addr, unsigned char *data, unsigned short length)
{
	int retval;
	unsigned char retry;
	unsigned char buf[length + 1];
	struct i2c_client *i2c = to_i2c_client(rmi4_data->pdev->dev.parent);
	struct i2c_msg msg[] = {
		{
			.addr = i2c->addr,
			.flags = 0,
			.len = length + 1,
			.buf = buf,
		}
	};

	mutex_lock(&rmi4_data->rmi4_io_ctrl_mutex);

	retval = synaptics_rmi4_i2c_set_page(rmi4_data, addr);
	if (retval != PAGE_SELECT_LEN) {
		retval = -EIO;
		goto exit;
	}

	buf[0] = addr & MASK_8BIT;
	memcpy(&buf[1], &data[0], length);

	for (retry = 0; retry < SYN_I2C_RETRY_TIMES; retry++) {
		if (i2c_transfer(i2c->adapter, msg, 1) == 1) {
			retval = length;
			break;
		}
		dev_err(rmi4_data->pdev->dev.parent,
				"%s: I2C retry %d\n",
				__func__, retry + 1);
		msleep(20);
	}

	if (retry == SYN_I2C_RETRY_TIMES) {
		dev_err(rmi4_data->pdev->dev.parent,
				"%s: I2C write over retry limit\n",
				__func__);
		retval = -EIO;
	}

exit:
	mutex_unlock(&rmi4_data->rmi4_io_ctrl_mutex);

	return retval;
}

static struct synaptics_dsx_bus_access bus_access = {
	.type = BUS_I2C,
	.read = synaptics_rmi4_i2c_read,
	.write = synaptics_rmi4_i2c_write,
};

static struct synaptics_dsx_hw_interface hw_if;

static struct platform_device *synaptics_dsx_i2c_device;

static void synaptics_rmi4_i2c_dev_release(struct device *dev)
{
	kfree(synaptics_dsx_i2c_device);

	return;
}

static void synaptics_rmi4_para_dump(struct device *dev,
				struct synaptics_dsx_board_data *bdata)
{
	int i;

	dev_info(dev, "power_gpio = %d\n", bdata->power_gpio);
	dev_info(dev, "reset_gpio = %d\n", bdata->reset_gpio);
	dev_info(dev, "irq_gpio = %d\n", bdata->irq_gpio);
	dev_info(dev, "x_flip = %d\n", (int)bdata->x_flip);
	dev_info(dev, "y_flip = %d\n", (int)bdata->y_flip);
	dev_info(dev, "swap_axes = %d\n", (int)bdata->swap_axes);
	dev_info(dev, "power_on_state = %d\n", (int)bdata->power_on_state);
	dev_info(dev, "reset_on_state = %d\n", (int)bdata->reset_on_state);
	dev_info(dev, "panel_x = %d\n", (int)bdata->panel_x);
	dev_info(dev, "panel_y = %d\n", (int)bdata->panel_y);
	dev_info(dev, "power_delay_ms = %d\n", (int)bdata->power_delay_ms);
	dev_info(dev, "reset_delay_ms = %d\n", (int)bdata->reset_delay_ms);
	dev_info(dev, "reset_active_ms = %d\n", (int)bdata->reset_active_ms);
	dev_info(dev, "fw_name = %s\n", bdata->fw_name);
	dev_info(dev, "self_test_name = %s\n", bdata->self_test_name);
	dev_info(dev, "regulator_name = %s\n", bdata->regulator_name);

	for (i = 0; i < bdata->cap_button_map->nbuttons; i++)
		dev_info(dev, "key[%d] = %d\n", i, bdata->cap_button_map->map[i]);
}

static int parse_dt(struct device *dev, struct synaptics_dsx_board_data *bdata)
{
	struct device_node *np = dev->of_node;
	struct property *prop;

	of_property_read_u32(np, "synaptics,x-flip",(u32 *) &bdata->x_flip);
	of_property_read_u32(np, "synaptics,y-flip",(u32 *) &bdata->y_flip);
	of_property_read_u32(np, "synaptics,swap-axes",(u32 *) &bdata->swap_axes);
	of_property_read_u32(np, "synaptics,irq-on-state",(u32 *) &bdata->irq_on_state);
	of_property_read_u32(np, "synaptics,power-on-state",(u32 *) &bdata->power_on_state);
	of_property_read_u32(np, "synaptics,reset-on-state",(u32 *) &bdata->reset_on_state);
	of_property_read_u32(np, "synaptics,irq-flags",(u32 *) &bdata->irq_flags);
	of_property_read_u32(np, "synaptics,panel-x",(u32 *) &bdata->panel_x);
	of_property_read_u32(np, "synaptics,panel-y",(u32 *) &bdata->panel_y);
	of_property_read_u32(np, "synaptics,power-delay-ms",(u32 *) &bdata->power_delay_ms);
	of_property_read_u32(np, "synaptics,reset-delay-ms",(u32 *) &bdata->reset_delay_ms);
	of_property_read_u32(np, "synaptics,reset-active-ms",(u32 *) &bdata->reset_active_ms);
	of_property_read_u32(np, "synaptics,byte-delay-us",(u32 *) &bdata->byte_delay_us);
	of_property_read_u32(np, "synaptics,block-delay-us",(u32 *) &bdata->block_delay_us);
	of_property_read_string(np, "synaptics,regulator-name", (const char **) &bdata->regulator_name);
	of_property_read_string(np, "synaptics,fw-name", &bdata->fw_name);
	of_property_read_string(np, "synaptics,self-test-name", &bdata->self_test_name);

	prop = of_find_property(np, "synaptics,cap-button-map", NULL);
	if (prop) {
		bdata->cap_button_map->map = devm_kzalloc(dev,
				prop->length,
				GFP_KERNEL);
		if (!bdata->cap_button_map->map)
			return -ENOMEM;
		bdata->cap_button_map->nbuttons = prop->length / sizeof(u32);
		of_property_read_u32_array(np,
				"synaptics,cap-button-map",
				bdata->cap_button_map->map,
				bdata->cap_button_map->nbuttons);
	}

	bdata->power_gpio = of_get_named_gpio(np, "synaptics,power-gpio", 0);
	if (bdata->power_gpio < 0)
		bdata->power_gpio = -1;

	bdata->dcdc_gpio = of_get_named_gpio(np, "synaptics,dcdc-gpio", 0);
	if (bdata->dcdc_gpio < 0)
		bdata->dcdc_gpio = -1;

	bdata->reset_gpio = of_get_named_gpio(np, "synaptics,reset-gpio", 0);
	if (bdata->reset_gpio < 0)
		bdata->reset_gpio = -1;

	bdata->irq_gpio = of_get_named_gpio(np, "synaptics,irq-gpio", 0);
	if (bdata->irq_gpio < 0)
		bdata->irq_gpio = -1;

	return 0;
}

static int synaptics_rmi4_i2c_probe(struct i2c_client *client,
		const struct i2c_device_id *dev_id)
{
	int retval;

	if (!i2c_check_functionality(client->adapter,
			I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&client->dev,
				"%s: SMBus byte data commands not supported by host\n",
				__func__);
		return -EIO;
	}

	synaptics_dsx_i2c_device = kzalloc(
			sizeof(struct platform_device),
			GFP_KERNEL);
	if (!synaptics_dsx_i2c_device) {
		dev_err(&client->dev,
				"%s: Failed to allocate memory for synaptics_dsx_i2c_device\n",
				__func__);
		return -ENOMEM;
	}

	if (client->dev.of_node) {
		hw_if.board_data = devm_kzalloc(&client->dev,
				sizeof(struct synaptics_dsx_board_data),
				GFP_KERNEL);
		if (!hw_if.board_data) {
			dev_err(&client->dev,
					"%s: Failed to allocate memory for board data\n",
					__func__);
			return -ENOMEM;
		}
		hw_if.board_data->cap_button_map = devm_kzalloc(&client->dev,
				sizeof(struct synaptics_dsx_cap_button_map),
				GFP_KERNEL);
		if (!hw_if.board_data->cap_button_map) {
			dev_err(&client->dev,
					"%s: Failed to allocate memory for 0D button map\n",
					__func__);
			return -ENOMEM;
		}

		parse_dt(&client->dev, hw_if.board_data);
	} else
		hw_if.board_data = client->dev.platform_data;

	hw_if.bus_access = &bus_access;

	synaptics_rmi4_para_dump(&client->dev, hw_if.board_data);

	synaptics_dsx_i2c_device->name = PLATFORM_DRIVER_NAME;
	synaptics_dsx_i2c_device->id = 0;
	synaptics_dsx_i2c_device->num_resources = 0;
	synaptics_dsx_i2c_device->dev.parent = &client->dev;
	synaptics_dsx_i2c_device->dev.platform_data = &hw_if;
	synaptics_dsx_i2c_device->dev.release = synaptics_rmi4_i2c_dev_release;

	retval = platform_device_register(synaptics_dsx_i2c_device);
	if (retval) {
		dev_err(&client->dev,
				"%s: Failed to register platform device\n",
				__func__);
		return -ENODEV;
	}

	return 0;
}

static int synaptics_rmi4_i2c_remove(struct i2c_client *client)
{
	platform_device_unregister(synaptics_dsx_i2c_device);

	return 0;
}

static const struct i2c_device_id synaptics_rmi4_id_table[] = {
	{I2C_DRIVER_NAME, 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, synaptics_rmi4_id_table);

static struct of_device_id synaptics_rmi4_of_match_table[] = {
	{ .compatible = "synaptics,dsx-i2c",},
	{ },
};

static struct i2c_driver synaptics_rmi4_i2c_driver = {
	.driver = {
		.name = I2C_DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = synaptics_rmi4_of_match_table,
	},
	.probe = synaptics_rmi4_i2c_probe,
	.remove = synaptics_rmi4_i2c_remove,
	.id_table = synaptics_rmi4_id_table,
};

int synaptics_rmi4_bus_init()
{
	return i2c_add_driver(&synaptics_rmi4_i2c_driver);
}
EXPORT_SYMBOL(synaptics_rmi4_bus_init);

void synaptics_rmi4_bus_exit()
{
	i2c_del_driver(&synaptics_rmi4_i2c_driver);

	return;
}
EXPORT_SYMBOL(synaptics_rmi4_bus_exit);

MODULE_AUTHOR("Synaptics, Inc.");
MODULE_DESCRIPTION("Synaptics DSX I2C Bus Support Module");
MODULE_LICENSE("GPL v2");
