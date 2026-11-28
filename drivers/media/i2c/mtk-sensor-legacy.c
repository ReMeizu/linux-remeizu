// SPDX-License-Identifier: GPL-2.0-only
/*
 * Common V4L2 sub-device implementation for MediaTek legacy Bayer sensors.
 *
 * Copyright (C) 2026 Meizu fleet kernel port
 *
 * Every sensor driver for the MT6735/MT6755/MT6797 camera stack describes the
 * same things: a handful of power rails, one MCLK, an optional reset GPIO, a
 * table of readout modes with their MIPI timing and the register sequences
 * that program them.  This file implements the V4L2 sub-device side of that
 * contract so the per-sensor files stay close to the vendor sources they are
 * derived from.
 *
 * Streaming: the modes are programmed lazily.  When the first stream user
 * calls .s_stream(1) the mode table of the currently selected format is
 * written, followed by gain/exposure/flip, and the sensor is put into
 * streaming state.  The CSI-2 receiver then sees the D-PHY high speed clock.
 */

#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "mtk-sensor-legacy.h"

#define MTK_SENSOR_REG_STREAM	0x0100
#define MTK_SENSOR_STREAM_OFF	0
#define MTK_SENSOR_STREAM_ON	1

static const struct mtk_sensor_mode *
mtk_sensor_active_mode(struct mtk_sensor *sensor)
{
	if (sensor->cur_mode)
		return sensor->cur_mode;

	return &sensor->modes[0];
}

/* --------------------------------------------------------------------------
 * Power sequencing
 * ------------------------------------------------------------------------ */

static void mtk_sensor_power_off(struct mtk_sensor *sensor)
{
	unsigned int i;

	if (sensor->reset_gpio)
		gpiod_set_value_cansleep(sensor->reset_gpio, 1);

	if (sensor->power_gpio)
		gpiod_set_value_cansleep(sensor->power_gpio, 0);

	if (!IS_ERR_OR_NULL(sensor->xclk))
		clk_disable_unprepare(sensor->xclk);

	for (i = 0; i < sensor->num_supplies; i++)
		regulator_disable(sensor->supplies[i].consumer);
}

static int mtk_sensor_power_on(struct mtk_sensor *sensor)
{
	unsigned int i;
	int ret;

	for (i = 0; i < sensor->num_supplies; i++) {
		ret = regulator_enable(sensor->supplies[i].consumer);
		if (ret) {
			dev_err(sensor->dev, "failed to enable %s: %d\n",
				sensor->supplies[i].supply, ret);
			goto err_disable;
		}
	}
	usleep_range(500, 1000);

	if (!IS_ERR_OR_NULL(sensor->xclk)) {
		ret = clk_prepare_enable(sensor->xclk);
		if (ret) {
			dev_err(sensor->dev, "failed to enable MCLK: %d\n",
				ret);
			goto err_disable;
		}
	}

	if (sensor->power_gpio)
		gpiod_set_value_cansleep(sensor->power_gpio, 1);

	if (sensor->reset_gpio) {
		gpiod_set_value_cansleep(sensor->reset_gpio, 1);
		usleep_range(1000, 1500);
		gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	}

	/* Vendor boards need a few milliseconds before the I2C slave answers */
	msleep(sensor->power_up_delay_ms);

	return 0;

err_disable:
	while (i--)
		regulator_disable(sensor->supplies[i].consumer);

	return ret;
}

/* --------------------------------------------------------------------------
 * Register helpers
 * ------------------------------------------------------------------------ */

int mtk_sensor_write16(struct mtk_sensor *sensor, u16 reg, u16 val)
{
	int err = 0;

	if (sensor->ops->write16) {
		sensor->ops->write16(sensor->regmap, reg, val, &err);
		return err;
	}

	cci_write(sensor->regmap, SENSOR_REG(reg), val >> 8, &err);
	cci_write(sensor->regmap, SENSOR_REG(reg + 1), val & 0xff, &err);

	return err;
}
EXPORT_SYMBOL_GPL(mtk_sensor_write16);

static int mtk_sensor_write_array(struct mtk_sensor *sensor,
				  const struct cci_reg_sequence *regs,
				  unsigned int num_regs)
{
	return cci_multi_reg_write(sensor->regmap, regs, num_regs, NULL);
}

/* --------------------------------------------------------------------------
 * Controls
 * ------------------------------------------------------------------------ */

