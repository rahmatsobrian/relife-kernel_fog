/*
 *  Driver for Goodix Touchscreens
 *
 *  Copyright (c) 2014 Red Hat Inc.
 *  Copyright (c) 2015 K. Merker <merker@debian.org>
 *
 *  This code is based on gt9xx.c authored by andrew@goodix.com:
 *
 *  2010 - 2012 Goodix Technology.
 */

/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; version 2 of the License.
 */

#include <linux/kernel.h>
#include <linux/dmi.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/workqueue.h>
#include <linux/of.h>
#include <asm/unaligned.h>
#if defined(CONFIG_DRM)
#include <drm/drm_panel.h>
#endif

struct goodix_ts_data;

struct goodix_chip_data {
	u16 config_addr;
	int config_len;
	int (*check_config)(struct goodix_ts_data *, const struct firmware *);
};

struct goodix_ts_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	const struct goodix_chip_data *chip;
	struct touchscreen_properties prop;
	unsigned int max_touch_num;
	unsigned int int_trigger_type;
	struct gpio_desc *gpiod_int;
	struct gpio_desc *gpiod_rst;
	struct regulator *vdd_ana;
	struct regulator *vcc_i2c;
	u16 id;
	u16 version;
	const char *cfg_name;
	int cfg_len;
#if !defined (CONFIG_OF)
	struct completion firmware_loading_complete;
#endif
	unsigned long irq_flags;
	bool regulator_support;
	struct delayed_work tch_cfg;
#if defined(CONFIG_DRM)
	struct notifier_block nb;
#endif
};

#if defined(CONFIG_DRM)
static struct drm_panel *active_goodix_panel;
#endif

#define GOODIX_GPIO_INT_NAME		"irq"
#define GOODIX_GPIO_RST_NAME		"reset"

#define GOODIX_MAX_HEIGHT		4096
#define GOODIX_MAX_WIDTH		4096
#define GOODIX_INT_TRIGGER		1
#define GOODIX_CONTACT_SIZE		8
#define GOODIX_MAX_CONTACTS		10

#define GOODIX_CONFIG_MAX_LENGTH	240
#define GOODIX_CONFIG_911_LENGTH	186
#define GOODIX_CONFIG_967_LENGTH	228
#define GOODIX_ADDR_LEN  		2
#define GOODIX_MAX_SENSOR		6

/* Register defines */
#define GOODIX_REG_COMMAND		0x8040
#define GOODIX_CMD_SCREEN_OFF		0x05

#define GOODIX_READ_COOR_ADDR		0x814E
#define GOODIX_GT1X_REG_CONFIG_DATA	0x8050
#define GOODIX_GT9X_REG_CONFIG_DATA	0x8047
#define GOODIX_REG_ID			0x8140
#define GOODIX_REG_SENSOR_ID		0x814A

#define GOODIX_BUFFER_STATUS_READY	BIT(7)
#define GOODIX_BUFFER_STATUS_TIMEOUT	20

#define RESOLUTION_LOC		1
#define MAX_CONTACTS_LOC	5
#define TRIGGER_LOC		6

#define GOODIX_CFG_MAX_STR	15
#define TCH_CFG_DELAY		10000	/* 10 sec */

static int goodix_check_cfg_8(struct goodix_ts_data *ts,
			const struct firmware *cfg);
static int goodix_check_cfg_16(struct goodix_ts_data *ts,
			const struct firmware *cfg);

static const struct goodix_chip_data gt1x_chip_data = {
	.config_addr		= GOODIX_GT1X_REG_CONFIG_DATA,
	.config_len		= GOODIX_CONFIG_MAX_LENGTH,
	.check_config		= goodix_check_cfg_16,
};

static const struct goodix_chip_data gt911_chip_data = {
	.config_addr		= GOODIX_GT9X_REG_CONFIG_DATA,
	.config_len		= GOODIX_CONFIG_911_LENGTH,
	.check_config		= goodix_check_cfg_8,
};

static const struct goodix_chip_data gt967_chip_data = {
	.config_addr		= GOODIX_GT9X_REG_CONFIG_DATA,
	.config_len		= GOODIX_CONFIG_967_LENGTH,
	.check_config		= goodix_check_cfg_8,
};

static const struct goodix_chip_data gt9x_chip_data = {
	.config_addr		= GOODIX_GT9X_REG_CONFIG_DATA,
	.config_len		= GOODIX_CONFIG_MAX_LENGTH,
	.check_config		= goodix_check_cfg_8,
};

static const unsigned long goodix_irq_flags[] = {
	IRQ_TYPE_EDGE_RISING,
	IRQ_TYPE_EDGE_FALLING,
	IRQ_TYPE_LEVEL_LOW,
	IRQ_TYPE_LEVEL_HIGH,
};

/* Array to hold firmware bytes */
u8 fw_cfg[GOODIX_CONFIG_MAX_LENGTH + GOODIX_ADDR_LEN] =
		{GOODIX_GT9X_REG_CONFIG_DATA >> 8, GOODIX_GT9X_REG_CONFIG_DATA & 0xff};

/* Forward declarations */
static int regulator_en_dis(struct goodix_ts_data *ts, bool on);
static int goodix_ts_suspend(struct goodix_ts_data *ts);
static int goodix_ts_resume(struct goodix_ts_data *ts);
static int goodix_register_lpm(struct goodix_ts_data *ts);
static int goodix_probe_panel(struct device_node *node);

/*
 * Those tablets have their coordinates origin at the bottom right
 * of the tablet, as if rotated 180 degrees
 */
