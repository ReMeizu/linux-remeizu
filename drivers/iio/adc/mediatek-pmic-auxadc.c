// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek PMIC AUXADC helper library
 *
 * Copyright (c) 2015 MediaTek Inc.
 * Copyright (c) 2026 The ReMeizu project
 *
 * The MT6328 / MT6351 / MT6353 PMIC family hosts a 16 channel auxiliary ADC.
 * A conversion is requested by writing bit @chan into AUXADC_RQST0_SET; the
 * block then samples the channel and publishes the result in ADC23
 * (AUXADC_ADC0 + 0x2e) with the ready flag in bit 15 and the 12 bit code in
 * bits 11:0. This is the "BY_AP" result used by the vendor
 * PMIC_IMM_GetOneChannelValue() for every PMIC of the family, unlike the
 * per-channel data registers whose wiring differs between parts.
 *
 * The IIO driver owns the instance. The fuel gauge and the thermal controller
 * look it up through their devicetree phandle, so the library keeps the
 * instances in a small table keyed by the PMIC node, and the instance is
 * removed when its owning device is unbound.
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/xarray.h>

#include <linux/iio/adc/mediatek-pmic-auxadc.h>

#define AUXADC_POLL_INTERVAL_US		100
#define AUXADC_CONV_TIMEOUT_US		40000
#define AUXADC_IMP_TIMEOUT_US		80000
#define AUXADC_POWER_ON_DELAY_US	100
#define AUXADC_IMPD_DELAY_US		5000

/* Offset of the AP immediate conversion result from AUXADC_ADC0 */
#define AUXADC_AP_RESULT_OFFSET		0x2e

#define AUXADC_DATA_MASK		GENMASK(11, 0)
#define AUXADC_RDY			BIT(15)

/* AUXADC_CON0 bits */
#define AUXADC_CON0_CK_ON_EXTD		GENMASK(5, 0)
#define AUXADC_CON0_ADC_PWDB		BIT(8)
#define AUXADC_CON0_ADC_PWDB_SWCTRL	BIT(9)
#define AUXADC_CON0_CK_AON		BIT(15)

/* AUXADC_CON2 bits */
#define AUXADC_CON2_AVG_NUM_SEL		GENMASK(13, 10)
#define AUXADC_CON2_DCXO_LARGE		BIT(3)

/* AUXADC_CON3: sampling number used by the automatic conversions */
#define AUXADC_CON3_SPL_NUM_SLEEP	GENMASK(9, 0)

/* AUXADC_CON4: channel selector for the shared channels 12..15 */
#define AUXADC_CON4_CHSEL_12_15		GENMASK(15, 13)

/* AUXADC_IMP0: impedance measurement control */
#define AUXADC_IMP0_TRIG_CNT_CLEAR	BIT(1)
#define AUXADC_IMP0_TRIG_IMP		BIT(0)
#define AUXADC_IMP0_IRQ_STATUS		BIT(8)
#define AUXADC_IMP0_MODE		BIT(15)

/* AUXADC_IMP1: impedance measurement result */
#define AUXADC_IMP1_DATA_MASK		GENMASK(14, 0)
#define AUXADC_IMP1_RDY			BIT(15)

/* TOP_CKPDN_CON2: FGADC analog/digital clock power down */
#define TOP_CKPDN2_FGADC_ANA_CK_PDN	BIT(2)
#define TOP_CKPDN2_FGADC_DIG_CK_PDN	BIT(3)

/**
 * struct mediatek_pmic_auxadc - AUXADC instance
 * @dev:	device owning the instance
 * @np:		devicetree node consumers use to find the instance
 * @regmap:	regmap of the PMIC (from the pwrap)
 * @data:	PMIC and board description
 * @lock:	serialises register state changes and conversions
 * @powered:	the analog block has been brought out of power down
 */
struct mediatek_pmic_auxadc {
	struct device *dev;
	struct device_node *np;
	struct regmap *regmap;
	const struct mtk_pmic_auxadc_data *data;
	struct mutex lock;
	bool powered;
};

static DEFINE_MUTEX(mediatek_pmic_auxadc_lock);
static DEFINE_XARRAY(mediatek_pmic_auxadc_instances);

static u16 mp_reg(struct mediatek_pmic_auxadc *adc, enum mtk_pmic_auxadc_regs reg)
{
	return adc->data->regs[reg];
}

