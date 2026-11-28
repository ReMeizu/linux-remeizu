// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony IMX386 (12 Mpix) MIPI CSI-2 Bayer sensor
 *
 * Copyright (C) 2026 Meizu fleet kernel port
 *
 * The MX6 (MT6797) rear camera is a Sony IMX386 module driven by
 * "imx386_mipi_raw" in the vendor tree.  The register sequences below come
 * from the working 3.18 vendor driver
 * (drivers/misc/mediatek/imgsensor/src/mt6797/imx386_mipi_raw/
 *  imx386mipi_sensor.c): sensor_init(), preview_setting() and
 * capture_setting().  The vendor 16 bit accessor write_cmos_sensor() is
 * split into the two consecutive 8 bit registers the sensor uses.
 *
 * The sensor runs from a 24 MHz MCLK and four D-PHY lanes; the visible array
 * is 4032x3016 (full) and 2016x1508 (2x2 binned preview).  The vendor code
 * keeps a large amount of per-module calibration (SPC, PDAF, lens shading)
 * in OTP, which is not loaded here: the factory sequences include the
 * default SPC registers, which is what the vendor driver uses when no OTP
 * is present.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "mtk-sensor-legacy.h"

#define IMX386_NAME		"imx386"

#define IMX386_REG_CHIP_ID	0x0016	/* 16 bit, 0x0386 */
#define IMX386_CHIP_ID		0x0386
#define IMX386_REG_MODE_SELECT	0x0100
#define IMX386_MODE_STANDBY	0x00
#define IMX386_MODE_STREAMING	0x01
#define IMX386_REG_MIRROR	0x0101
#define IMX386_REG_GROUP_PARAM	0x0104
#define IMX386_REG_COARSE_EXP	0x0202
#define IMX386_REG_ANALOG_GAIN	0x0204
#define IMX386_REG_FRAME_LENGTH	0x0340
#define IMX386_REG_LINE_LENGTH	0x0342

#define IMX386_MCLK		24000000
#define IMX386_FPS		30

/*
 * Vendor gain2reg(): reg = 512 - 512 * 64 / gain, i.e. 16.5 dB per step,
 * with gain expressed in 1/64 units.  0 is the smallest gain the formula
 * can produce (gain = 64 -> 0), 448 is roughly 16x.
 */
#define IMX386_BASE_GAIN	64
#define IMX386_GAIN_MIN		IMX386_BASE_GAIN
#define IMX386_GAIN_MAX		1024

#define IMX386_MARGIN		10

/* Pre-programmed register tables, see the vendor functions named above */

static const struct cci_reg_sequence imx386_init_regs[] = {
#include "imx386_init_regs.inc"
};

static const struct cci_reg_sequence imx386_mode_2016x1508[] = {
#include "imx386_mode_2016x1508.inc"
};

static const struct cci_reg_sequence imx386_mode_4032x3016[] = {
#include "imx386_mode_4032x3016.inc"
};

static const struct mtk_sensor_mode imx386_modes[] = {
	{
		.width = 2016,
		.height = 1508,
		.line_length = 4296,
		.frame_length = 1780,
		.pclk = 233300000,
		.vts_min = 1780,
		.mipi_lanes = 4,
		.link_freq = 350000000,	/* 700 Mb/s per lane */
		.regs = imx386_mode_2016x1508,
		.num_regs = ARRAY_SIZE(imx386_mode_2016x1508),
	},
	{
		.width = 4032,
		.height = 3016,
		.line_length = 4296,
		.frame_length = 3300,
		.pclk = 433300000,
		.vts_min = 3300,
		.mipi_lanes = 4,
		.link_freq = 700000000,	/* 1400 Mb/s per lane */
		.regs = imx386_mode_4032x3016,
		.num_regs = ARRAY_SIZE(imx386_mode_4032x3016),
	},
};

