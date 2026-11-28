// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6328 PMIC AUXADC IIO driver
 *
 * Copyright (c) 2015 MediaTek Inc.
 * Copyright (c) 2022 Yassine Oudjana <y.oudjana@protonmail.com>
 * Copyright (c) 2026 The ReMeizu project
 *
 * The MT6328 is the companion PMIC of the MT6735/MT6737M SoCs. Its
 * auxiliary ADC is reachable through the PMIC wrapper and provides the
 * battery voltage, the battery/charger NTC, the PMIC die temperature and
 * the battery-ID ladder used to select a fuel gauge profile.
 *
 * Register offsets are taken from the MediaTek 4.9 BSP
 * (drivers/misc/mediatek/include/mt-plat/mt6735/include/mach/upmu_hw.h:
 * MT6328_AUXADC_*).
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/types.h>

#include <linux/iio/iio.h>
#include <linux/iio/adc/mediatek-pmic-auxadc.h>

#include <linux/mfd/mt6397/core.h>

#include <dt-bindings/iio/adc/mediatek,mt6328-auxadc.h>

/*
 * Full scale input range of the AUXADC in millivolts. The converter has a
 * 12 bit resolution; channels 12..15 are selected through AUXADC_CON4[15:13].
 */
#define MT6328_AUXADC_VREF_MV		1800
#define MT6328_AUXADC_MAX_CODE		4095

/* AUXADC_CON2/CON3: averaging and sleep sampling parameters from BSP */
#define MT6328_AUXADC_AVG_NUM_SEL	0x03
#define MT6328_AUXADC_SPL_NUM		0x18

static const struct mtk_pmic_auxadc_data mt6328_auxadc_data = {
	.name = "MT6328",
	.regs = {
		[MTK_PMIC_AUXADC_REG_FGADC_CON0]	= 0x0ca4,
		[MTK_PMIC_AUXADC_REG_FGADC_CON1]	= 0x0ca6,
		[MTK_PMIC_AUXADC_REG_AUXADC_ADC0]	= 0x0e00,
		[MTK_PMIC_AUXADC_REG_AUXADC_STA0]	= 0x0e90,
		[MTK_PMIC_AUXADC_REG_AUXADC_RQST0_SET]	= 0x0e98,
		[MTK_PMIC_AUXADC_REG_AUXADC_CON0]	= 0x0ea2,
		[MTK_PMIC_AUXADC_REG_AUXADC_CON0_SET]	= 0x0ea4,
		[MTK_PMIC_AUXADC_REG_AUXADC_CON0_CLR]	= 0x0ea6,
		[MTK_PMIC_AUXADC_REG_AUXADC_CON2]	= 0x0eaa,
		[MTK_PMIC_AUXADC_REG_AUXADC_CON3]	= 0x0eac,
		[MTK_PMIC_AUXADC_REG_AUXADC_CON4]	= 0x0eae,
		[MTK_PMIC_AUXADC_REG_AUXADC_CON6]	= 0x0eb2,
		[MTK_PMIC_AUXADC_REG_AUXADC_CON7]	= 0x0eb4,
		[MTK_PMIC_AUXADC_REG_AUXADC_IMP0]	= 0x0ef6,
		[MTK_PMIC_AUXADC_REG_AUXADC_IMP1]	= 0x0ef8,
		[MTK_PMIC_AUXADC_REG_TOP_CKPDN_CON2]	= 0x0248,
	},
	.dcxo_con2_large = true,
	.avg_num_sel = MT6328_AUXADC_AVG_NUM_SEL,
	.spl_num_sleep = MT6328_AUXADC_SPL_NUM,
	.reads_batid = true,
	.reads_chrtemp = true,
	.isense_r_bat = 68000,
	.vcdt_r1 = 330000,
	.vcdt_r2 = 39000,
	.battemp_pull_up = 16900,
	.battemp_pull_volt = 1800,
};

/*
 * Channels 0..1 (BATSNS/ISENSE) and 13 (VBAT) are converted with a gain of 3,
 * all others with a gain of 1.
 */
static const int mt6328_auxadc_gain[MTK_PMIC_AUXADC_CHAN_MAX] = {
	[MTK_PMIC_AUXADC_CHAN_BATSNS] = 3,
	[MTK_PMIC_AUXADC_CHAN_ISENSE] = 3,
	[MTK_PMIC_AUXADC_CHAN_VCDT] = 1,
	[MTK_PMIC_AUXADC_CHAN_BATTEMP] = 1,
	[MTK_PMIC_AUXADC_CHAN_CHIPTEMP] = 1,
	[MTK_PMIC_AUXADC_CHAN_ACCDET] = 1,
	[MTK_PMIC_AUXADC_CHAN_TSX] = 1,
	[MTK_PMIC_AUXADC_CHAN_HPOFS] = 1,
	[MTK_PMIC_AUXADC_CHAN_BATID] = 1,
	[MTK_PMIC_AUXADC_CHAN_VBAT] = 3,
};

