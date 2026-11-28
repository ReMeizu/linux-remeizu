// SPDX-License-Identifier: GPL-2.0-only
/*
 * OmniVision OV5695 (5 Mpix) MIPI CSI-2 Bayer sensor
 *
 * Copyright (C) 2026 Meizu fleet kernel port
 *
 * The OV5695 is the front camera of the Meizu MX6 (MT6797) and of the Meizu
 * M3 Note (MT6755).  The vendor trees carry their own tuned register
 * sequences, mostly differing in the MCLK/PLL setup: the MX6 drives a
 * 125 MHz MCLK, the M3 Note the shared 24 MHz CSI MCLK.  The driver keeps
 * both sets and picks the one matching the "xclk" clock it finds in the
 * device tree, defaulting to the MX6 tables.
 *
 * Register data: drivers/misc/mediatek/imgsensor/src/mt6797/ov5695_mipi_raw/
 * and .../mt6755/ov5695_mipi_raw/, functions sensor_init(),
 * preview_setting() and capture_setting().
 */

#include <linux/clk.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "mtk-sensor-legacy.h"

#define OV5695_NAME		"ov5695"

#define OV5695_REG_MODE_SELECT	0x0100
#define OV5695_REG_GROUP_HOLD	0x3208
#define OV5695_REG_FRAME_LENGTH	0x380e
#define OV5695_REG_ANALOG_GAIN	0x3508
#define OV5695_REG_EXPOSURE	0x3501
#define OV5695_REG_TIMING_CTRL	0x3820

#define OV5695_MCLK_MX6		125000000
#define OV5695_MCLK_STD		24000000

/*
 * Vendor gain2reg(): register = gain * 16 / 64, clamped to 0x10..0xf8, so
 * V4L2 gain is expressed in 1/64 units of the sensor's minimum gain.
 */
#define OV5695_BASE_GAIN	64
#define OV5695_GAIN_MIN		16
#define OV5695_GAIN_MAX		248

/* MX6 (MT6797) tables: 2 lanes, 840 Mb/s per lane, 125 MHz MCLK */

static const struct cci_reg_sequence ov5695_mx6_init_regs[] = {
#include "ov5695_mx6_init.inc"
};

static const struct cci_reg_sequence ov5695_mx6_mode_1296x972[] = {
#include "ov5695_mx6_preview.inc"
};

static const struct cci_reg_sequence ov5695_mx6_mode_2592x1944[] = {
#include "ov5695_mx6_capture.inc"
};

/* M3 Note (MT6755) tables: 2 lanes, 840 Mb/s per lane, 24 MHz MCLK */

static const struct cci_reg_sequence ov5695_m3note_init_regs[] = {
#include "ov5695_m3note_init.inc"
};

static const struct cci_reg_sequence ov5695_m3note_mode_1296x972[] = {
#include "ov5695_m3note_preview.inc"
};

static const struct cci_reg_sequence ov5695_m3note_mode_2592x1944[] = {
#include "ov5695_m3note_capture.inc"
};

struct ov5695_variant {
	const struct mtk_sensor_mode *modes;
	const struct cci_reg_sequence *init_regs;
	unsigned int num_init_regs;
	unsigned long mclk;
	unsigned int power_up_delay_ms;
};

static const struct mtk_sensor_mode ov5695_mx6_modes[] = {
	{
		.width = 1296,
		.height = 972,
		.line_length = 672,
		.frame_length = 2232,
		.pclk = 45000000,
		.vts_min = 2232,
		.mipi_lanes = 2,
		.link_freq = 420000000,
		.regs = ov5695_mx6_mode_1296x972,
		.num_regs = ARRAY_SIZE(ov5695_mx6_mode_1296x972),
	},
	{
		.width = 2592,
		.height = 1944,
		.line_length = 740,
		.frame_length = 4048,
		.pclk = 45000000,
		.vts_min = 4048,
		.mipi_lanes = 2,
		.link_freq = 420000000,
		.regs = ov5695_mx6_mode_2592x1944,
		.num_regs = ARRAY_SIZE(ov5695_mx6_mode_2592x1944),
	},
};

static const struct mtk_sensor_mode ov5695_m3note_modes[] = {
	{
		.width = 1296,
		.height = 972,
		.line_length = 672,
		.frame_length = 2232,
		.pclk = 45000000,
		.vts_min = 2232,
		.mipi_lanes = 2,
		.link_freq = 420000000,
		.regs = ov5695_m3note_mode_1296x972,
		.num_regs = ARRAY_SIZE(ov5695_m3note_mode_1296x972),
	},
	{
		.width = 2592,
		.height = 1944,
		.line_length = 740,
		.frame_length = 4048,
		.pclk = 45000000,
		.vts_min = 4048,
		.mipi_lanes = 2,
		.link_freq = 420000000,
		.regs = ov5695_m3note_mode_2592x1944,
		.num_regs = ARRAY_SIZE(ov5695_m3note_mode_2592x1944),
	},
};

