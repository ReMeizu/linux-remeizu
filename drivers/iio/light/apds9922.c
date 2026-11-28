// SPDX-License-Identifier: GPL-2.0-only
/*
 * Avago/Broadcom APDS-9922 ambient light and proximity sensor
 *
 * The MX6 (Meizu M95) hangs an APDS9922 with both functions enabled on
 * i2c1 at 0x53, interrupt on EINT11 (GPIO88, active low).  The part shares
 * the 0x00..0x2f register map of the APDS-992x family but no upstream driver
 * covers it: apds9300.c is the older page-based APDS-9300 layout and
 * apds9960.c is the gesture part.  The register map below is taken from the
 * vendor driver (drivers/input/apds9922/apds9922.{c,h} in the 3.18 MX6
 * kernel tree).
 *
 * Channels: illuminance (raw ALS counts with a settable gain) and distance
 * (raw proximity counts, 11 bit resolution).
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include <linux/iio/iio.h>
#include <linux/iio/events.h>

#define APDS9922_DRV_NAME		"apds9922"

#define APDS9922_REG_MAIN_CTRL		0x00
#define APDS9922_REG_PRX_LED		0x01
#define APDS9922_REG_PRX_PULSES		0x02
#define APDS9922_REG_PRX_RATE		0x03
#define APDS9922_REG_ALS_RATE		0x04
#define APDS9922_REG_ALS_GAIN		0x05
#define APDS9922_REG_PART_ID		0x06
#define APDS9922_REG_STATUS		0x07
#define APDS9922_REG_PRX_DATA		0x08
#define APDS9922_REG_ALS_DATA		0x0d
#define APDS9922_REG_INT_PERSIST	0x1a
#define APDS9922_REG_PRX_THRES_UP	0x1b
#define APDS9922_REG_PRX_THRES_LOW	0x1d

#define APDS9922_PRX_EN			BIT(0)
#define APDS9922_ALS_EN			BIT(1)
#define APDS9922_SW_RESET		BIT(4)

#define APDS9922_LED_CURRENT_50MA	0x4
#define APDS9922_PRX_PULSES_8		7
#define APDS9922_PRX_RATE_100MS		0x5
#define APDS9922_PRX_RES_11BIT		(0x3 << 3)
#define APDS9922_ALS_RATE_100MS		0x2
#define APDS9922_ALS_RES_18BIT		(0x2 << 4)

#define APDS9922_STATUS_PRX_INT		BIT(1)
#define APDS9922_STATUS_ALS_INT		BIT(4)

/*
 * Proximity "covered"/"clear" thresholds and persistence.  The vendor driver
 * hard-codes these for the MX6 window (apds9922.h: PS 50/40, pulse count 8,
 * 100 ms measurement rate, 11 bit resolution, ALS 18 bit at 100 ms) rather
 * than reading values from the device tree, so they are kept as constants
 * matching the hardware the board ships with.
 */
#define APDS9922_INT_PERSIST_BOTH	0x11
#define APDS9922_PS_THRESHOLD_HIGH	50
#define APDS9922_PS_THRESHOLD_LOW	40

/* ALS gain register values, index into the scale table below */
static const int apds9922_als_gains[] = { 1, 3, 6, 9, 18 };

struct apds9922_data {
	struct i2c_client *client;
	struct regmap *regmap;
	struct mutex lock;
};

static int apds9922_power(struct apds9922_data *data, bool on)
{
	return regmap_update_bits(data->regmap, APDS9922_REG_MAIN_CTRL,
				  APDS9922_PRX_EN | APDS9922_ALS_EN,
				  on ? (APDS9922_PRX_EN | APDS9922_ALS_EN) : 0);
}

