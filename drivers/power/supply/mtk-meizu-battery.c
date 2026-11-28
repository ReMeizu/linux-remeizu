// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek/Meizu fuel gauge and battery driver
 *
 * Copyright (c) 2015 MediaTek Inc.
 * Copyright (c) 2026 The ReMeizu project
 *
 * This driver replaces the vendor "battery_meter"/"smart battery" stack of
 * the MediaTek 3.18/4.9 kernels for the Meizu boards built on MT6735,
 * MT6755 and MT6797. It exposes the standard Linux "battery" power_supply and
 * computes state of charge, voltage, current and temperature from:
 *
 *  - the PMIC auxiliary ADC (battery voltage, battery NTC, battery ID),
 *  - the PMIC FGADC coulomb counter when the PMIC provides one,
 *  - the board battery profiles (OCV and internal resistance tables),
 *  - the charger's input current limit and voltage limit.
 *
 * The algorithm mirrors the MediaTek BSP:
 *   - the pack thermistor is converted with the NTC table and the board's
 *     pull-up resistor,
 *   - the open-circuit voltage is estimated from the measured voltage plus
 *     the IR drop over the internal resistance at the present current,
 *   - the state of charge is interpolated from the OCV table that belongs to
 *     the closest profile temperature,
 *   - the result is tracked with the coulomb counter when one is available.
 */

#include <linux/delay.h>
#include <linux/iio/adc/mediatek-pmic-auxadc.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/power_supply/mtk-meizu-battery-profiles.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/workqueue.h>

/* Battery ID channels of the MT6328/MT6351/MT6353 PMIC AUXADC */
#define MTK_MEIZU_BATID_CHANNEL		12

/* Averaging window used for the voltage/current samples */
#define MTK_MEIZU_ADC_SAMPLES		8

/*
 * The PMIC scales the battery-sense and current-sense inputs by 3 before the
 * 12 bit converter (VSENSE/ISENSE are divided by 3 on the package), so one
 * LSB of channel 0/1/13 is 3 * 1.8 V / 4096.
 */
#define MTK_MEIZU_AUXADC_VREF_MV	1800
#define MTK_MEIZU_AUXADC_FULLSCALE	4096

/* The pack thermistor is a 10 kOhm NTC (BAT_NTC_10 in the stock tables) */
#define MTK_MEIZU_NTC_NOMINAL_OHM	10000

/*
 * Board defaults of the battery NTC divider. The bootloader leaves the PMIC
 * VBUF at 1.8 V; the pull-up is the one of the stock DTS
 * ("VBAT_NTC = <R>"), 24 kOhm on the BSP reference designs.
 */
#define MTK_MEIZU_NTC_PULLUP_OHM	24000
#define MTK_MEIZU_NTC_BIAS_MV		1800

/* State of charge limits applied when the voltage is out of the OCV range */
#define MTK_MEIZU_SOC_MAX		100
#define MTK_MEIZU_SOC_MIN		0

enum mtk_meizu_batt_temp_profile {
	MTK_MEIZU_PROFILE_T0 = 0,	/* -10 C */
	MTK_MEIZU_PROFILE_T1,		/* 0 C */
	MTK_MEIZU_PROFILE_T2,		/* 25 C */
	MTK_MEIZU_PROFILE_T3,		/* 50 C */
	MTK_MEIZU_PROFILE_MAX,
};

static const int mtk_meizu_profile_temp[MTK_MEIZU_PROFILE_MAX] = {
	-10, 0, 25, 50
};

