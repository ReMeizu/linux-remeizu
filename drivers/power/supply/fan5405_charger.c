// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fairchild FAN5405 / FAN5400 switching battery charger driver
 *
 * Copyright (c) 2015 MediaTek Inc.
 * Copyright (c) 2026 The ReMeizu project
 *
 * Register map (CON0..CON6) and voltage/current encodings follow the
 * Fairchild FAN5405 datasheet and the MediaTek MT6735 BSP driver used on
 * the Meizu M5c (I2C1 @ 0x6a).
 */

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

/* FAN5405 registers */
#define FAN5405_REG_CON0		0x00
#define FAN5405_REG_CON1		0x01
#define FAN5405_REG_CON2		0x02
#define FAN5405_REG_CON3		0x03
#define FAN5405_REG_CON4		0x04
#define FAN5405_REG_CON5		0x05
#define FAN5405_REG_CON6		0x06

/* CON0: control / status */
#define FAN5405_CON0_TMR_RST		BIT(7)
#define FAN5405_CON0_EN_STAT		BIT(6)
#define FAN5405_CON0_STAT_MASK		GENMASK(5, 4)
#define FAN5405_STAT_READY		0
#define FAN5405_STAT_CHARGING		1
#define FAN5405_STAT_DONE		2
#define FAN5405_STAT_FAULT		3
#define FAN5405_CON0_BOOST		BIT(3)
#define FAN5405_CON0_FAULT_MASK		GENMASK(2, 0)

/* CON1: input current limit, weak battery voltage, charge enable */
#define FAN5405_CON1_IINLIM_MASK	GENMASK(7, 6)
#define FAN5405_IINLIM_100MA		0
#define FAN5405_IINLIM_500MA		1
#define FAN5405_IINLIM_800MA		2
#define FAN5405_IINLIM_NOLIM		3
#define FAN5405_CON1_VLOWV_MASK		GENMASK(5, 4)
#define FAN5405_CON1_TE			BIT(3)
#define FAN5405_CON1_CE_N		BIT(2)
#define FAN5405_CON1_HZ_MODE		BIT(1)
#define FAN5405_CON1_OPA_MODE		BIT(0)

/* CON2: regulation voltage OREG = 3.50V + step * 20mV */
#define FAN5405_CON2_OREG_MASK		GENMASK(7, 2)
#define FAN5405_OREG_MIN_UV		3500000
#define FAN5405_OREG_MAX_UV		4440000
#define FAN5405_OREG_STEP_UV		20000

/* CON3: IC info */
#define FAN5405_CON3_VENDER_MASK	GENMASK(7, 5)
#define FAN5405_CON3_PN_MASK		GENMASK(4, 3)
#define FAN5405_CON3_REV_MASK		GENMASK(2, 0)

/* CON4: charge current & termination current (with 68 mOhm sense resistor) */
#define FAN5405_CON4_RESET		BIT(7)
#define FAN5405_CON4_IOCHARGE_MASK	GENMASK(6, 4)
#define FAN5405_CON4_ITERM_MASK		GENMASK(2, 0)

/* CON5: special charger voltage & low charge current */
#define FAN5405_CON5_LOW_CHG		BIT(5)
#define FAN5405_CON5_SP_STATUS		BIT(3)
#define FAN5405_CON5_VSP_MASK		GENMASK(2, 0)

/* CON6: safety register (write-once before reset) */
#define FAN5405_CON6_ISAFE_MASK		GENMASK(7, 4)
#define FAN5405_CON6_VSAFE_MASK		GENMASK(3, 0)
#define FAN5405_VSAFE_MIN_UV		4200000
#define FAN5405_VSAFE_MAX_UV		4440000
#define FAN5405_VSAFE_STEP_UV		20000

#define FAN5405_WD_INTERVAL_MS		10000

/* Charge current steps in uA (R_SENSE = 68 mOhm) */
static const u32 fan5405_ichg_ua[] = {
	550000, 650000, 750000, 850000,
	950000, 1050000, 1150000, 1250000,
};

struct fan5405_charger {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct mutex lock;
	struct delayed_work wd_work;

	u32 ichg_ua;
	u32 vreg_uv;
	u32 iinlim_ua;
	bool enabled;
};

static const struct regmap_config fan5405_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = FAN5405_REG_CON6,
};

static int fan5405_find_ichg_idx(u32 ua)
{
	int i;

	for (i = ARRAY_SIZE(fan5405_ichg_ua) - 1; i > 0; i--) {
		if (ua >= fan5405_ichg_ua[i])
			return i;
	}
	return 0;
}