static int mp_power_on_locked(struct mediatek_pmic_auxadc *adc)
{
	int ret;

	if (adc->powered || adc->data->no_pwdb)
		return 0;

	/* Un-gate the FGADC clocks: the impedance measurement needs them. */
	ret = regmap_clear_bits(adc->regmap,
				mp_reg(adc, MTK_PMIC_AUXADC_REG_TOP_CKPDN_CON2),
				TOP_CKPDN2_FGADC_ANA_CK_PDN |
				TOP_CKPDN2_FGADC_DIG_CK_PDN);
	if (ret)
		return ret;

	/* Keep the ADC clock alive in suspend */
	ret = regmap_set_bits(adc->regmap,
			      mp_reg(adc, MTK_PMIC_AUXADC_REG_AUXADC_CON0_SET),
			      AUXADC_CON0_CK_ON_EXTD | AUXADC_CON0_CK_AON);
	if (ret)
		return ret;

	/* Release the analog power down through the write-1-to-clear bank */
	ret = regmap_write(adc->regmap,
			   mp_reg(adc, MTK_PMIC_AUXADC_REG_AUXADC_CON0_CLR),
			   AUXADC_CON0_ADC_PWDB | AUXADC_CON0_ADC_PWDB_SWCTRL);
	if (ret)
		return ret;

	usleep_range(AUXADC_POWER_ON_DELAY_US, AUXADC_POWER_ON_DELAY_US * 2);
	adc->powered = true;

	return 0;
}

static void mp_unregister(void *data)
{
	struct mediatek_pmic_auxadc *adc = data;

	guard(mutex)(&mediatek_pmic_auxadc_lock);
	xa_erase(&mediatek_pmic_auxadc_instances, adc->np->phandle);
}

/**
 * mediatek_pmic_auxadc_alloc - create an AUXADC instance owned by @dev
 * @dev:	device that owns the AUXADC (the PMIC AUXADC driver)
 * @regmap:	PMIC regmap
 * @data:	PMIC description
 *
 * The instance is registered under the devicetree node of @dev so that
 * consumers can find it with mediatek_pmic_auxadc_get(). It is released
 * automatically when @dev is unbound.
 */
struct mediatek_pmic_auxadc *
mediatek_pmic_auxadc_alloc(struct device *dev, struct regmap *regmap,
			   const struct mtk_pmic_auxadc_data *data)
{
	struct mediatek_pmic_auxadc *adc;
	int ret;

	if (!data || !regmap)
		return ERR_PTR(-EINVAL);
	if (!dev->of_node)
		return ERR_PTR(-ENODEV);

	adc = devm_kzalloc(dev, sizeof(*adc), GFP_KERNEL);
	if (!adc)
		return ERR_PTR(-ENOMEM);

	adc->dev = dev;
	adc->np = dev->of_node;
	adc->regmap = regmap;
	adc->data = data;
	mutex_init(&adc->lock);

	guard(mutex)(&mediatek_pmic_auxadc_lock);
	ret = xa_err(xa_store(&mediatek_pmic_auxadc_instances, adc->np->phandle,
			      adc, GFP_KERNEL));
	if (ret)
		return ERR_PTR(ret);

	ret = devm_add_action_or_reset(dev, mp_unregister, adc);
	if (ret)
		return ERR_PTR(ret);

	return adc;
}
EXPORT_SYMBOL_GPL(mediatek_pmic_auxadc_alloc);

/**
 * mediatek_pmic_auxadc_get - look up the instance published for @np
 * @np:		devicetree node of the PMIC AUXADC
 *
 * Returns NULL when no instance is published for @np; callers that cannot
 * work without the AUXADC turn that into -EPROBE_DEFER.
 */
struct mediatek_pmic_auxadc *mediatek_pmic_auxadc_get(struct device_node *np)
{
	struct mediatek_pmic_auxadc *adc;

	if (!np)
		return NULL;

	guard(mutex)(&mediatek_pmic_auxadc_lock);
	adc = xa_load(&mediatek_pmic_auxadc_instances, np->phandle);

	return adc;
}
EXPORT_SYMBOL_GPL(mediatek_pmic_auxadc_get);

void mediatek_pmic_auxadc_put(struct mediatek_pmic_auxadc *adc)
{
	/* The instance lives as long as its owning device; nothing to drop. */
	(void)adc;
}
EXPORT_SYMBOL_GPL(mediatek_pmic_auxadc_put);

/**
 * mp_configure_locked - program the static sampling parameters
 * @adc:	AUXADC instance
 *
 * Programs the averaging width and the sampling number used by the automatic
 * (sleep and impedance) conversions. The values come from the MediaTek BSP
 * (pmic_auxadc_init(): AVG_NUM_SEL 0x83, SPL_NUM_SLEEP 0x18) and must match
 * what the fuel gauge expects.
 */
