//SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/sysfs.h>
#include <linux/slab.h>

struct usb7002_device {
	struct i2c_client *client;
	struct device *dev;
};
int usb7002_value;
struct usb7002_device *u7002 = NULL;

#define I2C_RETRY 5
static int usb7002_i2c_reg_write(u8 *data, u16 len)
{

	struct i2c_client *client;
	struct i2c_msg msg;
	int ret = 0, i = 0;

	if (!u7002)
		return 0;

	client = to_i2c_client(u7002->dev);

	if (!client && !client->adapter)
		return -ENODEV;

	msg.buf = data;
	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = len+1;

	for (i = 0; i < I2C_RETRY; i++) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret == 1) {
			pr_info("%s i2c_send success ret : %d\n", __func__, ret);
			return 0;
		}
		usleep_range(10*1000, 20*1000);
		pr_err("%s i2c_send failed retry : %d\n", __func__, i);
	}

	return ret;
}

/**
* usb7002_switch_peripheral - Switches the USB7002 to peripheral mode.
*
* This function switches the USB7002 hub to peripheral mode.
*
* Return: 0 on success, negative error code on failure.
*/

int usb7002_switch_peripheral(void)
{

	u8 data[11] = {0};
	int ret = 0;

	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x07;
	data[3] = 0x00;
	data[4] = 0x01;
	data[5] = 0xBF;
	data[6] = 0x80;
	data[7] = 0x08;
	data[8] = 0x08;
	data[9] = 0x01;
	ret = usb7002_i2c_reg_write(data, 10);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x99;
	data[1] = 0x37;
	data[2] = 0x00;
	ret = usb7002_i2c_reg_write(data, 3);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x07;
	data[3] = 0x00;
	data[4] = 0x01;
	data[5] = 0xBF;
	data[6] = 0x80;
	data[7] = 0x08;
	data[8] = 0x28;
	data[9] = 0x01;
	ret = usb7002_i2c_reg_write(data, 10);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x99;
	data[1] = 0x37;
	data[2] = 0x00;
	ret = usb7002_i2c_reg_write(data, 3);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x07;
	data[3] = 0x00;
	data[4] = 0x01;
	data[5] = 0xBF;
	data[6] = 0x80;
	data[7] = 0x50;
	data[8] = 0x00;
	data[9] = 0x05;
	ret = usb7002_i2c_reg_write(data, 10);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x99;
	data[1] = 0x37;
	data[2] = 0x00;
	ret = usb7002_i2c_reg_write(data, 3);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x07;
	data[3] = 0x00;
	data[4] = 0x01;
	data[5] = 0xBF;
	data[6] = 0x80;
	data[7] = 0x54;
	data[8] = 0x00;
	data[9] = 0x01;
	ret = usb7002_i2c_reg_write(data, 10);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x99;
	data[1] = 0x37;
	data[2] = 0x00;
	ret = usb7002_i2c_reg_write(data, 3);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0xAA;
	data[1] = 0x56;
	data[2] = 0x00;
	ret = usb7002_i2c_reg_write(data, 3);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x07;
	data[3] = 0x00;
	data[4] = 0x01;
	data[5] = 0xBF;
	data[6] = 0x80;
	data[7] = 0x3C;
	data[8] = 0x40;
	data[9] = 0x00;
	ret = usb7002_i2c_reg_write(data, 10);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x99;
	data[1] = 0x37;
	data[2] = 0x00;
	ret = usb7002_i2c_reg_write(data, 3);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x08;
	data[3] = 0x00;
	data[4] = 0x02;
	data[5] = 0xBF;
	data[6] = 0x80;
	data[7] = 0x09;
	data[8] = 0x03;
	data[9] = 0x01;
	data[10] = 0x01;
	ret = usb7002_i2c_reg_write(data, 11);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x99;
	data[1] = 0x37;
	data[2] = 0x00;
	ret = usb7002_i2c_reg_write(data, 3);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x08;
	data[3] = 0x00;
	data[4] = 0x02;
	data[5] = 0xBF;
	data[6] = 0x80;
	data[7] = 0x09;
	data[8] = 0x23;
	data[9] = 0x01;
	data[10] = 0x01;
	ret = usb7002_i2c_reg_write(data, 11);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x99;
	data[1] = 0x37;
	data[2] = 0x00;
	ret = usb7002_i2c_reg_write(data, 3);
	if (ret < 0)
		goto i2c_fail;

i2c_fail:
	return ret;
}
EXPORT_SYMBOL(usb7002_switch_peripheral);