/**
 * struct mtk_meizu_batt - driver state
 * @dev:		device
 * @psy:		"battery" power supply
 * @tables:		battery profile set
 * @auxadc:		PMIC AUXADC helper (voltage, temperature, battery ID)
 * @charger:		charger power supply (optional)
 * @lock:		protects the cached measurements
 * @vbat_mv:		last battery voltage in mV
 * @ibat_ua:		last battery current in uA, positive when discharging
 * @temp_mc:		last battery temperature in mdegC
 * @soc:		last state of charge in percent
 * @ocv_mv:		last estimated open-circuit voltage in mV
 * @rmt_mohm:		last internal resistance in mOhm
 * @batid_mv:		battery ID ladder voltage in mV
 * @batid_valid:	the ladder has been measured
 * @qmax_mah:		capacity used for the coulomb counting
 * @cc_rest_ua:		coulomb counter residue of the last update
 * @cc_valid:		the coulomb counter has been seeded
 * @chg_online:		cached charger "online" state
 * @chg_type:		cached charger charge type
 * @last_update:	last update timestamp in jiffies
 * @work:		periodic update work
 * @nb:			notifier of the charger power supply
 * @vmin_mv:		design minimum voltage
 * @vmax_mv:		design maximum voltage
 * @i_max_ua:		maximum charge current
 * @pullup_ohm:		battery NTC pull-up resistor
 * @pullup_volt_mv:	battery NTC bias voltage
 * @ptim_ok:		the PMIC impedance measurement works
 */
struct mtk_meizu_batt {
	struct device *dev;
	struct power_supply *psy;
	const struct mtk_meizu_batt_tables *tables;
	struct mediatek_pmic_auxadc *auxadc;
	struct power_supply *charger;

	struct mutex lock;
	int vbat_mv;
	int ibat_ua;
	int temp_mc;
	int soc;
	int ocv_mv;
	int rmt_mohm;
	int batid_mv;
	bool batid_valid;
	int qmax_mah;
	s64 cc_rest_ua;
	bool cc_valid;
	bool chg_online;
	int chg_type;
	unsigned long last_update;

	struct delayed_work work;
	struct notifier_block nb;

	int vmin_mv;
	int vmax_mv;
	int i_max_ua;
	int pullup_ohm;
	int pullup_volt_mv;
	bool ptim_ok;
};

static int mtk_meizu_ntc_to_mcelsius(const struct mtk_meizu_batt *batt, int mv)
{
	const struct mtk_meizu_ntc_pt *tab = batt->tables->ntc;
	int len = batt->tables->ntc_len;
	int pullup_mv = batt->pullup_volt_mv;
	int pullup_ohm = batt->pullup_ohm;
	s64 span, frac;
	int now_ohm, i;

	if (len < 2 || pullup_ohm <= 0 || pullup_mv <= 0 || mv <= 0)
		return INT_MIN;

	/*
	 * The pack thermistor is the low leg of a divider (VBAT_NTC in the
	 * stock DTS):
	 *	V = VBIAS * Rntc / (Rntc + Rpull)
	 * So Rntc = Rpull * V / (VBIAS - V).
	 */
	if (mv >= pullup_mv)
		return tab[len - 1].temp_c * 1000;

	now_ohm = div_s64((s64)mv * pullup_ohm, pullup_mv - mv);

	/* The table is ordered from hot (small R) to cold (large R) */
	if (now_ohm >= tab[0].ohm)
		return tab[0].temp_c * 1000;
	if (now_ohm <= tab[len - 1].ohm)
		return tab[len - 1].temp_c * 1000;

	for (i = 0; i < len - 1; i++)
		if (now_ohm <= tab[i].ohm && now_ohm >= tab[i + 1].ohm)
			break;

	span = tab[i].ohm - tab[i + 1].ohm;
	frac = (s64)(tab[i].ohm - now_ohm) *
	       (tab[i + 1].temp_c - tab[i].temp_c);

	return tab[i].temp_c * 1000 + div_s64(frac * 1000, span);
}

static int mtk_meizu_temp_profile(int temp_mc)
{
	int i, best = 0, diff, best_diff = INT_MAX;

	for (i = 0; i < MTK_MEIZU_PROFILE_MAX; i++) {
		diff = abs(temp_mc - mtk_meizu_profile_temp[i] * 1000);
		if (diff < best_diff) {
			best_diff = diff;
			best = i;
		}
	}

	return best;
}

/*
 * Interpolate a MediaTek OCV (DOD vs. mV) or R (mOhm vs. mV) table.
 * Tables are ordered by descending voltage; trailing zero-padded slots (as in
 * the stock M5c SUNWODA table) are ignored.
 */