static const struct dmi_system_id rotated_screen[] = {
#if defined(CONFIG_DMI) && defined(CONFIG_X86)
	{
		.ident = "Teclast X89",
		.matches = {
			/* tPAD is too generic, also match on bios date */
			DMI_MATCH(DMI_BOARD_VENDOR, "TECLAST"),
			DMI_MATCH(DMI_BOARD_NAME, "tPAD"),
			DMI_MATCH(DMI_BIOS_DATE, "12/19/2014"),
		},
	},
	{
		.ident = "Teclast X98 Pro",
		.matches = {
			/*
			 * Only match BIOS date, because the manufacturers
			 * BIOS does not report the board name at all
			 * (sometimes)...
			 */
			DMI_MATCH(DMI_BOARD_VENDOR, "TECLAST"),
			DMI_MATCH(DMI_BIOS_DATE, "10/28/2015"),
		},
	},
	{
		.ident = "WinBook TW100",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "WinBook"),
			DMI_MATCH(DMI_PRODUCT_NAME, "TW100")
		}
	},
	{
		.ident = "WinBook TW700",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "WinBook"),
			DMI_MATCH(DMI_PRODUCT_NAME, "TW700")
		},
	},
#endif
	{}
};

/**
 * goodix_i2c_read - read data from a register of the i2c slave device.
 *
 * @client: i2c device.
 * @reg: the register to read from.
 * @buf: raw write data buffer.
 * @len: length of the buffer to write
 */
static int goodix_i2c_read(struct i2c_client *client,
			   u16 reg, u8 *buf, int len)
{
	struct i2c_msg msgs[2];
	__be16 wbuf = cpu_to_be16(reg);
	int ret;

	msgs[0].flags = 0;
	msgs[0].addr  = client->addr;
	msgs[0].len   = 2;
	msgs[0].buf   = (u8 *)&wbuf;

	msgs[1].flags = I2C_M_RD;
	msgs[1].addr  = client->addr;
	msgs[1].len   = len;
	msgs[1].buf   = buf;

	ret = i2c_transfer(client->adapter, msgs, 2);

	return ret < 0 ? ret : (ret != ARRAY_SIZE(msgs) ? -EIO : 0);
}

/**
 * goodix_i2c_write - write data to a register of the i2c slave device.
 *
 * @client: i2c device.
 * @reg: the register to write to.
 * @buf: raw data buffer to write.
 * @len: length of the buffer to write
 */
static int goodix_i2c_write(struct i2c_client *client, u16 reg, const u8 *buf,
			    unsigned len)
{
	u8 *addr_buf;
	struct i2c_msg msg;
	int ret;

	addr_buf = kmalloc(len + 2, GFP_KERNEL);
	if (!addr_buf)
		return -ENOMEM;

	addr_buf[0] = reg >> 8;
	addr_buf[1] = reg & 0xFF;
	memcpy(&addr_buf[2], buf, len);

	msg.flags = 0;
	msg.addr = client->addr;
	msg.buf = addr_buf;
	msg.len = len + 2;

	ret = i2c_transfer(client->adapter, &msg, 1);
	kfree(addr_buf);

	return ret < 0 ? ret : (ret != 1 ? -EIO : 0);
}

static int goodix_i2c_write_u8(struct i2c_client *client, u16 reg, u8 value)
{
	return goodix_i2c_write(client, reg, &value, sizeof(value));
}

static const struct goodix_chip_data *goodix_get_chip_data(u16 id)
{
	switch (id) {
	case 1151:
		return &gt1x_chip_data;

	case 911:
	case 9271:
	case 9110:
	case 927:
	case 928:
		return &gt911_chip_data;

	case 912:
	case 967:
		return &gt967_chip_data;

	default:
		return &gt9x_chip_data;
	}
}

static int goodix_ts_read_input_report(struct goodix_ts_data *ts, u8 *data)
{
	unsigned long max_timeout;
	int touch_num;
	int error;

	/*
	 * The 'buffer status' bit, which indicates that the data is valid, is
	 * not set as soon as the interrupt is raised, but slightly after.
	 * This takes around 10 ms to happen, so we poll for 20 ms.
	 */
	max_timeout = jiffies + msecs_to_jiffies(GOODIX_BUFFER_STATUS_TIMEOUT);
	do {
		error = goodix_i2c_read(ts->client, GOODIX_READ_COOR_ADDR,
					data, GOODIX_CONTACT_SIZE + 1);
		if (error) {
			dev_err(&ts->client->dev, "I2C transfer error: %d\n",
					error);
			return error;
		}

		if (data[0] & GOODIX_BUFFER_STATUS_READY) {
			touch_num = data[0] & 0x0f;
			if (touch_num > ts->max_touch_num)
				return -EPROTO;

			if (touch_num > 1) {
				data += 1 + GOODIX_CONTACT_SIZE;
				error = goodix_i2c_read(ts->client,
						GOODIX_READ_COOR_ADDR +
							1 + GOODIX_CONTACT_SIZE,
						data,
						GOODIX_CONTACT_SIZE *
							(touch_num - 1));
				if (error)
					return error;
			}

			return touch_num;
		}

		usleep_range(1000, 2000); /* Poll every 1 - 2 ms */
	} while (time_before(jiffies, max_timeout));

	/*
	 * The Goodix panel will send spurious interrupts after a
	 * 'finger up' event, which will always cause a timeout.
	 */
	return 0;
}