static int apds9922_setup(struct apds9922_data *data)
{
	int ret;

	/* Soft reset, then wait for the part to come back (vendor: 10 ms). */
	ret = regmap_update_bits(data->regmap, APDS9922_REG_MAIN_CTRL,
				 APDS9922_SW_RESET, APDS9922_SW_RESET);
	if (ret)
		return ret;
	msleep(10);

	ret = regmap_write(data->regmap, APDS9922_REG_PRX_LED,
			   APDS9922_LED_CURRENT_50MA);
	if (ret)
		return ret;
	ret = regmap_write(data->regmap, APDS9922_REG_PRX_PULSES,
			   APDS9922_PRX_PULSES_8);
	if (ret)
		return ret;
	ret = regmap_write(data->regmap, APDS9922_REG_PRX_RATE,
			   APDS9922_PRX_RATE_100MS | APDS9922_PRX_RES_11BIT);
	if (ret)
		return ret;
	ret = regmap_write(data->regmap, APDS9922_REG_ALS_RATE,
			   APDS9922_ALS_RATE_100MS | APDS9922_ALS_RES_18BIT);
	if (ret)
		return ret;

	/* Interrupt after two consecutive measurements past a threshold. */
	ret = regmap_write(data->regmap, APDS9922_REG_INT_PERSIST,
			   APDS9922_INT_PERSIST_BOTH);
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, APDS9922_REG_PRX_THRES_UP,
			   APDS9922_PS_THRESHOLD_HIGH & 0xff);
	if (ret)
		return ret;
	ret = regmap_write(data->regmap, APDS9922_REG_PRX_THRES_UP + 1,
			   APDS9922_PS_THRESHOLD_HIGH >> 8);
	if (ret)
		return ret;
	ret = regmap_write(data->regmap, APDS9922_REG_PRX_THRES_LOW,
			   APDS9922_PS_THRESHOLD_LOW & 0xff);
	if (ret)
		return ret;
	ret = regmap_write(data->regmap, APDS9922_REG_PRX_THRES_LOW + 1,
			   APDS9922_PS_THRESHOLD_LOW >> 8);
	if (ret)
		return ret;

	return apds9922_power(data, true);
}

/**
 * apds9922_read_als() - read the ALS result
 * @data: driver data
 * @val: destination
 *
 * The register block holds the ALS low and high bytes at 0x0d/0x0e; these
 * are the two bytes the vendor driver converts to lux, so they are the ones
 * exposed here.
 */
static int apds9922_read_als(struct apds9922_data *data, int *val)
{
	unsigned int lo, hi;
	int ret;

	ret = regmap_read(data->regmap, APDS9922_REG_ALS_DATA, &lo);
	if (ret)
		return ret;
	ret = regmap_read(data->regmap, APDS9922_REG_ALS_DATA + 1, &hi);
	if (ret)
		return ret;

	*val = (hi << 8) | lo;
	return 0;
}

static int apds9922_read_raw(struct iio_dev *indio_dev,
			     const struct iio_chan_spec *chan, int *val,
			     int *val2, long mask)
{
	struct apds9922_data *data = iio_priv(indio_dev);
	unsigned int gain, prx;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&data->lock);
		switch (chan->type) {
		case IIO_PROXIMITY:
			ret = regmap_read(data->regmap, APDS9922_REG_PRX_DATA,
					  &prx);
			*val = prx;
			break;
		case IIO_LIGHT:
			ret = apds9922_read_als(data, val);
			break;
		default:
			ret = -EINVAL;
		}
		mutex_unlock(&data->lock);
		return ret ? ret : IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		if (chan->type != IIO_LIGHT)
			return -EINVAL;
		ret = regmap_read(data->regmap, APDS9922_REG_ALS_GAIN, &gain);
		if (ret)
			return ret;
		gain = min_t(unsigned int, gain & 0x7, ARRAY_SIZE(apds9922_als_gains) - 1);
		/* 18 bit ALS, gain 1: one ADC count is 1/4 lux */
		*val = 0;
		*val2 = 250000 * apds9922_als_gains[gain];
		return IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

static int apds9922_write_raw(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan, int val,
			      int val2, long mask)
{
	struct apds9922_data *data = iio_priv(indio_dev);
	int i;

	if (mask != IIO_CHAN_INFO_CALIBSCALE || chan->type != IIO_LIGHT)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(apds9922_als_gains); i++) {
		if (apds9922_als_gains[i] == val) {
			mutex_lock(&data->lock);
			i = regmap_write(data->regmap, APDS9922_REG_ALS_GAIN, i);
			mutex_unlock(&data->lock);
			return i;
		}
	}

	return -EINVAL;
}