static int mtk_meizu_table_lookup(const struct mtk_meizu_batt_pt *tab, int len,
				  int mv, bool x_is_dod)
{
	int lo = 0, hi = len - 1, mid;
	s64 dv, res;

	while (hi > 0 && tab[hi].mv == 0)
		hi--;

	if (mv >= tab[0].mv)
		res = tab[0].x;
	else if (mv <= tab[hi].mv)
		res = tab[hi].x;
	else {
		while (hi - lo > 1) {
			mid = (lo + hi) / 2;
			if (tab[mid].mv > mv)
				lo = mid;
			else
				hi = mid;
		}

		dv = tab[lo].mv - tab[hi].mv;
		if (dv == 0)
			res = tab[lo].x;
		else
			res = tab[lo].x + div_s64((s64)(tab[hi].x - tab[lo].x) *
						  (tab[lo].mv - mv), dv);
	}

	/* MediaTek OCV tables store depth-of-discharge (0 = full, 100 = empty) */
	if (x_is_dod)
		return clamp_t(int, 100 - (int)res, MTK_MEIZU_SOC_MIN,
			       MTK_MEIZU_SOC_MAX);

	return (int)res;
}

static void mtk_meizu_read_sensors(struct mtk_meizu_batt *batt)
{
	u32 raw;
	s32 ibat = 0;
	int i, n, sum_mv, sum_ibat;
	int mv;

	/* Battery voltage, BATSNS channel 0 (3x divider, 12 bits, 1.8 V ref) */
	sum_mv = 0;
	n = 0;
	for (i = 0; i < MTK_MEIZU_ADC_SAMPLES; i++) {
		if (mediatek_pmic_auxadc_read(batt->auxadc,
					      MTK_PMIC_AUXADC_CHAN_BATSNS, &raw))
			break;
		sum_mv += DIV_ROUND_CLOSEST(raw * 1800 * 3, 4096);
		n++;
	}
	if (n)
		batt->vbat_mv = sum_mv / n;

	/* Battery current from the impedance measurement (FGADC) */
	sum_ibat = 0;
	n = 0;
	for (i = 0; i < 2; i++) {
		if (mediatek_pmic_auxadc_read_ptim(batt->auxadc, NULL, &ibat))
			break;
		sum_ibat += ibat;
		n++;
	}
	if (n) {
		/* mediatek_pmic_auxadc_read_ptim() reports mA */
		ibat = sum_ibat / n * 1000;
		batt->ibat_ua = (batt->ibat_ua * 3 + ibat) / 4;
		batt->ptim_ok = true;
	}

	/* Battery pack thermistor, channel 3 (gain 1) */
	sum_mv = 0;
	n = 0;
	for (i = 0; i < MTK_MEIZU_ADC_SAMPLES; i++) {
		if (mediatek_pmic_auxadc_read(batt->auxadc,
					      MTK_PMIC_AUXADC_CHAN_BATTEMP, &raw))
			break;
		sum_mv += DIV_ROUND_CLOSEST(raw * MTK_MEIZU_AUXADC_VREF_MV,
					    MTK_MEIZU_AUXADC_FULLSCALE);
		n++;
	}
	if (n) {
		mv = mtk_meizu_ntc_to_mcelsius(batt, sum_mv / n);
		/* A missing pack thermistor reads as 0 V */
		if (mv != INT_MIN)
			batt->temp_mc = mv;
	}

	/* Battery ID ladder, channel 12 (only needed to pick a profile) */
	if (!batt->batid_valid) {
		sum_mv = 0;
		n = 0;
		for (i = 0; i < 2; i++) {
			if (mediatek_pmic_auxadc_read_batid(batt->auxadc,
							    MTK_MEIZU_BATID_CHANNEL,
							    &raw))
				break;
			sum_mv += DIV_ROUND_CLOSEST(raw * MTK_MEIZU_AUXADC_VREF_MV,
						    MTK_MEIZU_AUXADC_FULLSCALE);
			n++;
		}
		if (n) {
			batt->batid_mv = sum_mv / n;
			batt->batid_valid = true;
		}
	}
}

/*
 * Sample the charger state. This is only called from the update work and the
 * charger notifier, never from get_property(), so power_supply_get_property()
 * can take the charger's own locks without nesting ours.
 */
