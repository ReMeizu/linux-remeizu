/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */

#ifndef _DT_BINDINGS_MEDIATEK_MT6351_AUXADC_H
#define _DT_BINDINGS_MEDIATEK_MT6351_AUXADC_H

/* ADC Channel Index. MT6351 and MT6353 share this channel map. */
#define MT6351_AUXADC_BATSNS		0
#define MT6351_AUXADC_ISENSE		1
#define MT6351_AUXADC_VCDT		2
#define MT6351_AUXADC_BATTEMP		3
#define MT6351_AUXADC_CHIPTEMP		4
#define MT6351_AUXADC_ACCDET		5
#define MT6351_AUXADC_TSX		7
#define MT6351_AUXADC_HPOFS_CAL		9
#define MT6351_AUXADC_BATID		12
#define MT6351_AUXADC_VBAT		13

#define MT6353_AUXADC_BATSNS		0
#define MT6353_AUXADC_ISENSE		1
#define MT6353_AUXADC_VCDT		2
#define MT6353_AUXADC_BATTEMP		3
#define MT6353_AUXADC_CHIPTEMP		4
#define MT6353_AUXADC_ACCDET		5
#define MT6353_AUXADC_TSX		7
#define MT6353_AUXADC_HPOFS_CAL		9
#define MT6353_AUXADC_BATID		12
#define MT6353_AUXADC_VBAT		13

/*
 * Channels 12..15 are multiplexed through AUXADC_CON4[15:13]. The value is
 * the slot number, and channel 12 of each slot carries a battery-ID input.
 */
#define MT6351_AUXADC_BATID_SLOT(slot)	(((slot) & 3) << 13)
#define MT6353_AUXADC_BATID_SLOT(slot)	(((slot) & 3) << 13)

#endif
