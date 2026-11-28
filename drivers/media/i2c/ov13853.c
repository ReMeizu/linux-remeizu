// SPDX-License-Identifier: GPL-2.0-only
/*
 * OmniVision OV13853 (13 Mpix) MIPI CSI-2 Bayer sensor
 *
 * Copyright (C) 2026 Meizu fleet kernel port
 *
 * The rear camera of the Meizu M3 Note (MT6755) is an OV13853 module
 * ("ov13853_mipi_raw"; the qtech/ofilm/sunny2 module variants share the
 * same silicon and answer sensor ID 0xd853 on I2C address 0x10).  The
 * register sequences below are taken from the working vendor driver
 * (drivers/misc/mediatek/imgsensor/src/common/v1/ov13855_mipi_raw/
 *  ov13855mipiraw_Sensor.c): sensor_init(), preview_setting() and
 * capture_setting().  OV13853 and OV13855 are the same die with different
 * module colour filters, which is why the vendor tree ships the OV13853
 * board under the "ov13855" driver directory.
 *
 * The sensor is clocked by a 24 MHz MCLK and uses two D-PHY lanes.  Two
 * readout modes are described: the 2112x1568 2x2 binned preview and the
 * full 4224x3136 capture, both at 30 fps.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "mtk-sensor-legacy.h"

#define OV13853_NAME		"ov13853"

#define OV13853_REG_CHIP_ID	0x300a	/* 16 bit, 0xd853 */
#define OV13853_CHIP_ID		0xd853
#define OV13853_REG_MODE_SELECT	0x0100
#define OV13853_REG_GROUP_HOLD	0x3208
#define OV13853_REG_TIMING_CTRL	0x3820
#define OV13853_REG_HTS		0x380c
#define OV13853_REG_VTS		0x380e
#define OV13853_REG_EXPOSURE_HI	0x3500	/* 0x3500..0x3502: 20 bit */
#define OV13853_REG_ANALOG_GAIN	0x3508

#define OV13853_MCLK		24000000

/*
 * Vendor gain2reg(): register = gain * 128 / BASEGAIN with BASEGAIN = 64,
 * i.e. an OV13853 gain register of 0x80 is the minimum (1x) gain.  V4L2
 * gain is therefore expressed in 1/64 units, 64 = 1x.
 */
#define OV13853_BASE_GAIN	64
#define OV13853_GAIN_REG_BASE	0x80
#define OV13853_GAIN_REG_MAX	0x7c0	/* vendor clamp: 15.5x */
#define OV13853_GAIN_MIN	OV13853_BASE_GAIN
#define OV13853_GAIN_MAX	(OV13853_GAIN_REG_MAX * OV13853_BASE_GAIN / 128)

static const struct cci_reg_sequence ov13853_init_regs[] = {
#include "ov13853_init.inc"
};

static const struct cci_reg_sequence ov13853_mode_2112x1568[] = {
#include "ov13853_preview.inc"
};

static const struct cci_reg_sequence ov13853_mode_4224x3136[] = {
#include "ov13853_capture.inc"
};

static const struct mtk_sensor_mode ov13853_modes[] = {
	{
		.width = 2112,
		.height = 1568,
		.line_length = 2244,	/* 0x380c/0x380d, 2x2 binned */
		.frame_length = 1608,
		.pclk = 108250560,
		.vts_min = 1608,
		.mipi_lanes = 2,
		.link_freq = 338000000,	/* MIPI link frequency, see mtk_sensor_mode */
		.regs = ov13853_mode_2112x1568,
		.num_regs = ARRAY_SIZE(ov13853_mode_2112x1568),
	},
	{
		.width = 4224,
		.height = 3136,
		.line_length = 1122,
		.frame_length = 3214,
		.pclk = 108183240,
		.vts_min = 3214,
		.mipi_lanes = 2,
		.link_freq = 338000000,
		.regs = ov13853_mode_4224x3136,
		.num_regs = ARRAY_SIZE(ov13853_mode_4224x3136),
	},
};

static const char * const ov13853_supply_names[] = {
	"avdd",		/* VCAMA, analog 2.8 V */
	"dovdd",	/* VCAMIO, I/O 1.8 V */
	"dvdd",		/* VCAMD, core 1.2 V */
};