/**
* usb7002_switch_host - Switches the USB7002 to host mode.
*
* This function switches the USB7002 hub to host mode.
*
* Return: 0 on success, negative error code on failure.
*/

int usb7002_switch_host(void)
{

	u8 data[11] = {0};
	int ret = 0;

	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x07;
	data[3] = 0x00;
	data[4] = 0x01;
	data[5] = 0xBF;
	data[6] = 0x80;
	data[7] = 0x08;
	data[8] = 0x08;
	data[9] = 0x00;
	ret = usb7002_i2c_reg_write(data, 10);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x99;
	data[1] = 0x37;
	data[2] = 0x00;
	ret = usb7002_i2c_reg_write(data, 3);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x07;
	data[3] = 0x00;
	data[4] = 0x01;
	data[5] = 0xBF;
	data[6] = 0x80;
	data[7] = 0x08;
	data[8] = 0x28;
	data[9] = 0x00;
	ret = usb7002_i2c_reg_write(data, 10);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x99;
	data[1] = 0x37;
	data[2] = 0x00;
	ret = usb7002_i2c_reg_write(data, 3);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x07;
	data[3] = 0x00;
	data[4] = 0x01;
	data[5] = 0xBF;
	data[6] = 0x80;
	data[7] = 0x50;
	data[8] = 0x00;
	data[9] = 0x01;
	ret = usb7002_i2c_reg_write(data, 10);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x99;
	data[1] = 0x37;
	data[2] = 0x00;
	ret = usb7002_i2c_reg_write(data, 3);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x07;
	data[3] = 0x00;
	data[4] = 0x01;
	data[5] = 0xBF;
	data[6] = 0x80;
	data[7] = 0x54;
	data[8] = 0x00;
	data[9] = 0x05;
	ret = usb7002_i2c_reg_write(data, 10);
	if (ret < 0)
		goto i2c_fail;
	data[0] = 0x99;
	data[1] = 0x37;
	data[2] = 0x00;
	ret = usb7002_i2c_reg_write(data, 3);
	if (ret < 0)
		goto i2c_fail;

i2c_fail:
	return ret;
}
EXPORT_SYMBOL(usb7002_switch_host);

static int usb7002_i2c_probe(struct i2c_client *client,
		const struct i2c_device_id *id) {

	int ret = 0;

	u7002 = kzalloc(sizeof(*u7002), GFP_KERNEL);
	if (!u7002) {
		pr_err("%s Memory allocation failed\n", __func__);
		return -ENOMEM;
	}

	u7002->client = client;
	u7002->dev = &client->dev;
	i2c_set_clientdata(client, u7002);

	ret = usb7002_switch_peripheral();
	if (ret < 0) {
		pr_err("%s flex failed\n", __func__);
		goto free_mem;
	}
	pr_info("%s success\n", __func__);
	return 0;

free_mem:
	kfree(u7002);
	u7002 = NULL;
	return ret;
}

static int usb7002_i2c_remove(struct i2c_client *client)
{
	kfree(u7002);
	u7002 = NULL;
	return 0;
}

static const struct of_device_id usb7002_of_match[] = {
	{.compatible = "microchip,usb7002",},
	{},
};

MODULE_DEVICE_TABLE(of, usb7002_of_match);

static struct i2c_driver usb7002_driver = {
	.driver = {
		.name = "usb7002-i2c",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(usb7002_of_match),
	},
	.probe = usb7002_i2c_probe,
	.remove = usb7002_i2c_remove,
};

module_i2c_driver(usb7002_driver);

MODULE_DESCRIPTION("usb7002 flex/unflex i2c driver");
MODULE_LICENSE("GPL v2");