static void goodix_ts_report_touch(struct goodix_ts_data *ts, u8 *coor_data)
{
	int id = coor_data[0] & 0x0F;
	int input_x = get_unaligned_le16(&coor_data[1]);
	int input_y = get_unaligned_le16(&coor_data[3]);
	int input_w = get_unaligned_le16(&coor_data[5]);

	input_mt_slot(ts->input_dev, id);
	input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
	touchscreen_report_pos(ts->input_dev, &ts->prop,
			       input_x, input_y, true);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, input_w);
	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, input_w);
}

/**
 * goodix_process_events - Process incoming events
 *
 * @ts: our goodix_ts_data pointer
 *
 * Called when the IRQ is triggered. Read the current device state, and push
 * the input events to the user space.
 */
static void goodix_process_events(struct goodix_ts_data *ts)
{
	u8  point_data[1 + GOODIX_CONTACT_SIZE * GOODIX_MAX_CONTACTS];
	int touch_num;
	int i;

	dev_dbg(&ts->client->dev, "%s: enter\n", __func__);

	touch_num = goodix_ts_read_input_report(ts, point_data);
	if (touch_num < 0)
		return;

	/*
	 * Bit 4 of the first byte reports the status of the capacitive
	 * Windows/Home button.
	 */
	input_report_key(ts->input_dev, KEY_LEFTMETA, point_data[0] & BIT(4));

	for (i = 0; i < touch_num; i++)
		goodix_ts_report_touch(ts,
				&point_data[1 + GOODIX_CONTACT_SIZE * i]);

	input_mt_sync_frame(ts->input_dev);
	input_sync(ts->input_dev);

	dev_dbg(&ts->client->dev, "%s: exit\n", __func__);
}

/**
 * goodix_ts_irq_handler - The IRQ handler
 *
 * @irq: interrupt number.
 * @dev_id: private data pointer.
 */
static irqreturn_t goodix_ts_irq_handler(int irq, void *dev_id)
{
	struct goodix_ts_data *ts = dev_id;

	goodix_process_events(ts);

	if (goodix_i2c_write_u8(ts->client, GOODIX_READ_COOR_ADDR, 0) < 0)
		dev_err(&ts->client->dev, "I2C write end_cmd error\n");

	return IRQ_HANDLED;
}

static void goodix_free_irq(struct goodix_ts_data *ts)
{
	devm_free_irq(&ts->client->dev, ts->client->irq, ts);
}

static int goodix_request_irq(struct goodix_ts_data *ts)
{
	return devm_request_threaded_irq(&ts->client->dev, ts->client->irq,
					 NULL, goodix_ts_irq_handler,
					 ts->irq_flags, ts->client->name, ts);
}

static int goodix_check_cfg_8(struct goodix_ts_data *ts,
			const struct firmware *cfg)
{
	int i, raw_cfg_len = cfg->size - 2;
	u8 check_sum = 0;

	for (i = 0; i < raw_cfg_len; i++)
		check_sum += cfg->data[i];
	check_sum = (~check_sum) + 1;
	if (check_sum != cfg->data[raw_cfg_len]) {
		dev_err(&ts->client->dev,
			"The checksum of the config fw is not correct");
		return -EINVAL;
	}

	if (cfg->data[raw_cfg_len + 1] != 1) {
		dev_err(&ts->client->dev,
			"Config fw must have Config_Fresh register set");
		return -EINVAL;
	}

	return 0;
}

static int goodix_check_cfg_16(struct goodix_ts_data *ts,
			const struct firmware *cfg)
{
	int i, raw_cfg_len = cfg->size - 3;
	u16 check_sum = 0;

	for (i = 0; i < raw_cfg_len; i += 2)
		check_sum += get_unaligned_be16(&cfg->data[i]);
	check_sum = (~check_sum) + 1;
	if (check_sum != get_unaligned_be16(&cfg->data[raw_cfg_len])) {
		dev_err(&ts->client->dev,
			"The checksum of the config fw is not correct");
		return -EINVAL;
	}

	if (cfg->data[raw_cfg_len + 2] != 1) {
		dev_err(&ts->client->dev,
			"Config fw must have Config_Fresh register set");
		return -EINVAL;
	}

	return 0;
}

#if !defined(CONFIG_OF)
/**
 * goodix_check_cfg - Checks if config fw is valid
 *
 * @ts: goodix_ts_data pointer
 * @cfg: firmware config data
 */
static int goodix_check_cfg(struct goodix_ts_data *ts,
			    const struct firmware *cfg)
{
	if (cfg->size > GOODIX_CONFIG_MAX_LENGTH) {
		dev_err(&ts->client->dev,
			"The length of the config fw is not correct");
		return -EINVAL;
	}

	return ts->chip->check_config(ts, cfg);
}

/**
 * goodix_send_cfg - Write fw config to device
 *
 * @ts: goodix_ts_data pointer
 * @cfg: config firmware to write to device
 */
static int goodix_send_cfg(struct goodix_ts_data *ts,
			   const struct firmware *cfg)
{
	int error;

	error = goodix_check_cfg(ts, cfg);
	if (error)
		return error;

	error = goodix_i2c_write(ts->client, ts->chip->config_addr, cfg->data,
				 cfg->size);
	if (error) {
		dev_err(&ts->client->dev, "Failed to write config data: %d",
			error);
		return error;
	}
	dev_dbg(&ts->client->dev, "Config sent successfully.");

	/* Let the firmware reconfigure itself, so sleep for 10ms */
	usleep_range(10000, 11000);

	return 0;
}
#else
/**
 * goodix_check_cfg - Checks if config fw is valid
 *
 * @ts: goodix_ts_data pointer
 */
