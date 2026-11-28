/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Common plumbing for the MediaTek legacy (MT6735/MT6755/MT6797) MIPI CSI-2
 * Bayer sensor drivers.
 *
 * Copyright (C) 2026 Meizu fleet kernel port
 *
 * The vendor kernels (3.18/4.9) wrap every sensor in the MTK "kd_imgsensor"
 * framework: the sensor driver only supplies register sequences plus a table
 * of per-mode timing constants, while power rails, MCLK and the V4L2 glue are
 * framework code.  These declarations and the glue in mtk-sensor-legacy.c
 * re-create that on top of the standard Linux sensor driver model, so the
 * individual sensor drivers only provide
 *
 *  - their mode table (resolution, line length, frame length, pixel clock),
 *  - the register sequences taken from the vendor sources, and
 *  - three small callbacks for gain, exposure and mirror/flip.
 */

#ifndef __MTK_SENSOR_LEGACY_H__
#define __MTK_SENSOR_LEGACY_H__

#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/media-entity.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

/* All MTK-era sensors use a 16 bit register address, 8 bit data. */
#define SENSOR_REG(a)		CCI_REG8(a)

#define MTK_SENSOR_MAX_WIDTH	8192
#define MTK_SENSOR_MAX_HEIGHT	8192
#define MTK_SENSOR_DEFAULT_FPS	30

struct mtk_sensor;

/**
 * struct mtk_sensor_mode - one native sensor readout mode
 * @width: output width in pixels
 * @height: output height in pixels
 * @line_length: horizontal total in pixel clocks (vendor linelength)
 * @frame_length: vertical total in lines at the default frame rate
 * @pclk: pixel clock in Hz (vendor pclk)
 * @vts_min: minimum vertical total, used as the VBLANK minimum
 * @mipi_lanes: number of D-PHY data lanes used by the mode
 * @link_freq: D-PHY link frequency in bps/2 (V4L2 convention)
 * @regs: register sequence programming the mode, stream-on excluded
 * @num_regs: number of entries in @regs
 */
struct mtk_sensor_mode {
	u32 width;
	u32 height;
	u32 line_length;
	u32 frame_length;
	u32 pclk;
	u32 vts_min;
	u8 mipi_lanes;
	u64 link_freq;
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct mtk_sensor_ops {
	/* Program analog gain, @gain is the raw register value */
	int (*set_gain)(struct mtk_sensor *sensor, u32 gain);
	/* Program exposure, @lines is the integration time in lines */
	int (*set_exposure)(struct mtk_sensor *sensor, u32 lines);
	/* Apply mirror/flip */
	int (*set_flip)(struct mtk_sensor *sensor, bool hflip, bool vflip);
	/*
	 * Optional: called before the mode sequence, e.g. to latch the
	 * group-parameter hold bit while a frame length change is pending.
	 */
	void (*prepare)(struct mtk_sensor *sensor,
			const struct mtk_sensor_mode *mode);
	/*
	 * Optional: write the two halves of a 16 bit value to a 16 bit
	 * register.  The byte order differs between manufacturers (Samsung
	 * writes the low byte first, Sony the high byte first).
	 */
	void (*write16)(struct regmap *map, u16 reg, u16 val, int *err);
};

/**
 * struct mtk_sensor - one sensor instance
 * @dev: device backing the sensor (the I2C client device)
 * @sd: V4L2 sub-device exported to the CSI-2 receiver
 * @pads: single source pad
 * @lock: protects @cur_mode, @streaming and the control values
 * @regmap: CCI register map
 * @ops: manufacturer hooks
 * @modes: mode table
 * @num_modes: entries in @modes
 * @cur_mode: active mode, NULL until the first set_fmt
 * @name: sensor name for the sub-device
 * @mbus_code: media bus format produced by the sensor
 * @supply_data: regulator bulk data template (for probe)
 * @supply_names: supply names from the driver
 * @num_supplies: number of supplies
 * @supplies: resolved regulators
 * @xclk: MCLK
 * @xclk_rate: MCLK rate to program, 0 to leave alone
 * @reset_gpio: optional reset GPIO, active high by convention
 * @power_gpio: optional enable/powerdown GPIO
 * @power_up_delay_ms: settle time after power-on before I2C access
 * @mode_delay_us: settle time between mode write and stream-on
 * @init_regs: optional sequence run once after power-on
 * @num_init_regs: entries in @init_regs
 * @id_reg: optional sensor ID register
 * @id_val: expected value of @id_reg
 * @gain_min: minimum V4L2 analogue gain, in raw register units
 * @gain_max: maximum V4L2 analogue gain
 * @streaming: true while the sensor outputs frames
 * @ctrls: control handler
 * @link_freq_ctrl: V4L2_CID_LINK_FREQ
 * @pixel_rate_ctrl: V4L2_CID_PIXEL_RATE
 * @vblank_ctrl: V4L2_CID_VBLANK
 * @hblank_ctrl: V4L2_CID_HBLANK
 * @exposure_ctrl: V4L2_CID_EXPOSURE
 * @analog_gain_ctrl: V4L2_CID_ANALOGUE_GAIN
 * @hflip_ctrl: V4L2_CID_HFLIP
 * @vflip_ctrl: V4L2_CID_VFLIP
 */
struct mtk_sensor {
	struct device *dev;

	struct v4l2_subdev sd;
	struct media_pad pads[1];
	struct mutex lock;

	struct regmap *regmap;
	const struct mtk_sensor_ops *ops;

	const struct mtk_sensor_mode *modes;
	unsigned int num_modes;
	const struct mtk_sensor_mode *cur_mode;

	const char *name;
	u32 mbus_code;

	const char *const *supply_names;
	unsigned int num_supplies;
	struct regulator_bulk_data *supply_data;
	struct regulator_bulk_data *supplies;
	struct clk *xclk;
	unsigned long xclk_rate;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *power_gpio;

	unsigned int power_up_delay_ms;
	unsigned int mode_delay_us;

	const struct cci_reg_sequence *init_regs;
	unsigned int num_init_regs;
	u32 id_reg;
	u64 id_val;
	u32 gain_min;
	u32 gain_max;

	bool streaming;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *link_freq_ctrl;
	struct v4l2_ctrl *pixel_rate_ctrl;
	struct v4l2_ctrl *vblank_ctrl;
	struct v4l2_ctrl *hblank_ctrl;
	struct v4l2_ctrl *exposure_ctrl;
	struct v4l2_ctrl *analog_gain_ctrl;
	struct v4l2_ctrl *hflip_ctrl;
	struct v4l2_ctrl *vflip_ctrl;
};

int mtk_sensor_register(struct mtk_sensor *sensor, struct i2c_client *client);
void mtk_sensor_unregister(struct i2c_client *client);
int mtk_sensor_write16(struct mtk_sensor *sensor, u16 reg, u16 val);

static inline struct mtk_sensor *to_mtk_sensor(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mtk_sensor, sd);
}

#endif /* __MTK_SENSOR_LEGACY_H__ */