static int mtk_sensor_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mtk_sensor *sensor = container_of(ctrl->handler,
						 struct mtk_sensor, ctrls);
	const struct mtk_sensor_mode *mode = mtk_sensor_active_mode(sensor);
	u32 vblank, exposure_max;
	int ret = 0;

	guard(mutex)(&sensor->lock);

	/*
	 * Blanks and gain/exposure are always kept in software; only the
	 * sensor state matters once it produces frames, and the streaming
	 * start path applies them anyway.
	 */
	if (!sensor->streaming)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = sensor->ops->set_gain(sensor, ctrl->val);
		break;
	case V4L2_CID_EXPOSURE:
		ret = sensor->ops->set_exposure(sensor, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = sensor->ops->set_flip(sensor, sensor->hflip_ctrl->val,
					    sensor->vflip_ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		vblank = ctrl->val + mode->height;
		exposure_max = vblank > 2 ? vblank - 2 : 1;
		__v4l2_ctrl_modify_range(sensor->exposure_ctrl,
					 sensor->exposure_ctrl->minimum,
					 exposure_max,
					 sensor->exposure_ctrl->step,
					 min_t(u32,
					       sensor->exposure_ctrl->default_value,
					       exposure_max));
		break;
	default:
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops mtk_sensor_ctrl_ops = {
	.s_ctrl = mtk_sensor_set_ctrl,
};

static u64 mtk_sensor_max_link_freq(struct mtk_sensor *sensor)
{
	u64 max = 0;
	unsigned int i;

	for (i = 0; i < sensor->num_modes; i++)
		max = max(max, (u64)sensor->modes[i].link_freq);

	return max;
}

static int mtk_sensor_init_controls(struct mtk_sensor *sensor)
{
	const struct mtk_sensor_mode *mode = &sensor->modes[0];
	s64 link_freqs[2];
	unsigned int hblank;
	int ret;

	/*
	 * MTK sensors only describe one link frequency: derive it from the
	 * pixel clock and the number of lanes of the fastest mode.
	 */
	link_freqs[0] = 0;
	link_freqs[1] = mtk_sensor_max_link_freq(sensor);

	v4l2_ctrl_handler_init(&sensor->ctrls, 10);

	sensor->link_freq_ctrl =
		v4l2_ctrl_new_int_menu(&sensor->ctrls, &mtk_sensor_ctrl_ops,
				       V4L2_CID_LINK_FREQ, ARRAY_SIZE(link_freqs) - 1,
				       0, link_freqs);
	if (sensor->link_freq_ctrl)
		sensor->link_freq_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->pixel_rate_ctrl =
		v4l2_ctrl_new_std(&sensor->ctrls, &mtk_sensor_ctrl_ops,
				  V4L2_CID_PIXEL_RATE, 1, INT_MAX, 1,
				  mode->pclk);
	if (sensor->pixel_rate_ctrl)
		sensor->pixel_rate_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->exposure_ctrl =
		v4l2_ctrl_new_std(&sensor->ctrls, &mtk_sensor_ctrl_ops,
				  V4L2_CID_EXPOSURE, 1, mode->frame_length - 2,
				  1, mode->frame_length / 2);

	sensor->analog_gain_ctrl =
		v4l2_ctrl_new_std(&sensor->ctrls, &mtk_sensor_ctrl_ops,
				  V4L2_CID_ANALOGUE_GAIN, sensor->gain_min,
				  sensor->gain_max, 1, sensor->gain_min);

	hblank = mode->line_length > mode->width ?
		 mode->line_length - mode->width : 0;
	sensor->hblank_ctrl =
		v4l2_ctrl_new_std(&sensor->ctrls, &mtk_sensor_ctrl_ops,
				  V4L2_CID_HBLANK, hblank, hblank, 1, hblank);
	if (sensor->hblank_ctrl)
		sensor->hblank_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->vblank_ctrl =
		v4l2_ctrl_new_std(&sensor->ctrls, &mtk_sensor_ctrl_ops,
				  V4L2_CID_VBLANK,
				  mode->vts_min - mode->height,
				  MTK_SENSOR_MAX_HEIGHT - mode->height, 1,
				  mode->frame_length - mode->height);

	sensor->hflip_ctrl =
		v4l2_ctrl_new_std(&sensor->ctrls, &mtk_sensor_ctrl_ops,
				  V4L2_CID_HFLIP, 0, 1, 1, 0);
	sensor->vflip_ctrl =
		v4l2_ctrl_new_std(&sensor->ctrls, &mtk_sensor_ctrl_ops,
				  V4L2_CID_VFLIP, 0, 1, 1, 0);

	if (sensor->ctrls.error) {
		ret = sensor->ctrls.error;
		dev_err(sensor->dev, "failed to create controls: %d\n", ret);
		v4l2_ctrl_handler_free(&sensor->ctrls);
		return ret;
	}

	sensor->sd.ctrl_handler = &sensor->ctrls;

	return 0;
}

/* --------------------------------------------------------------------------
 * V4L2 sub-device operations
 * ------------------------------------------------------------------------ */

static void mtk_sensor_fill_format(struct mtk_sensor *sensor,
				   const struct mtk_sensor_mode *mode,
				   struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = sensor->mbus_code;
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static const struct mtk_sensor_mode *
mtk_sensor_find_mode(struct mtk_sensor *sensor, u32 width, u32 height)
{
	const struct mtk_sensor_mode *best = &sensor->modes[0];
	u64 best_delta = U64_MAX;
	unsigned int i;

	for (i = 0; i < sensor->num_modes; i++) {
		const struct mtk_sensor_mode *m = &sensor->modes[i];
		u64 delta = abs_diff(m->width, width) +
			    abs_diff(m->height, height);

		if (delta < best_delta) {
			best = m;
			best_delta = delta;
		}
	}

	return best;
}

static void mtk_sensor_update_limits(struct mtk_sensor *sensor,
				     const struct mtk_sensor_mode *mode)
{
	u32 hblank = mode->line_length > mode->width ?
		     mode->line_length - mode->width : 0;

	__v4l2_ctrl_modify_range(sensor->pixel_rate_ctrl, 1, INT_MAX, 1,
				 mode->pclk);
	__v4l2_ctrl_s_ctrl(sensor->pixel_rate_ctrl, mode->pclk);

	__v4l2_ctrl_modify_range(sensor->hblank_ctrl, hblank, hblank, 1,
				 hblank);
	__v4l2_ctrl_s_ctrl(sensor->hblank_ctrl, hblank);

	__v4l2_ctrl_modify_range(sensor->vblank_ctrl,
				 mode->vts_min - mode->height,
				 MTK_SENSOR_MAX_HEIGHT - mode->height, 1,
				 mode->frame_length - mode->height);
	__v4l2_ctrl_s_ctrl(sensor->vblank_ctrl,
			   mode->frame_length - mode->height);

	__v4l2_ctrl_modify_range(sensor->exposure_ctrl, 1,
				 mode->frame_length - 2, 1,
				 mode->frame_length / 2);
	__v4l2_ctrl_s_ctrl(sensor->exposure_ctrl, mode->frame_length / 2);
}

static int mtk_sensor_enum_mbus_code(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_mbus_code_enum *code)
{
	struct mtk_sensor *sensor = to_mtk_sensor(sd);

	if (code->index)
		return -EINVAL;

	code->code = sensor->mbus_code;

	return 0;
}

static int mtk_sensor_enum_frame_size(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_frame_size_enum *fse)
{
	struct mtk_sensor *sensor = to_mtk_sensor(sd);

	if (fse->index >= sensor->num_modes)
		return -EINVAL;

	fse->code = sensor->mbus_code;
	fse->min_width = fse->max_width = sensor->modes[fse->index].width;
	fse->min_height = fse->max_height = sensor->modes[fse->index].height;

	return 0;
}

static int mtk_sensor_get_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *fmt)
{
	if (fmt->pad)
		return -EINVAL;

	*fmt = (struct v4l2_subdev_format) {
		.which = fmt->which,
		.pad = fmt->pad,
		.format = *v4l2_subdev_state_get_format(state, fmt->pad),
	};

	return 0;
}

static int mtk_sensor_set_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *fmt)
{
	struct mtk_sensor *sensor = to_mtk_sensor(sd);
	struct v4l2_mbus_framefmt *format;
	const struct mtk_sensor_mode *mode;

	if (fmt->pad)
		return -EINVAL;

	mode = mtk_sensor_find_mode(sensor, fmt->format.width,
				    fmt->format.height);
	mtk_sensor_fill_format(sensor, mode, &fmt->format);

	format = v4l2_subdev_state_get_format(state, fmt->pad);
	*format = fmt->format;

	if (fmt->which != V4L2_SUBDEV_FORMAT_ACTIVE)
		return 0;

	guard(mutex)(&sensor->lock);

	sensor->cur_mode = mode;
	mtk_sensor_update_limits(sensor, mode);

	return 0;
}

static int mtk_sensor_init_state(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state)
{
	struct mtk_sensor *sensor = to_mtk_sensor(sd);
	struct v4l2_mbus_framefmt *fmt;

	fmt = v4l2_subdev_state_get_format(state, 0);
	mtk_sensor_fill_format(sensor, &sensor->modes[0], fmt);

	return 0;
}

static int mtk_sensor_get_mbus_config(struct v4l2_subdev *sd,
				      unsigned int pad,
				      struct v4l2_mbus_config *cfg)
{
	struct mtk_sensor *sensor = to_mtk_sensor(sd);

	cfg->type = V4L2_MBUS_CSI2_DPHY;
	cfg->bus.mipi_csi2.num_data_lanes =
		mtk_sensor_active_mode(sensor)->mipi_lanes;

	return 0;
}

static int mtk_sensor_start_streaming(struct mtk_sensor *sensor)
{
	const struct mtk_sensor_mode *mode = mtk_sensor_active_mode(sensor);
	int ret;

	if (sensor->ops->prepare)
		sensor->ops->prepare(sensor, mode);

	ret = mtk_sensor_write_array(sensor, mode->regs, mode->num_regs);
	if (ret) {
		dev_err(sensor->dev, "failed to write %ux%u mode: %d\n",
			mode->width, mode->height, ret);
		return ret;
	}

	ret = sensor->ops->set_flip(sensor, sensor->hflip_ctrl->val,
				    sensor->vflip_ctrl->val);
	if (ret)
		return ret;

	ret = sensor->ops->set_exposure(sensor, sensor->exposure_ctrl->val);
	if (ret)
		return ret;

	ret = sensor->ops->set_gain(sensor, sensor->analog_gain_ctrl->val);
	if (ret)
		return ret;

	usleep_range(sensor->mode_delay_us, sensor->mode_delay_us + 1000);

	ret = cci_write(sensor->regmap, SENSOR_REG(MTK_SENSOR_REG_STREAM),
			MTK_SENSOR_STREAM_ON, NULL);
	if (ret)
		return ret;

	sensor->streaming = true;

	return 0;
}

static int mtk_sensor_stop_streaming(struct mtk_sensor *sensor)
{
	sensor->streaming = false;

	return cci_write(sensor->regmap, SENSOR_REG(MTK_SENSOR_REG_STREAM),
			 MTK_SENSOR_STREAM_OFF, NULL);
}

static int mtk_sensor_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct mtk_sensor *sensor = to_mtk_sensor(sd);

	guard(mutex)(&sensor->lock);

	if (enable && sensor->streaming)
		return 0;

	if (!enable && !sensor->streaming)
		return 0;

	if (enable)
		return mtk_sensor_start_streaming(sensor);

	return mtk_sensor_stop_streaming(sensor);
}

