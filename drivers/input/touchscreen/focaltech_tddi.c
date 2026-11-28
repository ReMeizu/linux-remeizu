// SPDX-License-Identifier: GPL-2.0-only
/*
 * FocalTech FT5x46 TDDI and FT5x26 touch controllers
 *
 * Meizu ships this controller family on boards where the vendor kernel talks
 * to it directly instead of through the generic MTK tpd framework taken from
 * another device:
 *
 *   m681/l681 (M3 Note)  FT5x46 @ i2c0 0x38, EINT GPIO1, RST GPIO10
 *   U10/M6/M6T/M3s       FT5x26 @ i2c0 0x38, EINT GPIO1, RST GPIO42
 *   MX6 (M95)            TDDI variant of the FT8716 FHD panel
 *
 * All of them speak the same register/event protocol:
 *
 *   0x00..0x3b  touch points, 6 bytes each: [Xh Yh Xl Yl] [id] [weight]
 *               bit 7..6 of Xh is the event (0 down, 1 up, 2 contact),
 *               id is the low nibble of byte 4, x/y are 12 bits each.
 *   0x02        number of valid points
 *   0xa3        chip id (0x54 = "FT5x46")
 *   0xa5        power mode (0 active, 1 monitor, 3 hibernate)
 *   0xa6        firmware major version
 *   0xa8        vendor id
 *   0x91/0x92   frame counter (ESD watchdog)
 *
 * The point layout is the one documented in the vendor headers as
 * FTS_TOUCH_X_H_POS..FTS_TOUCH_ID_POS (tpd_custom_ft5x26.h, fts_ts.h), which
 * is also what the generic edt-ft5x06 driver assumes for its "generic FT"
 * path.  Unlike edt-ft5x06 this driver does not try to guess the model from
 * the identification registers: the board device tree knows the controller.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define FTS_NAME			"focaltech_tddi"

#define FTS_REG_TD_STATUS		0x02
#define FTS_REG_FRAME_CNT_HI		0x91
#define FTS_REG_FRAME_CNT_LO		0x92
#define FTS_REG_CHIP_ID			0xa3
#define FTS_REG_PMODE			0xa5
#define FTS_REG_FW_VER			0xa6
#define FTS_REG_VENDOR_ID		0xa8

#define FTS_PMODE_ACTIVE		0x00
#define FTS_PMODE_MONITOR		0x01
#define FTS_PMODE_HIBERNATE		0x03

#define FTS_MAX_POINTS			10
#define FTS_POINT_LEN			6
#define FTS_POINT_BUF_LEN		(FTS_POINT_LEN * FTS_MAX_POINTS)
/*
 * Touch event field (bits 7..6 of the X high byte), FT5x0x/FT5x26/FT5x46
 * coding: 0 = press, 1 = release, 2 = contact/continue, 3 = no event.  The
 * controller repeats the frame every scan, so a reported point is down for
 * every event except 3.
 */
#define FTS_EVENT_UP			0x01
#define FTS_EVENT_RESERVED		0x03
#define FTS_X_MASK			0x0fff

/* Reset timing of the vendor driver: low 20 ms, firmware ready after 200 ms */
#define FTS_RESET_LOW_MS		20
#define FTS_RESET_HIGH_MS		200

/* ESD watchdog; the vendor TPD_ESD_CHECK_CIRCLE is one second. */
#define FTS_ESD_CHECK_PERIOD_MS		1000
#define FTS_ESD_MAX_FROZEN		5

/**
 * struct focaltech_data - controller state
 * @client: i2c client
 * @input: input device
 * @prop: touchscreen axis properties from device tree
 * @regmap: register map
 * @reset_gpio: optional reset line
 * @vcc: optional analogue supply
 * @iovcc: optional I/O supply
 * @max_points: number of report slots to decode
 * @locked: only report while the device is resumed
 * @esd_work: ESD watchdog
 * @prev_frames: frame counter seen by the previous watchdog run
 * @frozen_runs: consecutive runs with an unchanged frame counter
 */