static int fan5405_set_vreg(struct fan5405_charger *chg, u32 uv)
{
	u32 step;

	uv = clamp_t(u32, uv, FAN5405_OREG_MIN_UV, FAN5405_OREG_MAX_UV);
	step = (uv - FAN5405_OREG_MIN_UV) / FAN5405_OREG_STEP_UV;
	chg->vreg_uv = FAN5405_OREG_MIN_UV + step * FAN5405_OREG_STEP_UV;

	return regmap_update_bits(chg->regmap, FAN5405_REG_CON2,
				  FAN5405_CON2_OREG_MASK,
				  FIELD_PREP(FAN5405_CON2_OREG_MASK, step));
}

static int fan5405_set_ichg(struct fan5405_charger *chg, u32 ua)
{
	int idx = fan5405_find_ichg_idx(ua);

	chg->ichg_ua = fan5405_ichg_ua[idx];

	return regmap_update_bits(chg->regmap, FAN5405_REG_CON4,
				  FAN5405_CON4_IOCHARGE_MASK | FAN5405_CON4_RESET,
				  FIELD_PREP(FAN5405_CON4_IOCHARGE_MASK, idx));
}

static int fan5405_set_iinlim(struct fan5405_charger *chg, u32 ua)
{
	u32 code;

	if (ua < 500000) {
		code = FAN5405_IINLIM_100MA;
		chg->iinlim_ua = 100000;
	} else if (ua < 800000) {
		code = FAN5405_IINLIM_500MA;
		chg->iinlim_ua = 500000;
	} else if (ua < 1000000) {
		code = FAN5405_IINLIM_800MA;
		chg->iinlim_ua = 800000;
	} else {
		code = FAN5405_IINLIM_NOLIM;
		chg->iinlim_ua = 1500000;
	}

	return regmap_update_bits(chg->regmap, FAN5405_REG_CON1,
				  FAN5405_CON1_IINLIM_MASK,
				  FIELD_PREP(FAN5405_CON1_IINLIM_MASK, code));
}

static int fan5405_hw_init(struct fan5405_charger *chg)
{
	u32 vsafe_step;
	int ret;

	/*
	 * Program safety register CON6 first: it is write-once after reset
	 * and caps both ISAFE (max 1250 mA) and VSAFE (up to 4.44 V so a
	 * 4.35 V cell is not clamped to 4.20 V).
	 */
	vsafe_step = clamp_t(u32, chg->vreg_uv, FAN5405_VSAFE_MIN_UV,
			     FAN5405_VSAFE_MAX_UV);
	vsafe_step = (vsafe_step - FAN5405_VSAFE_MIN_UV) / FAN5405_VSAFE_STEP_UV;

	ret = regmap_write(chg->regmap, FAN5405_REG_CON6,
			   FIELD_PREP(FAN5405_CON6_ISAFE_MASK, 7) |
			   FIELD_PREP(FAN5405_CON6_VSAFE_MASK, vsafe_step));
	if (ret)
		return ret;

	ret = fan5405_set_vreg(chg, chg->vreg_uv);
	if (ret)
		return ret;

	ret = fan5405_set_ichg(chg, chg->ichg_ua);
	if (ret)
		return ret;

	ret = fan5405_set_iinlim(chg, chg->iinlim_ua);
	if (ret)
		return ret;

	/* Enable charge termination, exit Hi-Z, enable charging (CE_N = 0) */
	ret = regmap_update_bits(chg->regmap, FAN5405_REG_CON1,
				 FAN5405_CON1_TE | FAN5405_CON1_CE_N |
				 FAN5405_CON1_HZ_MODE | FAN5405_CON1_OPA_MODE,
				 FAN5405_CON1_TE);
	if (ret)
		return ret;

	chg->enabled = true;

	/* Enable STAT pin and kick the 32 s safety timer */
	return regmap_update_bits(chg->regmap, FAN5405_REG_CON0,
				  FAN5405_CON0_TMR_RST | FAN5405_CON0_EN_STAT,
				  FAN5405_CON0_TMR_RST | FAN5405_CON0_EN_STAT);
}

static void fan5405_wd_work_fn(struct work_struct *work)
{
	struct fan5405_charger *chg =
		container_of(to_delayed_work(work), struct fan5405_charger,
			     wd_work);

	mutex_lock(&chg->lock);
	regmap_update_bits(chg->regmap, FAN5405_REG_CON0,
			   FAN5405_CON0_TMR_RST, FAN5405_CON0_TMR_RST);
	mutex_unlock(&chg->lock);

	schedule_delayed_work(&chg->wd_work,
			      msecs_to_jiffies(FAN5405_WD_INTERVAL_MS));
}