static const struct v4l2_subdev_pad_ops mtk_sensor_pad_ops = {
	.enum_mbus_code = mtk_sensor_enum_mbus_code,
	.enum_frame_size = mtk_sensor_enum_frame_size,
	.get_fmt = mtk_sensor_get_fmt,
	.set_fmt = mtk_sensor_set_fmt,
	.get_mbus_config = mtk_sensor_get_mbus_config,
};

static const struct v4l2_subdev_video_ops mtk_sensor_video_ops = {
	.s_stream = mtk_sensor_s_stream,
};

static const struct v4l2_subdev_ops mtk_sensor_subdev_ops = {
	.pad = &mtk_sensor_pad_ops,
	.video = &mtk_sensor_video_ops,
};

static const struct v4l2_subdev_internal_ops mtk_sensor_internal_ops = {
	.init_state = mtk_sensor_init_state,
};

/* --------------------------------------------------------------------------
 * Probe / remove
 * ------------------------------------------------------------------------ */

static int mtk_sensor_acquire_resources(struct mtk_sensor *sensor)
{
	struct device *dev = sensor->dev;
	int ret;

	if (sensor->num_supplies) {
		ret = devm_regulator_bulk_get_const(dev, sensor->num_supplies,
						    sensor->supply_data,
						    &sensor->supplies);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to get regulators\n");
	}

	sensor->xclk = devm_clk_get_optional(dev, "xclk");
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk),
				     "failed to get MCLK\n");

	if (!IS_ERR_OR_NULL(sensor->xclk) && sensor->xclk_rate) {
		ret = clk_set_rate(sensor->xclk, sensor->xclk_rate);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to set MCLK rate\n");
	}

	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
				     "failed to get reset GPIO\n");

	sensor->power_gpio = devm_gpiod_get_optional(dev, "powerdown",
						     GPIOD_OUT_LOW);
	if (IS_ERR(sensor->power_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->power_gpio),
				     "failed to get powerdown GPIO\n");

	return 0;
}