static void mtk_meizu_poll_charger(struct mtk_meizu_batt *batt)
{
	union power_supply_propval pval = { 0 };
	bool online = false;
	int type = POWER_SUPPLY_CHARGE_TYPE_NONE;

	if (batt->charger) {
		if (!power_supply_get_property(batt->charger,
					       POWER_SUPPLY_PROP_ONLINE, &pval))
			online = pval.intval;

		if (online &&
		    !power_supply_get_property(batt->charger,
					       POWER_SUPPLY_PROP_CHARGE_TYPE, &pval))
			type = pval.intval;
	}

	mutex_lock(&batt->lock);
	batt->chg_online = online;
	batt->chg_type = type;
	mutex_unlock(&batt->lock);
}

static void mtk_meizu_update(struct mtk_meizu_batt *batt)
{
	const struct mtk_meizu_batt_tables *t = batt->tables;
	const struct mtk_meizu_batt_profile *p;
	unsigned long now = jiffies;
	s64 d_ua;
	int prof, soc, rmt, ocv;

	mtk_meizu_read_sensors(batt);

	prof = mtk_meizu_temp_profile(batt->temp_mc);
	p = &t->profiles[prof];

	rmt = mtk_meizu_table_lookup(p->r, t->points, batt->vbat_mv, false);
	if (rmt <= 0)
		rmt = 100;
	batt->rmt_mohm = rmt;

	/* OCV = VBAT + I * R (I is positive while discharging) */
	ocv = batt->vbat_mv + DIV_ROUND_CLOSEST((s64)batt->ibat_ua * rmt, 1000000);
	batt->ocv_mv = ocv;

	soc = mtk_meizu_table_lookup(p->ocv, t->points, ocv, true);
	soc = clamp(soc, MTK_MEIZU_SOC_MIN, MTK_MEIZU_SOC_MAX);

	/* Coulomb counting: integrate the current between two updates */
	if (batt->cc_valid && batt->qmax_mah > 0) {
		d_ua = (s64)batt->ibat_ua *
		       jiffies_to_msecs(now - batt->last_update);
		batt->cc_rest_ua += d_ua;
	}

	batt->last_update = now;
	batt->cc_valid = true;

	/*
	 * Track the OCV result while the accumulated charge stays below 0.5 %
	 * of the pack capacity, then fold it into the state of charge.
	 */
	if (batt->qmax_mah > 0) {
		s64 limit = (s64)batt->qmax_mah * 3600000 / 200;

		if (batt->cc_rest_ua > limit || batt->cc_rest_ua < -limit) {
			s64 d_soc = div_s64(batt->cc_rest_ua * 100,
					    (s64)batt->qmax_mah * 3600000);

			batt->soc = clamp(soc + (int)d_soc, MTK_MEIZU_SOC_MIN,
					  MTK_MEIZU_SOC_MAX);
			batt->cc_rest_ua = 0;
		} else {
			/* Blend: keep the previous value, it is smoothed */
			batt->soc = (batt->soc * 3 + soc) / 4;
		}
	} else {
		batt->soc = soc;
	}
}

static int mtk_meizu_batt_init_state(struct mtk_meizu_batt *batt)
{
	mutex_lock(&batt->lock);
	mtk_meizu_update(batt);
	mutex_unlock(&batt->lock);

	return 0;
}