struct focaltech_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct touchscreen_properties prop;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	struct regulator *vcc;
	struct regulator *iovcc;
	unsigned int max_points;
	bool locked;
	struct delayed_work esd_work;
	u16 prev_frames;
	unsigned int frozen_runs;
};

static const struct regmap_config focaltech_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

static bool focaltech_has_regulators(struct focaltech_data *data)
{
	return !IS_ERR_OR_NULL(data->vcc) || !IS_ERR_OR_NULL(data->iovcc);
}

static int focaltech_power_on(struct focaltech_data *data)
{
	int ret;

	if (!IS_ERR_OR_NULL(data->iovcc)) {
		ret = regulator_enable(data->iovcc);
		if (ret)
			return ret;
		usleep_range(10, 100);
	}

	if (!IS_ERR_OR_NULL(data->vcc)) {
		ret = regulator_enable(data->vcc);
		if (ret) {
			if (!IS_ERR_OR_NULL(data->iovcc))
				regulator_disable(data->iovcc);
			return ret;
		}
	}

	return 0;
}

static void focaltech_power_off(void *ctx)
{
	struct focaltech_data *data = ctx;

	if (!IS_ERR_OR_NULL(data->vcc))
		regulator_disable(data->vcc);
	if (!IS_ERR_OR_NULL(data->iovcc))
		regulator_disable(data->iovcc);
}

/**
 * focaltech_reset() - pulse the reset line and let the firmware start
 * @data: controller state
 *
 * The FT5x46 starts reporting 200 ms after reset is released; the vendor
 * driver waits the same before the first register access.
 */
static void focaltech_reset(struct focaltech_data *data)
{
	if (!data->reset_gpio)
		return;

	gpiod_set_value_cansleep(data->reset_gpio, 1);
	msleep(FTS_RESET_LOW_MS);
	gpiod_set_value_cansleep(data->reset_gpio, 0);
	msleep(FTS_RESET_HIGH_MS);
}

static int focaltech_report(struct focaltech_data *data)
{
	u8 buf[FTS_POINT_BUF_LEN];
	int i, type, id, x, y, ret;

	ret = regmap_bulk_read(data->regmap, 0x00, buf, sizeof(buf));
	if (ret) {
		dev_err_ratelimited(&data->client->dev,
				    "unable to read touch points: %d\n", ret);
		return ret;
	}

	for (i = 0; i < data->max_points; i++) {
		const u8 *p = &buf[i * FTS_POINT_LEN];

		/* The remaining slots are zeroed and their event is 3. */
		type = (p[0] >> 6) & 0x3;
		if (type == FTS_EVENT_RESERVED)
			continue;

		id = p[4] & 0x0f;
		if (id >= FTS_MAX_POINTS)
			continue;

		x = get_unaligned_be16(&p[0]) & FTS_X_MASK;
		y = get_unaligned_be16(&p[2]) & FTS_X_MASK;

		input_mt_slot(data->input, id);
		if (input_mt_report_slot_state(data->input, MT_TOOL_FINGER,
					       type != FTS_EVENT_UP))
			touchscreen_report_pos(data->input, &data->prop,
					       x, y, true);
	}
	input_mt_sync_frame(data->input);
	input_sync(data->input);

	return 0;
}

static irqreturn_t focaltech_irq(int irq, void *dev_id)
{
	struct focaltech_data *data = dev_id;
	unsigned int dummy;
	int ret;

	/* Reading the status register releases the controller interrupt. */
	ret = regmap_read(data->regmap, FTS_REG_TD_STATUS, &dummy);
	if (ret)
		return IRQ_NONE;

	if (!data->locked)
		return IRQ_HANDLED;

	focaltech_report(data);
	return IRQ_HANDLED;
}