static int goodix_check_cfg(struct goodix_ts_data *ts)
{
	int i, raw_cfg_len = ts->cfg_len - 2;
	u8 check_sum = 0;

	if (ts->cfg_len > GOODIX_CONFIG_MAX_LENGTH) {
		dev_err(&ts->client->dev,
			"The length of the config fw is not correct");
		return -EINVAL;
	}

	for (i = 0; i < raw_cfg_len; i++)
		check_sum += fw_cfg[i];

	check_sum = (~check_sum) + 1;

	if (check_sum != fw_cfg[raw_cfg_len]) {
		dev_err(&ts->client->dev,
			"The checksum of the config fw is not correct");
		return -EINVAL;
	}

	if (fw_cfg[raw_cfg_len + 1] != 1) {
		dev_err(&ts->client->dev,
			"Config fw must have Config_Fresh register set");
		return -EINVAL;
	}
	return 0;
}

/**
 * goodix_send_cfg - Write fw config to device
 *
 * @client: i2c client
 */
static int goodix_send_cfg(struct i2c_client *client)
{
	int error;
	struct goodix_ts_data *ts = i2c_get_clientdata(client);

	error=goodix_check_cfg(ts);
	if (error)
		return error;

	error = goodix_i2c_write(ts->client, ts->chip->config_addr, fw_cfg,
				 ts->cfg_len);
	if (error) {
		dev_err(&ts->client->dev, "Failed to write config data: %d",
			error);
		return error;
	}

	/* Let the firmware reconfigure itself, so sleep for 10ms */
	usleep_range(10000, 11000);

	return 0;
}
#endif

static int goodix_int_sync(struct goodix_ts_data *ts)
{
	int error;

	error = gpiod_direction_output(ts->gpiod_int, 0);
	if (error)
		return error;

	msleep(50);				/* T5: 50ms */

	error = gpiod_direction_input(ts->gpiod_int);
	if (error)
		return error;

	return 0;
}

/**
 * goodix_reset - Reset device during power on
 *
 * @ts: goodix_ts_data pointer
 */
static int goodix_reset(struct goodix_ts_data *ts)
{
	int error;

	dev_dbg(&ts->client->dev, "%s: enter\n", __func__);

	/* begin select I2C slave addr */
	error = gpiod_direction_output(ts->gpiod_rst, 0);
	if (error)
		return error;

	msleep(20);				/* T2: > 10ms */

	/* HIGH: 0x28/0x29, LOW: 0xBA/0xBB */
	error = gpiod_direction_output(ts->gpiod_int, ts->client->addr == 0x14);
	if (error)
		return error;

	usleep_range(100, 2000);		/* T3: > 100us */

	error = gpiod_direction_output(ts->gpiod_rst, 1);
	if (error)
		return error;

	usleep_range(6000, 10000);		/* T4: > 5ms */

	/* end select I2C slave addr */
	error = gpiod_direction_input(ts->gpiod_rst);
	if (error)
		return error;

	error = goodix_int_sync(ts);
	if (error)
		return error;

	dev_dbg(&ts->client->dev, "%s: exit\n", __func__);
	return 0;
}

/**
 * goodix_get_gpio_config - Get GPIO config from ACPI/DT
 *
 * @ts: goodix_ts_data pointer
 */
static int goodix_get_gpio_config(struct goodix_ts_data *ts)
{
	int error;
	struct device *dev;
	struct gpio_desc *gpiod;

	if (!ts->client)
		return -EINVAL;

	dev = &ts->client->dev;

	/* Get the interrupt GPIO pin number */
	gpiod = devm_gpiod_get_optional(dev, GOODIX_GPIO_INT_NAME, GPIOD_IN);
	if (IS_ERR(gpiod)) {
		error = PTR_ERR(gpiod);
		if (error != -EPROBE_DEFER)
			dev_err(dev, "Failed to get %s GPIO: %d\n",
				GOODIX_GPIO_INT_NAME, error);
		return error;
	}

	ts->gpiod_int = gpiod;


	/* Get the reset line GPIO pin number */
	gpiod = devm_gpiod_get_optional(dev, GOODIX_GPIO_RST_NAME, GPIOD_IN);
	if (IS_ERR(gpiod)) {
		error = PTR_ERR(gpiod);
		if (error != -EPROBE_DEFER)
			dev_err(dev, "Failed to get %s GPIO: %d\n",
				GOODIX_GPIO_RST_NAME, error);
		return error;
	}

	ts->gpiod_rst = gpiod;

	return 0;
}

/**
 * goodix_read_config - Read the embedded configuration of the panel
 *
 * @ts: our goodix_ts_data pointer
 *
 * Must be called during probe
 */
static void goodix_read_config(struct goodix_ts_data *ts)
{
	u8 config[GOODIX_CONFIG_MAX_LENGTH];
	int x_max, y_max;
	int error;

	error = goodix_i2c_read(ts->client, ts->chip->config_addr,
				config, ts->chip->config_len);
	if (error) {
		dev_err(&ts->client->dev, "Error reading config: %d\n",
			 error);
		ts->int_trigger_type = GOODIX_INT_TRIGGER;
		ts->max_touch_num = GOODIX_MAX_CONTACTS;
		return;
	}

	ts->int_trigger_type = config[TRIGGER_LOC] & 0x03;
	ts->max_touch_num = config[MAX_CONTACTS_LOC] & 0x0f;

	x_max = get_unaligned_le16(&config[RESOLUTION_LOC]);
	y_max = get_unaligned_le16(&config[RESOLUTION_LOC + 2]);
	if (x_max && y_max) {
		input_abs_set_max(ts->input_dev, ABS_MT_POSITION_X, x_max - 1);
		input_abs_set_max(ts->input_dev, ABS_MT_POSITION_Y, y_max - 1);
	}
}