static int mtk_meizu_get_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct mtk_meizu_batt *batt = power_supply_get_drvdata(psy);
	int ret;

	mutex_lock(&batt->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (!batt->chg_online)
			val->intval = batt->soc <= 0 ?
				POWER_SUPPLY_STATUS_NOT_CHARGING :
				POWER_SUPPLY_STATUS_DISCHARGING;
		else if (batt->soc >= MTK_MEIZU_SOC_MAX)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = batt->chg_online ? batt->chg_type :
			POWER_SUPPLY_CHARGE_TYPE_NONE;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = batt->vbat_mv > 2500;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = batt->vmax_mv * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = batt->vmin_mv * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = batt->vbat_mv * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		val->intval = batt->ocv_mv * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = batt->ibat_ua;
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		val->intval = batt->ibat_ua;
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
		val->intval = div_s64((s64)batt->ibat_ua * batt->vbat_mv, 1000);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = batt->temp_mc / 100;
		break;
	case POWER_SUPPLY_PROP_TEMP_ALERT_MIN:
		val->intval = -200;
		break;
	case POWER_SUPPLY_PROP_TEMP_ALERT_MAX:
		val->intval = 600;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = batt->soc;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		if (batt->soc >= 95)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		else if (batt->soc >= 70)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
		else if (batt->soc >= 20)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		else if (batt->soc > 0)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		else
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		if (batt->temp_mc >= 60000 || batt->temp_mc <= -20000)
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		else if (batt->vbat_mv >= 4600)
			val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		else if (batt->vbat_mv <= 3000)
			val->intval = POWER_SUPPLY_HEALTH_DEAD;
		else
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = batt->qmax_mah * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = batt->qmax_mah * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = div_s64((s64)batt->soc * batt->qmax_mah * 1000, 100);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = batt->i_max_ua;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = batt->tables->name;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = batt->tables->name;
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

	ret = 0;
out:
	mutex_unlock(&batt->lock);

	return ret;
}

static void mtk_meizu_work_fn(struct work_struct *work)
{
	struct mtk_meizu_batt *batt =
		container_of(to_delayed_work(work), struct mtk_meizu_batt, work);

	mtk_meizu_poll_charger(batt);
	mutex_lock(&batt->lock);
	mtk_meizu_update(batt);
	mutex_unlock(&batt->lock);

	power_supply_changed(batt->psy);
	schedule_delayed_work(&batt->work, msecs_to_jiffies(10000));
}

static int mtk_meizu_charger_event(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	struct mtk_meizu_batt *batt =
		container_of(nb, struct mtk_meizu_batt, nb);

	if (batt->psy)
		power_supply_changed(batt->psy);

	return NOTIFY_DONE;
}

static enum power_supply_property mtk_meizu_batt_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TEMP_ALERT_MIN,
	POWER_SUPPLY_PROP_TEMP_ALERT_MAX,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static const struct power_supply_desc mtk_meizu_batt_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = mtk_meizu_batt_props,
	.num_properties = ARRAY_SIZE(mtk_meizu_batt_props),
	.get_property = mtk_meizu_get_property,
};

static const struct mtk_meizu_batt_tables *mtk_meizu_match_tables(struct device *dev)
{
	const char *profile;
	int ret;

	ret = device_property_read_string(dev, "mediatek,battery-profile", &profile);
	if (ret)
		return ERR_PTR(-EINVAL);

	if (!strcmp(profile, "m5c-atl"))
		return &mtk_meizu_batt_m5c_atl;
	if (!strcmp(profile, "m5c-veken"))
		return &mtk_meizu_batt_m5c_veken;
	if (!strcmp(profile, "m5c-sunwoda"))
		return &mtk_meizu_batt_m5c_sunwoda;
	if (!strcmp(profile, "m3note"))
		return &mtk_meizu_batt_m3note;
	if (!strcmp(profile, "mx6"))
		return &mtk_meizu_batt_mx6;

	return ERR_PTR(-ENODEV);
}

