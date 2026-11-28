/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek/Meizu fuel gauge battery profile tables
 *
 * Copyright (c) 2015 MediaTek Inc.
 * Copyright (c) 2026 The ReMeizu project
 *
 * OCV (open-circuit voltage vs. state of charge) and internal resistance
 * tables for the Meizu devices built on the MT6735/MT6755/MT6797 platforms.
 *
 * Sources:
 *  - M5c (MT6737M): the stock Flyme 6.0.2.4G kernel tables of the three
 *    battery vendors (ATL, VEKEN, SUNWODA), recovered from the stock vmlinux
 *    by the M5c bring-up (fgauge_get_profile()/fgauge_get_profile_r_table(),
 *    100 points per vendor and temperature).
 *  - M3 Note (MT6755): MediaTek BSP multi-profile table of the MT6755 kernel
 *    (mt_battery_meter_table_multi_profile.h, 88 points, battery ID 2).
 *  - MX6 (MT6797): MediaTek BSP table of the MT6797 kernel
 *    (mt_battery_meter_table.h, 80 points).
 *
 * Temperatures are the four MediaTek profile slots: T0 = -10 C, T1 = 0 C,
 * T2 = 25 C and T3 = 50 C.
 */

#ifndef __LINUX_POWER_SUPPLY_MTK_MEIZU_BATTERY_PROFILES_H__
#define __LINUX_POWER_SUPPLY_MTK_MEIZU_BATTERY_PROFILES_H__

#include <linux/types.h>

/* One {percentage, voltage} or {resistance, voltage} point, mV / percent */
struct mtk_meizu_batt_pt {
	s16 x;
	s16 mv;
};

/* One {temperature, NTC resistance} point of the battery NTC table */
struct mtk_meizu_ntc_pt {
	s16 temp_c;
	s32 ohm;
};

#define MTK_MEIZU_OCV_POINTS		100
#define MTK_MEIZU_NUM_TEMPS		4

/**
 * struct mtk_meizu_batt_profile - one profile slot
 * @ocv:		state of charge (%) vs. battery voltage
 * @r:			internal resistance (mOhm) vs. battery voltage
 */
struct mtk_meizu_batt_profile {
	const struct mtk_meizu_batt_pt *ocv;
	const struct mtk_meizu_batt_pt *r;
};

/**
 * struct mtk_meizu_batt_tables - full table set of one board
 * @name:	profile set name
 * @points:	number of points of every table
 * @profiles:	T0, T1, T2, T3
 * @ntc:	12 kOhm pull-up NTC table (battery pack thermistor)
 * @ntc_len:	number of NTC entries
 * @q_max:	nominal capacity in mAh at 25 C
 */
struct mtk_meizu_batt_tables {
	const char *name;
	unsigned int points;
	struct mtk_meizu_batt_profile profiles[MTK_MEIZU_NUM_TEMPS];
	const struct mtk_meizu_ntc_pt *ntc;
	unsigned int ntc_len;
	int q_max;
};

extern const struct mtk_meizu_batt_tables mtk_meizu_batt_m5c_atl;
extern const struct mtk_meizu_batt_tables mtk_meizu_batt_m5c_veken;
extern const struct mtk_meizu_batt_tables mtk_meizu_batt_m5c_sunwoda;
extern const struct mtk_meizu_batt_tables mtk_meizu_batt_m3note;
extern const struct mtk_meizu_batt_tables mtk_meizu_batt_mx6;

#endif /* __LINUX_POWER_SUPPLY_MTK_MEIZU_BATTERY_PROFILES_H__ */
