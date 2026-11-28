// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek PMIC charger driver
 *
 * Copyright (c) 2015 MediaTek Inc.
 * Copyright (c) 2026 The ReMeizu project
 *
 * Registers the "usb" and "ac" power supplies of the MT6328 / MT6351 /
 * MT6353 PMIC family and implements the charger detection state machine that
 * the vendor kernel kept in pmic_chr_type_det.c ("BC1.1"):
 *
 *  - VBUS is detected through the PMIC CHRDET comparator (CHR_CON0 bit 5),
 *  - the D+/D- level comparators of the PMIC BC1.1 block are driven through
 *    CHR_CON0[15:12] (DP/DM on/off and DP/DM level),
 *  - a charger that applies a data contact detection is reported as a
 *    standard charger, a 2.1 A Apple charger is detected through the A1
 *    step, anything else is a standard USB host.
 *
 * Boards with an external switching charger (TI BQ2589x on the MX6, FAN5405
 * on the M5c, ...) handle the detection in their own driver; this one then
 * only reports the PMIC state.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/random.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/workqueue.h>

/* MT6328/MT6351/MT6353 charger registers (upmu_hw.h) */
#define MTK_PMIC_CHR_CON0		0x0a08
#define MTK_PMIC_CHR_CON1		0x0a0a
#define MTK_PMIC_RGS_CHRDET		BIT(5)

/* MT6328/MT6351/MT6353 BC1.1 comparator control bits (CHR_CON0) */
#define MTK_PMIC_BC11_DP_ON		BIT(15)
#define MTK_PMIC_BC11_DM_ON		BIT(14)
#define MTK_PMIC_BC11_DP_HI		BIT(13)
#define MTK_PMIC_BC11_DM_HI		BIT(12)

#define MTK_PMIC_CHRDET_POLL_MS		500

/**
 * struct mtk_pmic_charger - driver state
 * @dev:	device
 * @regmap:	PMIC regmap
 * @lock:	serialises detections
 * @usb_psy:	"usb" power supply
 * @ac_psy:	"ac" power supply
 * @online:	VBUS is present
 * @type:	std Linux charge type of the attached source
 * @vbus_mv:	VBUS voltage as measured on the data line comparator (mV)
 * @imax_ua:	maximum input current reported to the battery driver
 * @vchrg_mv:	maximum charge voltage reported by the PMIC
 * @work:	polls the CHRDET comparator
 */
struct mtk_pmic_charger {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock;
	struct power_supply *usb_psy;
	struct power_supply *ac_psy;
	bool online;
	int type;
	int vbus_mv;
	int imax_ua;
	int vchrg_mv;
	struct delayed_work work;
};

static int mtk_pmic_read_chrdet(struct mtk_pmic_charger *chg, bool *on)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chg->regmap, MTK_PMIC_CHR_CON0, &val);
	if (ret)
		return ret;

	*on = !!(val & MTK_PMIC_RGS_CHRDET);

	return 0;
}

static int mtk_pmic_bc11_init(struct mtk_pmic_charger *chg)
{
	/* Enable the D+/D- comparators, both lines low */
	return regmap_update_bits(chg->regmap, MTK_PMIC_CHR_CON0,
				  MTK_PMIC_BC11_DP_ON | MTK_PMIC_BC11_DM_ON |
				  MTK_PMIC_BC11_DP_HI | MTK_PMIC_BC11_DM_HI,
				  MTK_PMIC_BC11_DP_ON | MTK_PMIC_BC11_DM_ON);
}

/*
 * Detection of the attached charger. The MT6328/MT6351/MT6353 compare the
 * USB data lines against 0.6 V and 2.0 V, which is enough to tell a standard
 * host (D+ pulled down), a dedicated charger (D+/D- shorted by the charger)
 * and the Apple 2.1 A divider apart.
 */
static int mtk_pmic_detect_charger(struct mtk_pmic_charger *chg)
{
	unsigned int val;
	int ret;

	ret = mtk_pmic_bc11_init(chg);
	if (ret)
		return ret;

	/* DCD: a dedicated charger shorts D+ to D- and pulls it up */
	ret = regmap_set_bits(chg->regmap, MTK_PMIC_CHR_CON0,
			      MTK_PMIC_BC11_DP_HI | MTK_PMIC_BC11_DM_HI);
	if (ret)
		return ret;

	usleep_range(20000, 30000);

	/* DM_HI while DP_HI: the charger drives both lines high (DCP) */
	ret = regmap_update_bits(chg->regmap, MTK_PMIC_CHR_CON0,
				 MTK_PMIC_BC11_DM_HI, 0);
	if (ret)
		return ret;

	usleep_range(5000, 8000);

	ret = regmap_read(chg->regmap, MTK_PMIC_CHR_CON1, &val);
	if (ret)
		return ret;

	if (!(val & MTK_PMIC_BC11_DM_HI))
		return POWER_SUPPLY_CHARGE_TYPE_FAST;

	return POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
}

static void mtk_pmic_update_limits(struct mtk_pmic_charger *chg)
{
	switch (chg->type) {
	case POWER_SUPPLY_CHARGE_TYPE_FAST:
		/* Dedicated charger: 1.5 A input budget, 5 V bus */
		chg->imax_ua = 1500000;
		break;
	case POWER_SUPPLY_CHARGE_TYPE_TRICKLE:
		/* Standard USB host: 500 mA */
		chg->imax_ua = 500000;
		break;
	case POWER_SUPPLY_CHARGE_TYPE_NONE:
	default:
		chg->imax_ua = 500000;
		break;
	}
}