static void focaltech_esd_check(struct work_struct *work)
{
	struct focaltech_data *data = container_of(to_delayed_work(work),
						   struct focaltech_data,
						   esd_work);
	struct device *dev = &data->client->dev;
	unsigned int id, hi, lo;
	u16 frames;
	int ret;

	if (!data->locked)
		return;

	/*
	 * The controller must keep answering on the bus.  A failed read is a
	 * real disagreement (the chip fell off i2c), which is the one case
	 * worth a reset.  The frame counter at 0x91/0x92 is updated per scan
	 * irrespective of touch and wraps at 0xffff, so a change between two
	 * runs is expected; only a counter frozen across several runs while
	 * the chip still answers means the scan engine wedged.  Absence of
	 * touch is never evidence of ESD.
	 */
	ret = regmap_read(data->regmap, FTS_REG_CHIP_ID, &id);
	if (ret)
		goto reset;

	ret = regmap_read(data->regmap, FTS_REG_FRAME_CNT_HI, &hi);
	if (ret)
		goto reset;
	ret = regmap_read(data->regmap, FTS_REG_FRAME_CNT_LO, &lo);
	if (ret)
		goto reset;

	frames = (hi << 8) | lo;
	if (frames == data->prev_frames)
		data->frozen_runs++;
	else
		data->frozen_runs = 0;
	data->prev_frames = frames;

	if (data->frozen_runs > FTS_ESD_MAX_FROZEN)
		goto reset;

	goto out;

	reset:
	dev_warn(dev, "ESD: resetting touch controller after %u frozen runs\n",
		 data->frozen_runs);
	focaltech_reset(data);
	regmap_write(data->regmap, FTS_REG_PMODE, FTS_PMODE_ACTIVE);
	data->frozen_runs = 0;
	data->prev_frames = 0;

out:
	schedule_delayed_work(&data->esd_work,
			      msecs_to_jiffies(FTS_ESD_CHECK_PERIOD_MS));
}

static int focaltech_read_identification(struct focaltech_data *data)
{
	struct device *dev = &data->client->dev;
	unsigned int id, fw, vendor;
	int ret;

	ret = regmap_read(data->regmap, FTS_REG_CHIP_ID, &id);
	if (ret)
		return ret;
	ret = regmap_read(data->regmap, FTS_REG_FW_VER, &fw);
	if (ret)
		return ret;
	ret = regmap_read(data->regmap, FTS_REG_VENDOR_ID, &vendor);
	if (ret)
		return ret;

	dev_info(dev, "FocalTech 0x%02x, firmware 0x%02x, vendor 0x%02x\n",
		 id, fw, vendor);

	return 0;
}