static int mtk_meizu_batt_probe(struct platform_device *pdev)
{
	struct power_supply_config cfg = { };
	struct mtk_meizu_batt *batt;
	struct device_node *adc_np;
	int ret;

	batt = devm_kzalloc(&pdev->dev, sizeof(*batt), GFP_KERNEL);
	if (!batt)
		return -ENOMEM;

	batt->dev = &pdev->dev;
	mutex_init(&batt->lock);

	batt->tables = mtk_meizu_match_tables(&pdev->dev);
	if (IS_ERR(batt->tables))
		return dev_err_probe(&pdev->dev, PTR_ERR(batt->tables),
				     "unsupported or missing battery profile\n");

	adc_np = of_parse_phandle(pdev->dev.of_node, "mediatek,auxadc", 0);
	if (!adc_np)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing the PMIC AUXADC node\n");

	batt->auxadc = mediatek_pmic_auxadc_get(adc_np);
	of_node_put(adc_np);
	if (!batt->auxadc)
		return -EPROBE_DEFER;

	ret = device_property_read_u32(&pdev->dev, "voltage-min-design-microvolt",
				       &batt->vmin_mv);
	if (!ret)
		batt->vmin_mv /= 1000;
	else
		batt->vmin_mv = 3400;

	ret = device_property_read_u32(&pdev->dev, "voltage-max-design-microvolt",
				       &batt->vmax_mv);
	if (!ret)
		batt->vmax_mv /= 1000;
	else
		batt->vmax_mv = 4350;

	ret = device_property_read_u32(&pdev->dev, "charge-current-max-microamp",
				       &batt->i_max_ua);
	if (ret)
		batt->i_max_ua = 1000000;

	batt->qmax_mah = batt->tables->q_max;

	ret = device_property_read_u32(&pdev->dev, "mediatek,ntc-pullup-ohms",
				       &batt->pullup_ohm);
	if (ret)
		batt->pullup_ohm = MTK_MEIZU_NTC_PULLUP_OHM;

	ret = device_property_read_u32(&pdev->dev, "mediatek,ntc-bias-microvolt",
				       &batt->pullup_volt_mv);
	if (!ret)
		batt->pullup_volt_mv /= 1000;
	else
		batt->pullup_volt_mv = MTK_MEIZU_NTC_BIAS_MV;

	cfg.fwnode = dev_fwnode(&pdev->dev);
	cfg.drv_data = batt;

	batt->psy = devm_power_supply_register(&pdev->dev, &mtk_meizu_batt_desc,
					       &cfg);
	if (IS_ERR(batt->psy))
		return dev_err_probe(&pdev->dev, PTR_ERR(batt->psy),
				     "failed to register the battery\n");

	batt->charger = power_supply_get_by_name("usb");
	if (!batt->charger)
		batt->charger = power_supply_get_by_name("fan5405-charger");
	if (!batt->charger)
		batt->charger = power_supply_get_by_name("bq25890-charger");

	if (!batt->charger) {
		dev_warn(&pdev->dev, "no charger power supply linked\n");
	} else {
		batt->nb.notifier_call = mtk_meizu_charger_event;
		ret = power_supply_reg_notifier(&batt->nb);
		if (ret) {
			power_supply_put(batt->charger);
			return dev_err_probe(&pdev->dev, ret,
					     "failed to register the notifier\n");
		}
	}

	platform_set_drvdata(pdev, batt);

	mtk_meizu_batt_init_state(batt);

	INIT_DELAYED_WORK(&batt->work, mtk_meizu_work_fn);
	schedule_delayed_work(&batt->work, msecs_to_jiffies(10000));

	dev_info(&pdev->dev, "%s battery profile, %d mAh, SOC %d %%, %d mV, %d mA, %d.%d C\n",
		 batt->tables->name, batt->qmax_mah, batt->soc, batt->vbat_mv,
		 batt->ibat_ua / 1000, batt->temp_mc / 1000,
		 abs(batt->temp_mc % 1000) / 100);

	return 0;
}

static void mtk_meizu_batt_remove(struct platform_device *pdev)
{
	struct mtk_meizu_batt *batt = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&batt->work);
	if (batt->charger) {
		power_supply_unreg_notifier(&batt->nb);
		power_supply_put(batt->charger);
	}
}

static const struct of_device_id mtk_meizu_batt_of_match[] = {
	{ .compatible = "mediatek,mt6328-battery", },
	{ .compatible = "mediatek,mt6351-battery", },
	{ .compatible = "mediatek,mt6353-battery", },
	{ .compatible = "mediatek,mt6735-battery", },
	{ .compatible = "mediatek,mt6797-battery", },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_meizu_batt_of_match);

static struct platform_driver mtk_meizu_batt_driver = {
	.driver = {
		.name = "mtk-meizu-battery",
		.of_match_table = mtk_meizu_batt_of_match,
	},
	.probe = mtk_meizu_batt_probe,
	.remove = mtk_meizu_batt_remove,
};
module_platform_driver(mtk_meizu_batt_driver);

MODULE_DESCRIPTION("MediaTek/Meizu fuel gauge and battery driver");
MODULE_LICENSE("GPL");