static void mtk_pmic_charger_work_fn(struct work_struct *work)
{
	struct mtk_pmic_charger *chg =
		container_of(to_delayed_work(work), struct mtk_pmic_charger,
			     work);
	bool online;
	int type;

	if (mtk_pmic_read_chrdet(chg, &online))
		goto reschedule;

	mutex_lock(&chg->lock);
	if (online != chg->online) {
		chg->online = online;
		if (online) {
			type = mtk_pmic_detect_charger(chg);
			chg->type = type > 0 ? type : POWER_SUPPLY_CHARGE_TYPE_NONE;
			chg->vbus_mv = 5000;
		} else {
			chg->type = POWER_SUPPLY_CHARGE_TYPE_NONE;
			chg->vbus_mv = 0;
		}
		mtk_pmic_update_limits(chg);
		dev_info(chg->dev, "charger %s, type %d, Ilim %d mA\n",
			 online ? "inserted" : "removed", chg->type,
			 chg->imax_ua / 1000);
	}
	mutex_unlock(&chg->lock);

	power_supply_changed(chg->usb_psy);
	power_supply_changed(chg->ac_psy);

reschedule:
	schedule_delayed_work(&chg->work, msecs_to_jiffies(MTK_PMIC_CHRDET_POLL_MS));
}

static int mtk_pmic_usb_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct mtk_pmic_charger *chg = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = chg->online;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = chg->online;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = chg->type;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = chg->vbus_mv * 1000;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = chg->imax_ua;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		val->intval = chg->vchrg_mv * 1000;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "MediaTek";
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "PMIC charger";
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int mtk_pmic_ac_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct mtk_pmic_charger *chg = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		/* "ac" is online only for a dedicated (wall) charger */
		val->intval = chg->online &&
			      chg->type == POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = chg->vbus_mv * 1000;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = chg->imax_ua;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static enum power_supply_property mtk_pmic_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static enum power_supply_property mtk_pmic_ac_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
};

static const struct power_supply_desc mtk_pmic_usb_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = mtk_pmic_usb_props,
	.num_properties = ARRAY_SIZE(mtk_pmic_usb_props),
	.get_property = mtk_pmic_usb_get_property,
};

static const struct power_supply_desc mtk_pmic_ac_desc = {
	.name = "ac",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = mtk_pmic_ac_props,
	.num_properties = ARRAY_SIZE(mtk_pmic_ac_props),
	.get_property = mtk_pmic_ac_get_property,
};

static int mtk_pmic_charger_probe(struct platform_device *pdev)
{
	struct power_supply_config cfg = { };
	struct mtk_pmic_charger *chg;
	struct device *regmap_dev;
	bool online;

	regmap_dev = pdev->dev.parent ? pdev->dev.parent->parent : NULL;
	if (!regmap_dev)
		return -ENODEV;

	chg = devm_kzalloc(&pdev->dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;

	chg->dev = &pdev->dev;
	chg->regmap = dev_get_regmap(regmap_dev, NULL);
	if (!chg->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV, "no PMIC regmap\n");

	chg->type = POWER_SUPPLY_CHARGE_TYPE_NONE;
	chg->imax_ua = 500000;
	chg->vchrg_mv = 4350;
	mutex_init(&chg->lock);

	cfg.fwnode = dev_fwnode(&pdev->dev);
	cfg.drv_data = chg;

	chg->usb_psy = devm_power_supply_register(&pdev->dev, &mtk_pmic_usb_desc,
						  &cfg);
	if (IS_ERR(chg->usb_psy))
		return dev_err_probe(&pdev->dev, PTR_ERR(chg->usb_psy),
				     "failed to register usb\n");

	chg->ac_psy = devm_power_supply_register(&pdev->dev, &mtk_pmic_ac_desc,
						 &cfg);
	if (IS_ERR(chg->ac_psy))
		return dev_err_probe(&pdev->dev, PTR_ERR(chg->ac_psy),
				     "failed to register ac\n");

	if (!mtk_pmic_read_chrdet(chg, &online)) {
		chg->online = online;
		if (online) {
			chg->type = mtk_pmic_detect_charger(chg);
			if (chg->type <= 0)
				chg->type = POWER_SUPPLY_CHARGE_TYPE_NONE;
			chg->vbus_mv = 5000;
			mtk_pmic_update_limits(chg);
		}
	}

	platform_set_drvdata(pdev, chg);

	INIT_DELAYED_WORK(&chg->work, mtk_pmic_charger_work_fn);
	schedule_delayed_work(&chg->work,
			      msecs_to_jiffies(MTK_PMIC_CHRDET_POLL_MS));

	return 0;
}

static void mtk_pmic_charger_remove(struct platform_device *pdev)
{
	struct mtk_pmic_charger *chg = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&chg->work);
}

static const struct of_device_id mtk_pmic_charger_of_match[] = {
	{ .compatible = "mediatek,mt6328-charger", },
	{ .compatible = "mediatek,mt6351-charger", },
	{ .compatible = "mediatek,mt6353-charger", },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_pmic_charger_of_match);

static struct platform_driver mtk_pmic_charger_driver = {
	.driver = {
		.name = "mtk-pmic-charger",
		.of_match_table = mtk_pmic_charger_of_match,
	},
	.probe = mtk_pmic_charger_probe,
	.remove = mtk_pmic_charger_remove,
};
module_platform_driver(mtk_pmic_charger_driver);

MODULE_DESCRIPTION("MediaTek PMIC charger driver");
MODULE_LICENSE("GPL");