/**
 * goodix_read_version - Read goodix touchscreen version
 *
 * @ts: our goodix_ts_data pointer
 */
static int goodix_read_version(struct goodix_ts_data *ts)
{
	int error;
	u8 buf[6];
	char id_str[5];

	error = goodix_i2c_read(ts->client, GOODIX_REG_ID, buf, sizeof(buf));
	if (error) {
		dev_err(&ts->client->dev, "read version failed: %d\n", error);
		return error;
	}

	memcpy(id_str, buf, 4);
	id_str[4] = 0;
	if (kstrtou16(id_str, 10, &ts->id))
		ts->id = 0x1001;

	ts->version = get_unaligned_le16(&buf[4]);

	dev_info(&ts->client->dev, "ID %d, version: %04x\n", ts->id,
		 ts->version);

	return 0;
}

/**
 * goodix_i2c_test - I2C test function to check if the device answers.
 *
 * @client: the i2c client
 */
static int goodix_i2c_test(struct i2c_client *client)
{
	int retry = 0;
	int error;
	u8 test;

	dev_dbg(&client->dev, "%s: enter\n", __func__);

	while (retry++ < 2) {
		error = goodix_i2c_read(client, GOODIX_REG_ID,
					&test, 1);
		if (!error)
			return 0;

		dev_err(&client->dev, "i2c test failed attempt %d: %d\n",
			retry, error);
		msleep(20);
	}

	dev_dbg(&client->dev, "%s: exit\n", __func__);
	return error;
}

/**
 * goodix_configure_dev - Finish device initialization
 *
 * @ts: our goodix_ts_data pointer
 *
 * Must be called from probe to finish initialization of the device.
 * Contains the common initialization code for both devices that
 * declare gpio pins and devices that do not. It is either called
 * directly from probe or from request_firmware_wait callback.
 */
static int goodix_configure_dev(struct goodix_ts_data *ts)
{
	int error;
	struct device_node *np = ts->client->dev.of_node;

	ts->int_trigger_type = GOODIX_INT_TRIGGER;
	ts->max_touch_num = GOODIX_MAX_CONTACTS;

	ts->input_dev = devm_input_allocate_device(&ts->client->dev);
	if (!ts->input_dev) {
		dev_err(&ts->client->dev, "Failed to allocate input device.");
		return -ENOMEM;
	}

	ts->input_dev->name = "Goodix Capacitive TouchScreen";
	ts->input_dev->phys = "input/ts";
	ts->input_dev->id.bustype = BUS_I2C;
	ts->input_dev->id.vendor = 0x0416;
	ts->input_dev->id.product = ts->id;
	ts->input_dev->id.version = ts->version;

	/* Capacitive Windows/Home button on some devices */
	input_set_capability(ts->input_dev, EV_KEY, KEY_LEFTMETA);

	input_set_capability(ts->input_dev, EV_ABS, ABS_MT_POSITION_X);
	input_set_capability(ts->input_dev, EV_ABS, ABS_MT_POSITION_Y);
	input_set_abs_params(ts->input_dev, ABS_MT_WIDTH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);

	/* Read configuration and apply touchscreen parameters */
	goodix_read_config(ts);

	/* Try overriding touchscreen parameters via device properties */
	touchscreen_parse_properties(ts->input_dev, true, &ts->prop);

	if (!ts->prop.max_x || !ts->prop.max_y || !ts->max_touch_num) {
		dev_err(&ts->client->dev, "Invalid config, using defaults\n");
		ts->prop.max_x = GOODIX_MAX_WIDTH - 1;
		ts->prop.max_y = GOODIX_MAX_HEIGHT - 1;
		ts->max_touch_num = GOODIX_MAX_CONTACTS;
		input_abs_set_max(ts->input_dev,
				  ABS_MT_POSITION_X, ts->prop.max_x);
		input_abs_set_max(ts->input_dev,
				  ABS_MT_POSITION_Y, ts->prop.max_y);
	}

	if (dmi_check_system(rotated_screen)) {
		ts->prop.invert_x = true;
		ts->prop.invert_y = true;
		dev_info(&ts->client->dev,
			"Applying '180 degrees rotated screen' quirk\n");
	}

	error = input_mt_init_slots(ts->input_dev, ts->max_touch_num,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error) {
		dev_err(&ts->client->dev,
			"Failed to initialize MT slots: %d", error);
		return error;
	}

	error = input_register_device(ts->input_dev);
	if (error) {
		dev_err(&ts->client->dev,
			"Failed to register input device: %d", error);
		return error;
	}

	/* Find the panel before registering */
	if(!goodix_probe_panel(np))
		dev_info(&ts->client->dev, "Panel probed successfully\n");

	/* Register for LPM Handling */
	if(!goodix_register_lpm(ts))
		dev_info(&ts->client->dev, "LPM registered successfully\n");

	ts->irq_flags = goodix_irq_flags[ts->int_trigger_type] | IRQF_ONESHOT;
	error = goodix_request_irq(ts);
	if (error) {
		dev_err(&ts->client->dev, "request IRQ failed: %d\n", error);
		return error;
	}

	dev_info(&ts->client->dev, "Touch device configured successfully\n");
	return 0;
}
#if !defined(CONFIG_OF)
/**
 * goodix_config_cb - Callback to finish device init
 *
 * @ts: our goodix_ts_data pointer
 *
 * request_firmware_wait callback that finishes
 * initialization of the device.
 */