static int mtk_sensor_init_regmap(struct mtk_sensor *sensor,
				  struct i2c_client *client)
{
	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(sensor->dev, PTR_ERR(sensor->regmap),
				     "failed to init register map\n");

	return 0;
}

static int mtk_sensor_identify(struct mtk_sensor *sensor)
{
	u32 reg;
	u64 id;
	int ret;

	if (!sensor->id_val)
		return 0;

	reg = sensor->id_val > 0xff ? CCI_REG16(sensor->id_reg) :
				      SENSOR_REG(sensor->id_reg);
	ret = cci_read(sensor->regmap, reg, &id, NULL);
	if (ret) {
		dev_err(sensor->dev, "failed to read ID: %d\n", ret);
		return ret;
	}

	if (id != sensor->id_val) {
		dev_err(sensor->dev, "unsupported sensor ID 0x%llx\n", id);
		return -ENODEV;
	}

	dev_dbg(sensor->dev, "sensor ID 0x%llx\n", id);

	return 0;
}

static int mtk_sensor_probe(struct mtk_sensor *sensor,
			    struct i2c_client *client)
{
	int ret;

	ret = mtk_sensor_acquire_resources(sensor);
	if (ret)
		return ret;

	ret = mtk_sensor_init_regmap(sensor, client);
	if (ret)
		return ret;