static const struct iio_info apds9922_info = {
	.read_raw = apds9922_read_raw,
	.write_raw = apds9922_write_raw,
};

static const struct iio_chan_spec apds9922_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_CALIBSCALE),
	},
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
};

static irqreturn_t apds9922_irq(int irq, void *priv)
{
	struct iio_dev *indio_dev = priv;
	struct apds9922_data *data = iio_priv(indio_dev);
	unsigned int status;
	int ret;

	ret = regmap_read(data->regmap, APDS9922_REG_STATUS, &status);
	if (ret)
		return IRQ_NONE;

	if (status & APDS9922_STATUS_ALS_INT)
		iio_push_event(indio_dev,
			       IIO_UNMOD_EVENT_CODE(IIO_LIGHT, 0,
						    IIO_EV_TYPE_THRESH,
						    IIO_EV_DIR_EITHER),
			       iio_get_time_ns(indio_dev));

	if (status & APDS9922_STATUS_PRX_INT)
		iio_push_event(indio_dev,
			       IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, 0,
						    IIO_EV_TYPE_THRESH,
						    IIO_EV_DIR_EITHER),
			       iio_get_time_ns(indio_dev));

	return IRQ_HANDLED;
}

static const struct regmap_config apds9922_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x2f,
};

static int apds9922_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct apds9922_data *data;
	unsigned int part_id;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	mutex_init(&data->lock);
	i2c_set_clientdata(client, indio_dev);

	data->regmap = devm_regmap_init_i2c(client, &apds9922_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(data->regmap),
				     "failed to allocate register map\n");

	ret = regmap_read(data->regmap, APDS9922_REG_PART_ID, &part_id);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to read part id\n");

	if (client->irq) {
		ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
						apds9922_irq,
						IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						client->name, indio_dev);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "failed to request IRQ\n");
	}

	ret = apds9922_setup(data);
	if (ret)
		return dev_err_probe(&client->dev, ret, "failed to init sensor\n");

	indio_dev->info = &apds9922_info;
	indio_dev->name = APDS9922_DRV_NAME;
	indio_dev->channels = apds9922_channels;
	indio_dev->num_channels = ARRAY_SIZE(apds9922_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = iio_device_register(indio_dev);
	if (ret)
		return ret;

	dev_info(&client->dev, "APDS9922 ready, part id 0x%02x\n", part_id);

	return 0;
}

static void apds9922_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct apds9922_data *data = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	apds9922_power(data, false);
}

static int apds9922_suspend(struct device *dev)
{
	struct apds9922_data *data =
		iio_priv(i2c_get_clientdata(to_i2c_client(dev)));

	return apds9922_power(data, false);
}

static int apds9922_resume(struct device *dev)
{
	struct apds9922_data *data =
		iio_priv(i2c_get_clientdata(to_i2c_client(dev)));

	return apds9922_power(data, true);
}

static DEFINE_SIMPLE_DEV_PM_OPS(apds9922_pm_ops, apds9922_suspend,
				apds9922_resume);

static const struct of_device_id apds9922_of_match[] = {
	{ .compatible = "avago,apds9922" },
	{ }
};
MODULE_DEVICE_TABLE(of, apds9922_of_match);

static const struct i2c_device_id apds9922_id[] = {
	{ "apds9922" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, apds9922_id);

static struct i2c_driver apds9922_driver = {
	.driver = {
		.name = APDS9922_DRV_NAME,
		.of_match_table = apds9922_of_match,
		.pm = pm_sleep_ptr(&apds9922_pm_ops),
	},
	.probe = apds9922_probe,
	.remove = apds9922_remove,
	.id_table = apds9922_id,
};
module_i2c_driver(apds9922_driver);

MODULE_AUTHOR("ReMeizu");
MODULE_DESCRIPTION("Avago APDS-9922 ambient light and proximity sensor");
MODULE_LICENSE("GPL");