static const struct ov5695_variant ov5695_mx6 = {
	.modes = ov5695_mx6_modes,
	.init_regs = ov5695_mx6_init_regs,
	.num_init_regs = ARRAY_SIZE(ov5695_mx6_init_regs),
	.mclk = OV5695_MCLK_MX6,
	.power_up_delay_ms = 20,
};

static const struct ov5695_variant ov5695_m3note = {
	.modes = ov5695_m3note_modes,
	.init_regs = ov5695_m3note_init_regs,
	.num_init_regs = ARRAY_SIZE(ov5695_m3note_init_regs),
	.mclk = OV5695_MCLK_STD,
	.power_up_delay_ms = 10,
};

static const char * const ov5695_supply_names[] = {
	"avdd",		/* VCAMA, analog 2.8 V */
	"dovdd",	/* VCAMIO, I/O 1.8 V */
	"dvdd",		/* VCAMD, core 1.2 V */
};

static int ov5695_set_gain(struct mtk_sensor *sensor, u32 gain)
{
	u16 reg = clamp_val(DIV_ROUND_CLOSEST(gain * 16, OV5695_BASE_GAIN),
			    0x10, 0xf8);

	return mtk_sensor_write16(sensor, OV5695_REG_ANALOG_GAIN, reg);
}

static int ov5695_set_exposure(struct mtk_sensor *sensor, u32 lines)
{
	int ret = 0;

	/* 0x3500/01/02 hold a 20 bit value, only the upper 16 bits are used */
	cci_write(sensor->regmap, SENSOR_REG(OV5695_REG_GROUP_HOLD), 0x00,
		  &ret);
	mtk_sensor_write16(sensor, OV5695_REG_EXPOSURE, lines);
	cci_write(sensor->regmap, SENSOR_REG(OV5695_REG_GROUP_HOLD), 0xa0,
		  &ret);

	return ret;
}

static int ov5695_set_flip(struct mtk_sensor *sensor, bool hflip, bool vflip)
{
	u8 mask = BIT(3) | GENMASK(5, 4);	/* 0x3820 bit 3 mirror, 5:4 flip */
	u8 val = (hflip ? BIT(3) : 0) | (vflip ? GENMASK(5, 4) : 0);

	return cci_update_bits(sensor->regmap,
			       SENSOR_REG(OV5695_REG_TIMING_CTRL), mask, val,
			       NULL);
}

static const struct mtk_sensor_ops ov5695_ops = {
	.set_gain = ov5695_set_gain,
	.set_exposure = ov5695_set_exposure,
	.set_flip = ov5695_set_flip,
};

static int ov5695_probe(struct i2c_client *client)
{
	const struct ov5695_variant *variant = &ov5695_mx6;
	struct clk *xclk;
	u32 rate = 0;
	struct mtk_sensor *sensor;

	/*
	 * The two vendor tables are not interchangeable: they differ in the
	 * MCLK/PLL programming, so pick the one that belongs to the clock the
	 * board provides.  The board describes its sensor clock with
	 * "clock-frequency" on the sensor node.
	 */
	xclk = devm_clk_get_optional(&client->dev, "xclk");
	if (!IS_ERR_OR_NULL(xclk) &&
	    !of_property_read_u32(client->dev.of_node, "clock-frequency",
				  &rate) && rate &&
	    rate < OV5695_MCLK_MX6 / 2)
		variant = &ov5695_m3note;

	sensor = devm_kzalloc(&client->dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->name = OV5695_NAME;
	sensor->mbus_code = MEDIA_BUS_FMT_SBGGR10_1X10;
	sensor->modes = variant->modes;
	sensor->num_modes = 2;
	sensor->ops = &ov5695_ops;
	sensor->supply_names = ov5695_supply_names;
	sensor->num_supplies = ARRAY_SIZE(ov5695_supply_names);
	sensor->xclk_rate = variant->mclk;
	sensor->power_up_delay_ms = variant->power_up_delay_ms;
	sensor->mode_delay_us = 5000;
	sensor->init_regs = variant->init_regs;
	sensor->num_init_regs = variant->num_init_regs;
	sensor->gain_min = OV5695_GAIN_MIN;
	sensor->gain_max = OV5695_GAIN_MAX;

	return mtk_sensor_register(sensor, client);
}

static void ov5695_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
}

static const struct of_device_id ov5695_of_match[] = {
	{ .compatible = "ovti,ov5695" },
	{ }
};
MODULE_DEVICE_TABLE(of, ov5695_of_match);

static struct i2c_driver ov5695_driver = {
	.driver = {
		.name = "ov5695",
		.of_match_table = ov5695_of_match,
	},
	.probe = ov5695_probe,
	.remove = ov5695_remove,
};

module_i2c_driver(ov5695_driver);

MODULE_DESCRIPTION("OmniVision OV5695 MIPI CSI-2 camera sensor driver");
MODULE_AUTHOR("Meizu fleet kernel port");
MODULE_LICENSE("GPL");