static int focaltech_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct focaltech_data *data;
	struct input_dev *input;
	unsigned long irq_flags;
	u32 max_points;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	data->locked = true;
	i2c_set_clientdata(client, data);

	data->regmap = devm_regmap_init_i2c(client, &focaltech_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(dev, PTR_ERR(data->regmap),
				     "failed to allocate register map\n");

	data->vcc = devm_regulator_get_optional(dev, "vcc");
	if (IS_ERR(data->vcc) && PTR_ERR(data->vcc) == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	data->iovcc = devm_regulator_get_optional(dev, "iovcc");
	if (IS_ERR(data->iovcc) && PTR_ERR(data->iovcc) == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	if (focaltech_has_regulators(data)) {
		ret = focaltech_power_on(data);
		if (ret)
			return dev_err_probe(dev, ret, "failed to power on\n");
		ret = devm_add_action_or_reset(dev, focaltech_power_off, data);
		if (ret)
			return ret;
	}

	data->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						   GPIOD_OUT_HIGH);
	if (IS_ERR(data->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(data->reset_gpio),
				     "failed to get reset GPIO\n");

	focaltech_reset(data);

	ret = focaltech_read_identification(data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "controller does not answer on i2c\n");

	ret = regmap_write(data->regmap, FTS_REG_PMODE, FTS_PMODE_ACTIVE);
	if (ret)
		return dev_err_probe(dev, ret, "failed to activate controller\n");

	data->max_points = FTS_MAX_POINTS;
	if (device_property_read_u32(dev, "focaltech,max-touch-number",
				     &max_points) == 0)
		data->max_points = min_t(u32, max_points, FTS_MAX_POINTS);

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	data->input = input;
	input->name = "FocalTech FT5x46 Touchscreen";
	input->id.bustype = BUS_I2C;
	input->dev.parent = dev;

	/* The controller reports raw coordinates in panel space. */
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, 4095, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, 4095, 0, 0);
	touchscreen_parse_properties(input, true, &data->prop);

	ret = input_mt_init_slots(input, data->max_points,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init MT slots\n");

	irq_flags = irq_get_trigger_type(client->irq);
	if (irq_flags == IRQF_TRIGGER_NONE)
		irq_flags = IRQF_TRIGGER_FALLING;
	irq_flags |= IRQF_ONESHOT;

	ret = devm_request_threaded_irq(dev, client->irq, NULL, focaltech_irq,
					irq_flags, client->name, data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ %d\n",
				     client->irq);

	ret = input_register_device(input);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register input\n");

	INIT_DELAYED_WORK(&data->esd_work, focaltech_esd_check);
	schedule_delayed_work(&data->esd_work,
			      msecs_to_jiffies(FTS_ESD_CHECK_PERIOD_MS));

	dev_dbg(dev, "initialised, IRQ %d, %u points\n",
		client->irq, data->max_points);

	return 0;
}

static void focaltech_remove(struct i2c_client *client)
{
	struct focaltech_data *data = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&data->esd_work);
}

static int focaltech_suspend(struct device *dev)
{
	struct focaltech_data *data = i2c_get_clientdata(to_i2c_client(dev));

	data->locked = false;
	cancel_delayed_work_sync(&data->esd_work);

	/* Hibernate the scan engine; the vendor driver writes 3 here. */
	regmap_write(data->regmap, FTS_REG_PMODE, FTS_PMODE_HIBERNATE);

	if (focaltech_has_regulators(data))
		focaltech_power_off(data);
	else if (data->reset_gpio)
		gpiod_set_value_cansleep(data->reset_gpio, 1);

	return 0;
}

static int focaltech_resume(struct device *dev)
{
	struct focaltech_data *data = i2c_get_clientdata(to_i2c_client(dev));
	int ret;

	if (focaltech_has_regulators(data)) {
		ret = focaltech_power_on(data);
		if (ret)
			return ret;
	}

	focaltech_reset(data);

	ret = regmap_write(data->regmap, FTS_REG_PMODE, FTS_PMODE_ACTIVE);
	if (ret)
		return ret;

	data->locked = true;
	data->frozen_runs = 0;
	data->prev_frames = 0;
	schedule_delayed_work(&data->esd_work,
			      msecs_to_jiffies(FTS_ESD_CHECK_PERIOD_MS));

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(focaltech_pm_ops, focaltech_suspend,
				focaltech_resume);

static const struct of_device_id focaltech_of_match[] = {
	{ .compatible = "focaltech,ft5x46" },
	{ .compatible = "focaltech,ft5x26" },
	{ .compatible = "focaltech,ft8716-tddi" },
	{ }
};
MODULE_DEVICE_TABLE(of, focaltech_of_match);

static const struct i2c_device_id focaltech_i2c_id[] = {
	{ "ft5x46" },
	{ "ft5x26" },
	{ "ft8716_tddi" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, focaltech_i2c_id);

static struct i2c_driver focaltech_driver = {
	.driver = {
		.name = FTS_NAME,
		.of_match_table = focaltech_of_match,
		.pm = pm_sleep_ptr(&focaltech_pm_ops),
	},
	.probe = focaltech_probe,
	.remove = focaltech_remove,
	.id_table = focaltech_i2c_id,
};
module_i2c_driver(focaltech_driver);

MODULE_AUTHOR("ReMeizu");
MODULE_DESCRIPTION("FocalTech FT5x46/FT5x26/FT8716 TDDI touchscreen driver");
MODULE_LICENSE("GPL");