static int ov13853_set_gain(struct mtk_sensor *sensor, u32 gain)
{
	u32 reg;

	gain = clamp_val(gain, OV13853_GAIN_MIN, OV13853_GAIN_MAX);
	reg = DIV_ROUND_CLOSEST(gain * 128, OV13853_BASE_GAIN);
	reg = clamp_val(reg, OV13853_GAIN_REG_BASE, OV13853_GAIN_REG_MAX);

	return mtk_sensor_write16(sensor, OV13853_REG_ANALOG_GAIN, reg);
}

static int ov13853_set_exposure(struct mtk_sensor *sensor, u32 lines)
{
	int ret = 0;

	/* 0x3500/01/02 hold a 20 bit shutter; the low nibble is fractional */
	cci_write(sensor->regmap, SENSOR_REG(OV13853_REG_GROUP_HOLD), 0x00,
		  &ret);
	cci_write(sensor->regmap, SENSOR_REG(OV13853_REG_EXPOSURE_HI),
		  (lines >> 12) & 0x0f, &ret);
	cci_write(sensor->regmap, SENSOR_REG(OV13853_REG_EXPOSURE_HI + 1),
		  (lines >> 4) & 0xff, &ret);
	cci_write(sensor->regmap, SENSOR_REG(OV13853_REG_EXPOSURE_HI + 2),
		  (lines << 4) & 0xf0, &ret);
	cci_write(sensor->regmap, SENSOR_REG(OV13853_REG_GROUP_HOLD), 0xa0,
		  &ret);

	return ret;
}

static int ov13853_set_flip(struct mtk_sensor *sensor, bool hflip, bool vflip)
{
	/* 0x3820 bit 3 mirror, bits 5:4 flip, same layout as the OV5695 */
	u8 mask = BIT(3) | GENMASK(5, 4);
	u8 val = (hflip ? BIT(3) : 0) | (vflip ? GENMASK(5, 4) : 0);

	return cci_update_bits(sensor->regmap,
			       SENSOR_REG(OV13853_REG_TIMING_CTRL), mask, val,
			       NULL);
}

static const struct mtk_sensor_ops ov13853_ops = {
	.set_gain = ov13853_set_gain,
	.set_exposure = ov13853_set_exposure,
	.set_flip = ov13853_set_flip,
};

static int ov13853_probe(struct i2c_client *client)
{
	struct mtk_sensor *sensor;

	sensor = devm_kzalloc(&client->dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->name = OV13853_NAME;
	sensor->mbus_code = MEDIA_BUS_FMT_SBGGR10_1X10;
	sensor->modes = ov13853_modes;
	sensor->num_modes = ARRAY_SIZE(ov13853_modes);
	sensor->ops = &ov13853_ops;
	sensor->supply_names = ov13853_supply_names;
	sensor->num_supplies = ARRAY_SIZE(ov13853_supply_names);
	sensor->xclk_rate = OV13853_MCLK;
	sensor->power_up_delay_ms = 20;
	sensor->mode_delay_us = 20000;
	sensor->init_regs = ov13853_init_regs;
	sensor->num_init_regs = ARRAY_SIZE(ov13853_init_regs);
	sensor->id_reg = OV13853_REG_CHIP_ID;
	sensor->id_val = OV13853_CHIP_ID;
	sensor->gain_min = OV13853_GAIN_MIN;
	sensor->gain_max = OV13853_GAIN_MAX;

	return mtk_sensor_register(sensor, client);
}

static void ov13853_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
}

static const struct of_device_id ov13853_of_match[] = {
	{ .compatible = "ovti,ov13853" },
	{ }
};
MODULE_DEVICE_TABLE(of, ov13853_of_match);

static struct i2c_driver ov13853_driver = {
	.driver = {
		.name = "ov13853",
		.of_match_table = ov13853_of_match,
	},
	.probe = ov13853_probe,
	.remove = ov13853_remove,
};

module_i2c_driver(ov13853_driver);

MODULE_DESCRIPTION("OmniVision OV13853 MIPI CSI-2 camera sensor driver");
MODULE_AUTHOR("Meizu fleet kernel port");
MODULE_LICENSE("GPL");