static void goodix_config_cb(const struct firmware *cfg, void *ctx)
{
	struct goodix_ts_data *ts = ctx;
	int error;

	if (cfg) {
		/* send device configuration to the firmware */
		error = goodix_send_cfg(ts, cfg);
		if (error)
			goto err_release_cfg;
	}

	goodix_configure_dev(ts);

err_release_cfg:
	release_firmware(cfg);
	complete_all(&ts->firmware_loading_complete);
}
#endif

static int goodix_get_sensor_id(struct goodix_ts_data *ts){
	int ret = 0;
	u8 buf = 0;

	ret = goodix_i2c_read(ts->client, GOODIX_REG_SENSOR_ID, &buf, sizeof(buf));
	if(ret){
		dev_err(&ts->client->dev, "Read sensor id failed %d\n", ret);
		return ret;
	}
	return buf;
}

static int goodix_parse_dt(struct goodix_ts_data *ts, int *sid){
	struct device_node *np = ts->client->dev.of_node;
	struct property *prop;
	char cfg_name[GOODIX_CFG_MAX_STR];
	u8 check_sum=0;
	int i;

	snprintf(cfg_name, sizeof(cfg_name), "goodix,config%d", *sid);

	dev_info(&ts->client->dev,"cfg_name %s sensor-id %d\n", cfg_name, *sid);

	prop = of_find_property(np, cfg_name, &ts->cfg_len);
	if (!prop || !prop->value || ts->cfg_len == 0 || ts->cfg_len > GOODIX_CONFIG_MAX_LENGTH) {
		dev_err(&ts->client->dev, "Reading prop failed\n");
		return -1;
	}

	/* Copy the firmware */
	memcpy(fw_cfg, prop->value, ts->cfg_len);

	if (ts->cfg_len < GOODIX_CONFIG_911_LENGTH){
		dev_err(&ts->client->dev, "Invalid touch config\n");
		return -1;
	}

	for(i=0; i<ts->cfg_len; i++)
		check_sum += fw_cfg[i];

	fw_cfg[ts->cfg_len] = (~check_sum) + 1;

	/* Schedule the worker now to configure touch controller */
	queue_delayed_work(system_wq, &ts->tch_cfg, msecs_to_jiffies(TCH_CFG_DELAY));
	return 0;
}

static int goodix_notifier_callback(struct notifier_block *noti,
					unsigned long event, void *data){
	struct drm_panel_notifier *evt_data = data;
	struct goodix_ts_data *ts = container_of(noti, struct goodix_ts_data, nb);
	int *pblank;

	if (evt_data && evt_data->data && event == DRM_PANEL_EVENT_BLANK && ts) {
		pblank = evt_data->data;
		if (*pblank == DRM_PANEL_BLANK_UNBLANK) {
			dev_info(&ts->client->dev, "Resume via DRM notifier\n");
			goodix_ts_resume(ts);
		} else if (*pblank == DRM_PANEL_BLANK_POWERDOWN) {
			dev_info(&ts->client->dev, "Suspend via DRM notifier\n");
			goodix_ts_suspend(ts);
		}
	}

	return 0;
}

static int goodix_register_lpm(struct goodix_ts_data *ts){
#if defined(CONFIG_DRM)
	ts->nb.notifier_call = goodix_notifier_callback;
	if (active_goodix_panel &&
		drm_panel_notifier_register(active_goodix_panel, &ts->nb) < 0)
		dev_err(&ts->client->dev, "Failed to register for DRM notifier\n");
#endif
	return 0;
}

static int goodix_unregister_lpm(struct goodix_ts_data *ts){
#if defined(CONFIG_DRM)
	if (active_goodix_panel)
		drm_panel_notifier_unregister(active_goodix_panel, &ts->nb);
#endif
	return 0;
}

static int goodix_probe_panel(struct device_node *node){
	struct device_node *np;
	struct drm_panel *panel;
	int i, cnt;

	cnt = of_count_phandle_with_args(node, "panel", NULL);

	if (cnt <=0)
		return 0;

	for(i=0; i<cnt; i++){
		np = of_parse_phandle(node, "panel", i);
		panel = of_drm_find_panel(np);
		of_node_put(np);
		if (!IS_ERR(panel)){
			active_goodix_panel = panel;
			pr_info("panel: %pOF\n", panel->dev->of_node);
			return 0;
		} else
			return (PTR_ERR(panel));
	}
	return -ENODEV;
}

static void goodix_touch_configure_work(struct work_struct *work){
	struct goodix_ts_data *ts = container_of(work,
						struct goodix_ts_data, tch_cfg.work);
	int error;

	if (!ts) {
		pr_err("%s: invalid touch driver context\n", __func__);
		return;
	}

	/* Configure the touch controller */
	error = goodix_send_cfg(ts->client);
	if (error < 0){
		dev_err(&ts->client->dev, "Failed to get firmware %d\n", error);
			return;
	}

	goodix_configure_dev(ts);
}

static int goodix_ts_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct goodix_ts_data *ts;
	struct device *dev = &client->dev;
	int error, ret=0;
	int sensor_id;

	dev_dbg(&client->dev, "%s: I2C Address: 0x%02x\n", __func__, client->addr);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "I2C check functionality failed.\n");
		return -ENXIO;
	}

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	i2c_set_clientdata(client, ts);
#if !defined(CONFIG_OF)
	init_completion(&ts->firmware_loading_complete);
