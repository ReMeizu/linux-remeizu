// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung S5K5E8 (5 Mpix) MIPI CSI-2 Bayer sensor
 *
 * Copyright (C) 2026 Meizu fleet kernel port
 *
 * The front camera of the Meizu M5c (MT6737M) is a S5K5E8 silhouette sensor
 * ("s5k5e8_mipi_raw", 2 lane, I2C 0x30, chip id 0x5e80) and the same part is
 * used as the secondary camera of the M3 Note.  The register sequences below
 * are taken from the vendor driver
 * (drivers/misc/mediatek/imgsensor/src/mt6755/s5k5e8yx_mipi_raw/
 *  s5k5e8yxmipiraw_Sensor.c): sensor_init(), preview_setting() and
 * capture_setting().
 *
 * Unlike the S5K4H8, this sensor uses 8 bit registers only, so the vendor
 * write_cmos_sensor() 16 bit parameter is always truncated to one byte.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "mtk-sensor-legacy.h"

#define S5K5E8_NAME		"s5k5e8"

#define S5K5E8_REG_MODE_SELECT	0x0100
#define S5K5E8_REG_MIRROR	0x0101
#define S5K5E8_REG_ANALOG_GAIN	0x0204
#define S5K5E8_REG_COARSE_EXP	0x0200
#define S5K5E8_REG_GROUP_PARAM	0x0104

#define S5K5E8_MCLK		24000000

/* Vendor gain2reg(): register = gain / 2, V4L2 gain in 1/64 units */
#define S5K5E8_BASE_GAIN	64
#define S5K5E8_GAIN_MIN		S5K5E8_BASE_GAIN
#define S5K5E8_GAIN_MAX		(32 * S5K5E8_BASE_GAIN)

static const struct cci_reg_sequence s5k5e8_init_regs[] = {
#include "s5k5e8_init_regs.inc"
};

static const struct cci_reg_sequence s5k5e8_mode_1296x972[] = {
#include "s5k5e8_mode_1296x972.inc"
};

static const struct cci_reg_sequence s5k5e8_mode_2592x1944[] = {
#include "s5k5e8_mode_2592x1944.inc"
};

static const struct mtk_sensor_mode s5k5e8_modes[] = {
	{
		.width = 1296,
		.height = 972,
		.line_length = 5200,
		.frame_length = 1062,
		.pclk = 168000000,
		.vts_min = 1062,
		.mipi_lanes = 2,
		.link_freq = 432000000,	/* 864 Mb/s per lane */
		.regs = s5k5e8_mode_1296x972,
		.num_regs = ARRAY_SIZE(s5k5e8_mode_1296x972),
	},
	{
		.width = 2592,
		.height = 1944,
		.line_length = 2856,
		.frame_length = 1968,
		.pclk = 168000000,
		.vts_min = 1968,
		.mipi_lanes = 2,
		.link_freq = 432000000,
		.regs = s5k5e8_mode_2592x1944,
		.num_regs = ARRAY_SIZE(s5k5e8_mode_2592x1944),
	},
};

static const char * const s5k5e8_supply_names[] = {
	"avdd",		/* VCAMA, analog 2.8 V */
	"dovdd",	/* VCAMIO, I/O 1.8 V */
	"dvdd",		/* VCAMD, core 1.2 V */
};

static int s5k5e8_set_gain(struct mtk_sensor *sensor, u32 gain)
{
	u16 reg;

	gain = clamp_val(gain, S5K5E8_GAIN_MIN, S5K5E8_GAIN_MAX);
	reg = gain >> 1;

	/* 0x0204 is written as two separate registers by the vendor code */
	return mtk_sensor_write16(sensor, S5K5E8_REG_ANALOG_GAIN, reg);
}

static int s5k5e8_set_exposure(struct mtk_sensor *sensor, u32 lines)
{
	int ret = 0;

	cci_write(sensor->regmap, SENSOR_REG(S5K5E8_REG_GROUP_PARAM), 0x01,
		  &ret);
	mtk_sensor_write16(sensor, S5K5E8_REG_COARSE_EXP, lines);
	cci_write(sensor->regmap, SENSOR_REG(S5K5E8_REG_GROUP_PARAM), 0x00,
		  &ret);

	return ret;
}

static int s5k5e8_set_flip(struct mtk_sensor *sensor, bool hflip, bool vflip)
{
	u8 val = (hflip ? 0x01 : 0x00) | (vflip ? 0x02 : 0x00);

	return cci_write(sensor->regmap, SENSOR_REG(S5K5E8_REG_MIRROR), val,
			 NULL);
}

static const struct mtk_sensor_ops s5k5e8_ops = {
	.set_gain = s5k5e8_set_gain,
	.set_exposure = s5k5e8_set_exposure,
	.set_flip = s5k5e8_set_flip,
};

static int s5k5e8_probe(struct i2c_client *client)
{
	struct mtk_sensor *sensor;

	sensor = devm_kzalloc(&client->dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->name = S5K5E8_NAME;
	sensor->mbus_code = MEDIA_BUS_FMT_SGRBG10_1X10;
	sensor->modes = s5k5e8_modes;
	sensor->num_modes = ARRAY_SIZE(s5k5e8_modes);
	sensor->ops = &s5k5e8_ops;
	sensor->supply_names = s5k5e8_supply_names;
	sensor->num_supplies = ARRAY_SIZE(s5k5e8_supply_names);
	sensor->xclk_rate = S5K5E8_MCLK;
	sensor->power_up_delay_ms = 10;
	sensor->mode_delay_us = 20000;
	sensor->init_regs = s5k5e8_init_regs;
	sensor->num_init_regs = ARRAY_SIZE(s5k5e8_init_regs);
	sensor->gain_min = S5K5E8_GAIN_MIN;
	sensor->gain_max = S5K5E8_GAIN_MAX;

	return mtk_sensor_register(sensor, client);
}

static void s5k5e8_remove(struct i2c_client *client)
{
	mtk_sensor_unregister(client);
}

static const struct of_device_id s5k5e8_of_match[] = {
	{ .compatible = "samsung,s5k5e8" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5k5e8_of_match);

static struct i2c_driver s5k5e8_driver = {
	.driver = {
		.name = "s5k5e8",
		.of_match_table = s5k5e8_of_match,
	},
	.probe = s5k5e8_probe,
	.remove = s5k5e8_remove,
};

module_i2c_driver(s5k5e8_driver);

MODULE_DESCRIPTION("Samsung S5K5E8 MIPI CSI-2 camera sensor driver");
MODULE_AUTHOR("Meizu fleet kernel port");
MODULE_LICENSE("GPL");