#define MT6328_AUXADC_CHAN(_chan, _name, _type)				\
{									\
	.type = _type,							\
	.channel = MT6328_AUXADC_##_chan,				\
	.datasheet_name = _name,					\
	.indexed = 1,							\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |			\
			      BIT(IIO_CHAN_INFO_SCALE),			\
}

static const struct iio_chan_spec mt6328_auxadc_channels[] = {
	MT6328_AUXADC_CHAN(BATSNS, "batsns", IIO_VOLTAGE),
	MT6328_AUXADC_CHAN(ISENSE, "isense", IIO_CURRENT),
	MT6328_AUXADC_CHAN(VCDT, "vcdt", IIO_VOLTAGE),
	MT6328_AUXADC_CHAN(BATTEMP, "battemp", IIO_TEMP),
	MT6328_AUXADC_CHAN(CHIPTEMP, "chiptemp", IIO_TEMP),
	MT6328_AUXADC_CHAN(ACCDET, "accdet", IIO_RESISTANCE),
	MT6328_AUXADC_CHAN(TSX, "tsx_temp", IIO_TEMP),
	MT6328_AUXADC_CHAN(HPOFS_CAL, "hpofs_cal", IIO_RESISTANCE),
	MT6328_AUXADC_CHAN(BATID, "batid", IIO_RESISTANCE),
	MT6328_AUXADC_CHAN(VBAT, "vbat", IIO_VOLTAGE),
};

struct mt6328_auxadc {
	struct device *dev;
	struct regmap *regmap;
	struct mediatek_pmic_auxadc *adc;
};

static int mt6328_auxadc_read_label(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    char *label)
{
	return sysfs_emit(label, "%s\n", chan->datasheet_name);
}

static int mt6328_auxadc_read_raw(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan,
				  int *val, int *val2, long mask)
{
	struct mt6328_auxadc *priv = iio_priv(indio_dev);
	int gain = mt6328_auxadc_gain[chan->channel];
	u32 raw;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->channel) {
		case MT6328_AUXADC_BATID:
		case MT6328_AUXADC_VBAT:
			ret = mediatek_pmic_auxadc_read_batid(priv->adc, chan->channel,
							      &raw);
			break;
		default:
			ret = mediatek_pmic_auxadc_read(priv->adc, chan->channel,
							&raw);
			break;
		}
		if (ret)
			return ret;

		*val = raw;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		/* millivolts per full-scale count */
		*val = gain * MT6328_AUXADC_VREF_MV;
		*val2 = MT6328_AUXADC_MAX_CODE + 1;
		return IIO_VAL_FRACTIONAL;
	default:
		return -EINVAL;
	}
}

static const struct iio_info mt6328_auxadc_info = {
	.read_label = mt6328_auxadc_read_label,
	.read_raw = mt6328_auxadc_read_raw,
};

static int mt6328_auxadc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *regmap_dev;
	struct mt6328_auxadc *priv;
	struct iio_dev *indio_dev;
	struct regmap *regmap;
	int ret;

	/* The MT6397 MFD is a child of the PMIC wrapper */
	regmap_dev = dev->parent ? dev->parent->parent : NULL;
	if (!regmap_dev)
		return -ENODEV;

	regmap = dev_get_regmap(regmap_dev, NULL);
	if (!regmap)
		return dev_err_probe(dev, -ENODEV, "Failed to get PMIC regmap\n");

	indio_dev = devm_iio_device_alloc(dev, sizeof(*priv));
	if (!indio_dev)
		return -ENOMEM;

	priv = iio_priv(indio_dev);
	priv->dev = dev;
	priv->regmap = regmap;
	indio_dev->name = "mt6328-auxadc";
	indio_dev->info = &mt6328_auxadc_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = mt6328_auxadc_channels;
	indio_dev->num_channels = ARRAY_SIZE(mt6328_auxadc_channels);

	priv->adc = mediatek_pmic_auxadc_alloc(dev, regmap, &mt6328_auxadc_data);
	if (IS_ERR(priv->adc))
		return dev_err_probe(dev, PTR_ERR(priv->adc),
				     "Failed to register AUXADC helper\n");

	ret = mediatek_pmic_auxadc_init(priv->adc);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialise the AUXADC\n");

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register IIO device\n");

	return 0;
}

static const struct of_device_id mt6328_auxadc_of_match[] = {
	{ .compatible = "mediatek,mt6328-auxadc", },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6328_auxadc_of_match);

static struct platform_driver mt6328_auxadc_driver = {
	.driver = {
		.name = "mt6328-auxadc",
		.of_match_table = mt6328_auxadc_of_match,
	},
	.probe = mt6328_auxadc_probe,
};
module_platform_driver(mt6328_auxadc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek MT6328 PMIC AUXADC driver");