static int fan5405_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct fan5405_charger *chg = power_supply_get_drvdata(psy);
	unsigned int con0;
	u32 stat, fault;
	int ret;

	mutex_lock(&chg->lock);
	ret = regmap_read(chg->regmap, FAN5405_REG_CON0, &con0);
	mutex_unlock(&chg->lock);
	if (ret)
		return ret;

	stat = FIELD_GET(FAN5405_CON0_STAT_MASK, con0);
	fault = FIELD_GET(FAN5405_CON0_FAULT_MASK, con0);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = (stat == FAN5405_STAT_CHARGING ||
			       stat == FAN5405_STAT_DONE);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		switch (stat) {
		case FAN5405_STAT_CHARGING:
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
			break;
		case FAN5405_STAT_DONE:
			val->intval = POWER_SUPPLY_STATUS_FULL;
			break;
		case FAN5405_STAT_READY:
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
			break;
		default:
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			break;
		}
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		if (stat == FAN5405_STAT_CHARGING)
			val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
		else
			val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		switch (fault) {
		case 0:
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
			break;
		case 1:
			val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
			break;
		case 5:
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
			break;
		case 6:
			val->intval = POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
			break;
		default:
			val->intval = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
			break;
		}
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = chg->ichg_ua;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		val->intval = chg->vreg_uv;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = chg->iinlim_ua;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Fairchild";
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "FAN5405";
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int fan5405_set_property(struct power_supply *psy,
				enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct fan5405_charger *chg = power_supply_get_drvdata(psy);
	int ret;

	mutex_lock(&chg->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = fan5405_set_ichg(chg, val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		ret = fan5405_set_vreg(chg, val->intval);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = fan5405_set_iinlim(chg, val->intval);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&chg->lock);

	return ret;
}

static int fan5405_property_is_writeable(struct power_supply *psy,
					 enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return 1;
	default:
		return 0;
	}
}

static enum power_supply_property fan5405_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static const struct power_supply_desc fan5405_psy_desc = {
	.name = "fan5405-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = fan5405_props,
	.num_properties = ARRAY_SIZE(fan5405_props),
	.get_property = fan5405_get_property,
	.set_property = fan5405_set_property,
	.property_is_writeable = fan5405_property_is_writeable,
};

static void fan5405_cancel_wd(void *data)
{
	struct fan5405_charger *chg = data;

	cancel_delayed_work_sync(&chg->wd_work);
}

static int fan5405_probe(struct i2c_client *client)
{
	struct power_supply_config psy_cfg = { };
	struct device *dev = &client->dev;
	struct fan5405_charger *chg;
	unsigned int ic_info;
	int ret;

	chg = devm_kzalloc(dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;

	chg->dev = dev;
	mutex_init(&chg->lock);

	chg->regmap = devm_regmap_init_i2c(client, &fan5405_regmap_config);
	if (IS_ERR(chg->regmap))
		return dev_err_probe(dev, PTR_ERR(chg->regmap),
				     "failed to init regmap\n");

	ret = regmap_read(chg->regmap, FAN5405_REG_CON3, &ic_info);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read IC info\n");

	chg->vreg_uv = 4350000;
	chg->ichg_ua = 1050000;
	chg->iinlim_ua = 1000000;

	device_property_read_u32(dev, "constant-charge-voltage-max-microvolt",
				 &chg->vreg_uv);
	device_property_read_u32(dev, "constant-charge-current-max-microamp",
				 &chg->ichg_ua);
	device_property_read_u32(dev, "input-current-limit-microamp",
				 &chg->iinlim_ua);

	ret = fan5405_hw_init(chg);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize FAN5405\n");

	psy_cfg.fwnode = dev_fwnode(dev);
	psy_cfg.drv_data = chg;

	chg->psy = devm_power_supply_register(dev, &fan5405_psy_desc, &psy_cfg);
	if (IS_ERR(chg->psy))
		return dev_err_probe(dev, PTR_ERR(chg->psy),
				     "failed to register power supply\n");

	INIT_DELAYED_WORK(&chg->wd_work, fan5405_wd_work_fn);
	ret = devm_add_action_or_reset(dev, fan5405_cancel_wd, chg);
	if (ret)
		return ret;

	schedule_delayed_work(&chg->wd_work,
			      msecs_to_jiffies(FAN5405_WD_INTERVAL_MS));

	dev_info(dev, "FAN5405 charger probed (IC info 0x%02x, Vreg %u mV, Ichg %u mA)\n",
		 ic_info, chg->vreg_uv / 1000, chg->ichg_ua / 1000);

	return 0;
}

static const struct i2c_device_id fan5405_i2c_ids[] = {
	{ "fan5405" },
	{ "fan5400" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fan5405_i2c_ids);

static const struct of_device_id fan5405_of_match[] = {
	{ .compatible = "fairchild,fan5405" },
	{ .compatible = "fairchild,fan5400" },
	{ .compatible = "mediatek,swithing_charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, fan5405_of_match);

static struct i2c_driver fan5405_driver = {
	.driver = {
		.name = "fan5405-charger",
		.of_match_table = fan5405_of_match,
	},
	.probe = fan5405_probe,
	.id_table = fan5405_i2c_ids,
};
module_i2c_driver(fan5405_driver);

MODULE_DESCRIPTION("Fairchild FAN5405 battery charger driver");
MODULE_LICENSE("GPL");