static const char * const imx386_supply_names[] = {
	"avdd",		/* VCAMA, analog 2.8 V */
	"dovdd",	/* VCAMIO, I/O 1.8 V */
	"dvdd",		/* VCAMD, core 1.05 V */
};

/*
 * The vendor accessor write_cmos_sensor() sends a 16 bit address followed by
 * a 16 bit value; values above 0xff are the high byte of the first register.
 */
static void imx386_write16(struct regmap *map, u16 reg, u16 val, int *err)
{
	cci_write(map, SENSOR_REG(reg), (val >> 8) & 0xff, err);
	cci_write(map, SENSOR_REG(reg + 1), val & 0xff, err);
}

static int imx386_set_gain(struct mtk_sensor *sensor, u32 gain)
{
	u32 reg;

	/* Vendor gain2reg(), see the comment at IMX386_BASE_GAIN */
	gain = clamp_val(gain, IMX386_BASE_GAIN, IMX386_GAIN_MAX);
	reg = 512 - (512 * IMX386_BASE_GAIN / gain);

	return mtk_sensor_write16(sensor, IMX386_REG_ANALOG_GAIN,
				  min(reg, 0xffff));
}

static int imx386_set_exposure(struct mtk_sensor *sensor, u32 lines)
{
	int ret = 0;

	cci_write(sensor->regmap, SENSOR_REG(IMX386_REG_GROUP_PARAM), 0x01,
		  &ret);
	imx386_write16(sensor->regmap, IMX386_REG_COARSE_EXP, lines, &ret);
	cci_write(sensor->regmap, SENSOR_REG(IMX386_REG_GROUP_PARAM), 0x00,
		  &ret);

	return ret;
}

static int imx386_set_flip(struct mtk_sensor *sensor, bool hflip, bool vflip)
{
	u8 val = (hflip ? 0x01 : 0x00) | (vflip ? 0x02 : 0x00);

	/* 0x0101: bit 0 mirror, bit 1 vertical flip (vendor set_mirror_flip) */
	return cci_write(sensor->regmap, SENSOR_REG(IMX386_REG_MIRROR), val,
			 NULL);
}

static const struct mtk_sensor_ops imx386_ops = {
	.set_gain = imx386_set_gain,
	.set_exposure = imx386_set_exposure,
	.set_flip = imx386_set_flip,
	.write16 = imx386_write16,
};

static int imx386_probe(struct i2c_client *client)
{
	struct mtk_sensor *sensor;

	sensor = devm_kzalloc(&client->dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->name = IMX386_NAME;
	sensor->mbus_code = MEDIA_BUS_FMT_SRGGB10_1X10;
	sensor->modes = imx386_modes;
	sensor->num_modes = ARRAY_SIZE(imx386_modes);
	sensor->ops = &imx386_ops;
	sensor->supply_names = imx386_supply_names;
	sensor->num_supplies = ARRAY_SIZE(imx386_supply_names);
	sensor->xclk_rate = IMX386_MCLK;
	sensor->power_up_delay_ms = 20;
	sensor->mode_delay_us = 10000;
	sensor->init_regs = imx386_init_regs;
	sensor->num_init_regs = ARRAY_SIZE(imx386_init_regs);
	sensor->gain_min = IMX386_GAIN_MIN;
	sensor->gain_max = IMX386_GAIN_MAX;

	return mtk_sensor_register(sensor, client);
}

static void imx386_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
}

static const struct of_device_id imx386_of_match[] = {
	{ .compatible = "sony,imx386" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx386_of_match);

static struct i2c_driver imx386_driver = {
	.driver = {
		.name = "imx386",
		.of_match_table = imx386_of_match,
	},
	.probe = imx386_probe,
	.remove = imx386_remove,
};

module_i2c_driver(imx386_driver);

MODULE_DESCRIPTION("Sony IMX386 MIPI CSI-2 camera sensor driver");
MODULE_AUTHOR("Meizu fleet kernel port");
MODULE_LICENSE("GPL");