#endif
	/* Check for regulator support */
	ts->regulator_support = of_property_read_bool(dev->of_node, "goodix,regulator-support");
	if(ts->regulator_support) {
		/* Get and enable regulators to control runtime supply */
		ts->vdd_ana = regulator_get(dev, "vdd_ana");
		if (IS_ERR(ts->vdd_ana)) {
			dev_err(dev,"regulator get of vdd_ana failed");
			ret = PTR_ERR(ts->vdd_ana);
			ts->vdd_ana = NULL;
		}

		ts->vcc_i2c = regulator_get(dev, "vcc_i2c");
		if (IS_ERR(ts->vcc_i2c)) {
			dev_err(dev,"regulator get of vcc_i2c failed");
			ret = PTR_ERR(ts->vcc_i2c);
			ts->vcc_i2c = NULL;
		}

		regulator_en_dis(ts, true);
	}

	error = goodix_get_gpio_config(ts);
	if (error)
		return error;

	if (ts->gpiod_int && ts->gpiod_rst) {
		/* reset the controller */
		error = goodix_reset(ts);
		if (error) {
			dev_err(&client->dev, "Controller reset failed.\n");
			return error;
		}
	}

	error = goodix_i2c_test(client);
	if (error) {
		dev_err(&client->dev, "I2C communication failure: %d\n", error);
		return error;
	}

	error = goodix_read_version(ts);
	if (error) {
		dev_err(&client->dev, "Read version failed.\n");
		return error;
	}

	ts->chip = goodix_get_chip_data(ts->id);

	sensor_id = goodix_get_sensor_id(ts);

	if (sensor_id > GOODIX_MAX_SENSOR){
		dev_err(&client->dev, "Invalid sensor id %#x\n", sensor_id);
		return -1;
	}

	if (ts->gpiod_int && ts->gpiod_rst) {
#if !defined(CONFIG_OF)
		/*
		 * This is going to read the firmware binary from one of the
		 * linux paths e.g. /vendor/etc, /vendor/firmware
		 * Update device config
		 */
		ts->cfg_name = devm_kasprintf(&client->dev, GFP_KERNEL,
					      "goodix_%d_cfg.bin", ts->id);
		if (!ts->cfg_name)
			return -ENOMEM;

		error = request_firmware_nowait(THIS_MODULE, true, ts->cfg_name,
						&client->dev, GFP_KERNEL, ts,
						goodix_config_cb);
		if (error) {
			dev_err(&client->dev,
				"Failed to invoke firmware loader: %d\n",
				error);
			return error;
		}
#else
		INIT_DELAYED_WORK(&ts->tch_cfg, goodix_touch_configure_work);
		/*
		 * Read the firmware from device-tree
		 */
		error = goodix_parse_dt(ts, &sensor_id);
		if (error < 0){
			dev_err(&client->dev, "Failed to get firmware %d\n", error);
			return -1;
		}
#endif
	} else {
		error = goodix_configure_dev(ts);
		if (error)
			return error;
	}

	dev_info(&client->dev, "ts_probe exited successfully\n");
	return 0;
}

static int goodix_ts_remove(struct i2c_client *client)
{
	struct goodix_ts_data *ts = i2c_get_clientdata(client);

#if !defined (CONFIG_OF)
	if (ts->gpiod_int && ts->gpiod_rst)
		wait_for_completion(&ts->firmware_loading_complete);
#endif
	goodix_unregister_lpm(ts);

	regulator_put(ts->vdd_ana);
	regulator_put(ts->vcc_i2c);

	cancel_delayed_work(&ts->tch_cfg);
	return 0;
}

static int regulator_en_dis(struct goodix_ts_data *ts, bool enable) {
	dev_dbg(&ts->client->dev, "regulators %s\n", enable?"enabled":"disabled");

	if (enable){
		regulator_enable(ts->vdd_ana);
		regulator_enable(ts->vcc_i2c);
	} else {
		regulator_disable(ts->vdd_ana);
		regulator_disable(ts->vcc_i2c);
	}

	return 0;
}

#if defined(CONFIG_DRM)
static int goodix_ts_suspend(struct goodix_ts_data *ts){
	int error;

	dev_dbg(&ts->client->dev, "%s: enter\n", __func__);

	/* We need gpio pins to suspend/resume */
	if (!ts->gpiod_int || !ts->gpiod_rst) {
		disable_irq(ts->client->irq);
		return 0;
	}

#if !defined (CONFIG_OF)
	wait_for_completion(&ts->firmware_loading_complete);
#endif

	/* Free IRQ as IRQ pin is used as output in the suspend sequence */
	goodix_free_irq(ts);

	/* Output LOW on the INT pin for 5 ms */
	error = gpiod_direction_output(ts->gpiod_int, 0);
	if (error) {
		goodix_request_irq(ts);
		return error;
	}

	usleep_range(5000, 6000);

	error = goodix_i2c_write_u8(ts->client, GOODIX_REG_COMMAND,
				    GOODIX_CMD_SCREEN_OFF);
	if (error) {
		dev_err(&ts->client->dev, "Screen off command failed\n");
		gpiod_direction_input(ts->gpiod_int);
		goodix_request_irq(ts);
		return -EAGAIN;
	}

	if (ts->regulator_support) {
		regulator_en_dis(ts, false);
		msleep(20);
	}

	/*
	 * The datasheet specifies that the interval between sending screen-off
	 * command and wake-up should be longer than 58 ms. To avoid waking up
	 * sooner, delay 58ms here.
	 */
	msleep(58);

	dev_dbg(&ts->client->dev, "%s: exit\n", __func__);
	return 0;
}