	ret = mtk_sensor_power_on(sensor);
	if (ret)
		return ret;

	if (sensor->num_init_regs)
		ret = mtk_sensor_write_array(sensor, sensor->init_regs,
					     sensor->num_init_regs);
	if (!ret)
		ret = mtk_sensor_identify(sensor);

	if (ret) {
		mtk_sensor_power_off(sensor);
		return ret;
	}

	return 0;
}

/**
 * mtk_sensor_register - register a MTK legacy sensor as a V4L2 sub-device
 * @sensor: sensor instance, calloc'ed by the caller (usually NULL-able data
 *	    is zero), with the static description already filled in
 * @client: I2C client the sensor is bound to
 *
 * Powers the sensor up, programs its init sequence, optionally verifies the
 * sensor ID and registers the sub-device together with its control set.
 *
 * Returns 0 on success or a negative error code.
 */
int mtk_sensor_register(struct mtk_sensor *sensor, struct i2c_client *client)
{
	struct device *dev = &client->dev;
	int ret;

	sensor->dev = dev;

	if (sensor->num_supplies) {
		sensor->supply_data =
			devm_kcalloc(dev, sensor->num_supplies,
				     sizeof(*sensor->supply_data), GFP_KERNEL);
		if (!sensor->supply_data)
			return -ENOMEM;

		for (unsigned int i = 0; i < sensor->num_supplies; i++)
			sensor->supply_data[i].supply = sensor->supply_names[i];
	}

	mutex_init(&sensor->lock);

	ret = mtk_sensor_probe(sensor, client);
	if (ret)
		return ret;

	v4l2_i2c_subdev_init(&sensor->sd, client, &mtk_sensor_subdev_ops);
	sensor->sd.internal_ops = &mtk_sensor_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pads[0].flags = MEDIA_PAD_FL_SOURCE;

	snprintf(sensor->sd.name, sizeof(sensor->sd.name), "%s",
		 sensor->name);

	ret = media_entity_pads_init(&sensor->sd.entity, 1, sensor->pads);
	if (ret)
		goto err_power;

	ret = mtk_sensor_init_controls(sensor);
	if (ret)
		goto err_entity;

	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto err_ctrls;

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto err_subdev;

	dev_info(dev, "%s (%s) registered, default %ux%u@%ufps\n",
		 sensor->name, dev_name(dev), sensor->modes[0].width,
		 sensor->modes[0].height, MTK_SENSOR_DEFAULT_FPS);

	return 0;

err_subdev:
	v4l2_subdev_cleanup(&sensor->sd);
err_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrls);
err_entity:
	media_entity_cleanup(&sensor->sd.entity);
err_power:
	mtk_sensor_power_off(sensor);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_sensor_register);

void mtk_sensor_unregister(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mtk_sensor *sensor = to_mtk_sensor(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrls);
	mtk_sensor_power_off(sensor);
}
EXPORT_SYMBOL_GPL(mtk_sensor_unregister);