static int mp_configure_locked(struct mediatek_pmic_auxadc *adc)
{
	const struct mtk_pmic_auxadc_data *d = adc->data;
	int ret;

	ret = regmap_update_bits(adc->regmap,
				 mp_reg(adc, MTK_PMIC_AUXADC_REG_AUXADC_CON2),
				 AUXADC_CON2_AVG_NUM_SEL,
				 FIELD_PREP(AUXADC_CON2_AVG_NUM_SEL,
					    d->avg_num_sel));
	if (ret)
		return ret;

	ret = regmap_update_bits(adc->regmap,
				 mp_reg(adc, MTK_PMIC_AUXADC_REG_AUXADC_CON2),
				 AUXADC_CON2_DCXO_LARGE,
				 d->dcxo_con2_large ? AUXADC_CON2_DCXO_LARGE : 0);
	if (ret)
		return ret;

	ret = regmap_update_bits(adc->regmap,
				 mp_reg(adc, MTK_PMIC_AUXADC_REG_AUXADC_CON3),
				 AUXADC_CON3_SPL_NUM_SLEEP,
				 d->spl_num_sleep);
	if (ret)
		return ret;

	/* Channel selector of the shared channels 12..15 */
	return regmap_update_bits(adc->regmap,
				  mp_reg(adc, MTK_PMIC_AUXADC_REG_AUXADC_CON4),
				  AUXADC_CON4_CHSEL_12_15,
				  FIELD_PREP(AUXADC_CON4_CHSEL_12_15,
					     d->sel_12_15));
}

int mediatek_pmic_auxadc_init(struct mediatek_pmic_auxadc *adc)
{
	int ret;

	if (IS_ERR_OR_NULL(adc))
		return adc ? PTR_ERR(adc) : -ENODEV;

	guard(mutex)(&adc->lock);

	ret = mp_power_on_locked(adc);
	if (ret)
		return ret;

	return mp_configure_locked(adc);
}
EXPORT_SYMBOL_GPL(mediatek_pmic_auxadc_init);

int mediatek_pmic_auxadc_power_on(struct mediatek_pmic_auxadc *adc)
{
	int ret;

	if (IS_ERR_OR_NULL(adc))
		return adc ? PTR_ERR(adc) : -ENODEV;

	guard(mutex)(&adc->lock);

	ret = mp_power_on_locked(adc);
	if (ret)
		return ret;

	return mp_configure_locked(adc);
}
EXPORT_SYMBOL_GPL(mediatek_pmic_auxadc_power_on);

static int mp_read_chan_locked(struct mediatek_pmic_auxadc *adc, unsigned int chan,
			       u32 *raw)
{
	u16 rqst = mp_reg(adc, MTK_PMIC_AUXADC_REG_AUXADC_RQST0_SET);
	u16 result = mp_reg(adc, MTK_PMIC_AUXADC_REG_AUXADC_ADC0) +
		     AUXADC_AP_RESULT_OFFSET;
	u32 val;
	int ret;

	/* Make sure the previous conversion has completed */
	ret = regmap_read_poll_timeout(adc->regmap, result, val,
				       !(val & AUXADC_RDY),
				       AUXADC_POLL_INTERVAL_US,
				       AUXADC_CONV_TIMEOUT_US);
	if (ret)
		return ret;

	ret = regmap_write(adc->regmap, rqst, BIT(chan));
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(adc->regmap, result, val,
				       val & AUXADC_RDY,
				       AUXADC_POLL_INTERVAL_US,
				       AUXADC_CONV_TIMEOUT_US);

	/* Always release the request so the block can go idle again */
	regmap_write(adc->regmap, rqst, 0);

	if (ret)
		return ret;

	*raw = val & AUXADC_DATA_MASK;

	return 0;
}

/**
 * mediatek_pmic_auxadc_read - run one immediate conversion on @chan
 * @adc:	AUXADC instance
 * @chan:	channel index, 0..15
 * @raw:	raw 12 bit conversion result
 */
int mediatek_pmic_auxadc_read(struct mediatek_pmic_auxadc *adc, unsigned int chan,
			      u32 *raw)
{
	int ret;

	if (IS_ERR_OR_NULL(adc))
		return adc ? PTR_ERR(adc) : -ENODEV;
	if (chan >= MTK_PMIC_AUXADC_CHAN_MAX)
		return -EINVAL;

	guard(mutex)(&adc->lock);

	ret = mp_power_on_locked(adc);
	if (ret)
		return ret;

	return mp_read_chan_locked(adc, chan, raw);
}
EXPORT_SYMBOL_GPL(mediatek_pmic_auxadc_read);

/**
 * mediatek_pmic_auxadc_read_batid - read a channel through the shared selector
 * @adc:	AUXADC instance
 * @chan:	channel index
 * @raw:	raw 12 bit conversion result
 *
 * MT6328/MT6351/MT6353 route channels 12..15 through one selector
 * (AUXADC_CON4[15:13]). The three vendor battery-ID inputs sit on channel 12
 * of the first slots, so reading the ladder means selecting a slot and then
 * converting channel 12.
 */