static int goodix_ts_resume(struct goodix_ts_data *ts){
	int error;

	dev_dbg(&ts->client->dev, "%s: enter\n", __func__);

	if (ts->regulator_support) {
		if (!(regulator_is_enabled(ts->vdd_ana))||
			!(regulator_is_enabled(ts->vcc_i2c)))
			regulator_en_dis(ts, true);
			msleep(20);
	}

	if (!ts->gpiod_int || !ts->gpiod_rst) {
		enable_irq(ts->client->irq);
		return 0;
	}

	/*
	 * Exit sleep mode by outputting HIGH level to INT pin
	 * for 2ms~5ms.
	 */
	error = gpiod_direction_output(ts->gpiod_int, 1);
	if (error)
		return error;

	usleep_range(2000, 5000);

	error = goodix_int_sync(ts);
	if (error)
		return error;

	error = goodix_request_irq(ts);
	if (error)
		return error;

	dev_dbg(&ts->client->dev, "%s: exit\n", __func__);
	return 0;
}
#elif !defined(CONFIG_DRM) && defined(CONFIG_PM)
static int goodix_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct goodix_ts_data *ts = i2c_get_clientdata(client);
	int error;

	dev_dbg(&ts->client->dev, "%s: enter\n", __func__);

	/* We need gpio pins to suspend/resume */
	if (!ts->gpiod_int || !ts->gpiod_rst) {
		disable_irq(client->irq);
		return 0;
	}
#if !defined (CONFIG_OF)
	wait_for_completion(&ts->firmware_loading_complete);
#endif
	/* Free IRQ as IRQ pin is used as output in the suspend sequence */
	goodix_free_irq(ts);

	/* Output LOW on the INT pin for 5 ms */
	error = gpiod_direction_output(ts->gpiod_int, 0);
	if (error) {
		goodix_request_irq(ts);
		return error;
	}

	usleep_range(5000, 6000);

	error = goodix_i2c_write_u8(ts->client, GOODIX_REG_COMMAND,
				    GOODIX_CMD_SCREEN_OFF);
	if (error) {
		dev_err(&ts->client->dev, "Screen off command failed\n");
		gpiod_direction_input(ts->gpiod_int);
		goodix_request_irq(ts);
		return -EAGAIN;
	}

	/*
	 * The datasheet specifies that the interval between sending screen-off
	 * command and wake-up should be longer than 58 ms. To avoid waking up
	 * sooner, delay 58ms here.
	 */
	msleep(58);

	if (ts->regulator_support)
		regulator_en_dis(ts, false);

	dev_dbg("%s: exit\n", __func__);
	return 0;
}

static int goodix_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct goodix_ts_data *ts = i2c_get_clientdata(client);
	int error;

	dev_dbg("%s: enter\n", __func__);

	if (ts->regulator_support)
		regulator_en_dis(ts, true);

	if (!ts->gpiod_int || !ts->gpiod_rst) {
		enable_irq(client->irq);
		return 0;
	}

	/*
	 * Exit sleep mode by outputting HIGH level to INT pin
	 * for 2ms~5ms.
	 */
	error = gpiod_direction_output(ts->gpiod_int, 1);
	if (error)
		return error;

	usleep_range(2000, 5000);

	error = goodix_int_sync(ts);
	if (error)
		return error;

	error = goodix_request_irq(ts);
	if (error)
		return error;

	dev_dbg("%s: exit\n", __func__);
	return 0;
}
static SIMPLE_DEV_PM_OPS(goodix_pm_ops, goodix_suspend, goodix_resume);
#endif

static const struct i2c_device_id goodix_ts_id[] = {
	{ "GDIX1001:00", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, goodix_ts_id);

#ifdef CONFIG_ACPI
static const struct acpi_device_id goodix_acpi_match[] = {
	{ "GDIX1001", 0 },
	{ "GDIX1002", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, goodix_acpi_match);
#endif

#ifdef CONFIG_OF
static const struct of_device_id goodix_of_match[] = {
	{ .compatible = "goodix,gt1151" },
	{ .compatible = "goodix,gt911" },
	{ .compatible = "goodix,gt9110" },
	{ .compatible = "goodix,gt912" },
	{ .compatible = "goodix,gt927" },
	{ .compatible = "goodix,gt9271" },
	{ .compatible = "goodix,gt928" },
	{ .compatible = "goodix,gt967" },
	{ }
};
MODULE_DEVICE_TABLE(of, goodix_of_match);
#endif

static struct i2c_driver goodix_ts_driver = {
	.probe = goodix_ts_probe,
	.remove = goodix_ts_remove,
	.id_table = goodix_ts_id,
	.driver = {
		.name = "Goodix-TS",
		.acpi_match_table = ACPI_PTR(goodix_acpi_match),
		.of_match_table = of_match_ptr(goodix_of_match),
#if !defined(CONFIG_DRM) && defined(CONFIG_PM)
		.pm = &goodix_pm_ops,
#endif
	},
};
module_i2c_driver(goodix_ts_driver);

MODULE_AUTHOR("Benjamin Tissoires <benjamin.tissoires@gmail.com>");
MODULE_AUTHOR("Bastien Nocera <hadess@hadess.net>");
MODULE_DESCRIPTION("Goodix touchscreen driver");
MODULE_LICENSE("GPL v2");