int mediatek_pmic_auxadc_read_batid(struct mediatek_pmic_auxadc *adc,
				    unsigned int chan, u32 *raw)
{
	unsigned int slot;
	u32 keep;
	int ret;

	if (IS_ERR_OR_NULL(adc))
		return adc ? PTR_ERR(adc) : -ENODEV;
	if (chan < MTK_PMIC_AUXADC_CHAN_BATID)
		return -EINVAL;

	slot = min(chan - MTK_PMIC_AUXADC_CHAN_BATID, 3u);

	guard(mutex)(&adc->lock);

	ret = mp_power_on_locked(adc);
	if (ret)
		return ret;

	ret = regmap_read(adc->regmap,
			  mp_reg(adc, MTK_PMIC_AUXADC_REG_AUXADC_CON4), &keep);
	if (ret)
		return ret;

	ret = regmap_update_bits(adc->regmap,
				 mp_reg(adc, MTK_PMIC_AUXADC_REG_AUXADC_CON4),
				 AUXADC_CON4_CHSEL_12_15,
				 FIELD_PREP(AUXADC_CON4_CHSEL_12_15, slot));
	if (ret)
		return ret;

	ret = mp_read_chan_locked(adc, MTK_PMIC_AUXADC_CHAN_BATID, raw);

	regmap_write(adc->regmap, mp_reg(adc, MTK_PMIC_AUXADC_REG_AUXADC_CON4),
		     keep);

	return ret;
}
EXPORT_SYMBOL_GPL(mediatek_pmic_auxadc_read_batid);

/**
 * mediatek_pmic_auxadc_read_ptim - impedance measurement (voltage + current)
 * @adc:	AUXADC instance
 * @vbat:	battery voltage in mV (may be NULL)
 * @ibat:	battery current in mA, positive when discharging (may be NULL)
 *
 * The MT6328/MT6351/MT6353 run a hardware state machine that samples BATSNS
 * and ISENSE back to back over the internal sense resistor, so both samples
 * belong to the same load state. The 15 bit result carries the voltage code
 * in bits 14:5 and the current code in bits 4:0.
 */
int mediatek_pmic_auxadc_read_ptim(struct mediatek_pmic_auxadc *adc,
				   u32 *vbat, s32 *ibat)
{
	u16 imp0 = mp_reg(adc, MTK_PMIC_AUXADC_REG_AUXADC_IMP0);
	u16 imp1 = mp_reg(adc, MTK_PMIC_AUXADC_REG_AUXADC_IMP1);
	u32 val, limit;
	int ret;

	if (IS_ERR_OR_NULL(adc))
		return adc ? PTR_ERR(adc) : -ENODEV;

	guard(mutex)(&adc->lock);

	ret = mp_power_on_locked(adc);
	if (ret)
		return ret;

	ret = regmap_write(adc->regmap, imp0,
			   AUXADC_IMP0_MODE | AUXADC_IMP0_IRQ_STATUS);
	if (ret)
		return ret;

	usleep_range(AUXADC_IMPD_DELAY_US, AUXADC_IMPD_DELAY_US + 1000);

	ret = regmap_write(adc->regmap, imp0,
			   AUXADC_IMP0_MODE | AUXADC_IMP0_TRIG_CNT_CLEAR);
	if (ret)
		return ret;

	ret = regmap_write(adc->regmap, imp0, AUXADC_IMP0_MODE);
	if (ret)
		return ret;

	ret = regmap_write(adc->regmap, imp1, 0);
	if (ret)
		return ret;

	ret = regmap_write(adc->regmap, imp0,
			   AUXADC_IMP0_MODE | AUXADC_IMP0_TRIG_IMP);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(adc->regmap, imp1, val, val & AUXADC_IMP1_RDY,
				       AUXADC_POLL_INTERVAL_US,
				       AUXADC_IMP_TIMEOUT_US);
	if (ret)
		return ret;

	val &= AUXADC_IMP1_DATA_MASK;
	limit = DIV_ROUND_CLOSEST(adc->data->isense_r_bat * 5, 100000);

	if (vbat)
		*vbat = DIV_ROUND_CLOSEST((val >> 5) * 1800 * 3, 4096);

	if (ibat)
		*ibat = limit == 0 ? 0 :
			FIELD_GET(GENMASK(4, 0), val) * 4000 / limit;

	return 0;
}
EXPORT_SYMBOL_GPL(mediatek_pmic_auxadc_read_ptim);

MODULE_DESCRIPTION("MediaTek PMIC AUXADC helper library");
MODULE_LICENSE("GPL");
