/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek PMIC AUXADC helper library
 *
 * The MT6328 / MT6351 / MT6353 PMIC family shares one AUXADC block. It is
 * reachable through the PMIC wrapper (pwrap) and is used by three different
 * kinds of consumers:
 *
 *  - the PMIC AUXADC IIO device (mt6328-auxadc.c), which exposes VBAT,
 *    VCHG, battery NTC and PMIC junction temperature channels;
 *  - the fuel gauge, which drives one immediate conversion per report and
 *    needs the impedance (PTIM) measurement;
 *  - the SoC thermal controller, which reads the TSX channel through its own
 *    hardware state machine and therefore only needs the block powered.
 *
 * Register offsets are absolute PMIC register addresses as documented in the
 * MediaTek 3.18/4.9 BSPs (upmu_hw.h: MT6328_AUXADC_*, MT6351_AUXADC_*), which
 * match the upstream MT6397-family register map (register + 0, _SET + 2,
 * _CLR + 4, 16 bit registers).
 */

#ifndef __LINUX_IIO_ADC_MEDIATEK_PMIC_AUXADC_H__
#define __LINUX_IIO_ADC_MEDIATEK_PMIC_AUXADC_H__

#include <linux/device.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/types.h>

struct mediatek_pmic_auxadc;

/* Channel indices, shared by all PMICs of the family */
#define MTK_PMIC_AUXADC_CHAN_BATSNS	0
#define MTK_PMIC_AUXADC_CHAN_ISENSE	1
#define MTK_PMIC_AUXADC_CHAN_VCDT	2
#define MTK_PMIC_AUXADC_CHAN_BATTEMP	3
#define MTK_PMIC_AUXADC_CHAN_CHIPTEMP	4
#define MTK_PMIC_AUXADC_CHAN_ACCDET	5
#define MTK_PMIC_AUXADC_CHAN_TSX	7
#define MTK_PMIC_AUXADC_CHAN_HPOFS	9
#define MTK_PMIC_AUXADC_CHAN_BATID	12
#define MTK_PMIC_AUXADC_CHAN_VBAT	13
#define MTK_PMIC_AUXADC_CHAN_MAX	16

enum mtk_pmic_auxadc_regs {
	MTK_PMIC_AUXADC_REG_FGADC_CON0,
	MTK_PMIC_AUXADC_REG_FGADC_CON1,
	MTK_PMIC_AUXADC_REG_AUXADC_ADC0,
	MTK_PMIC_AUXADC_REG_AUXADC_STA0,
	MTK_PMIC_AUXADC_REG_AUXADC_RQST0_SET,
	MTK_PMIC_AUXADC_REG_AUXADC_CON0,
	MTK_PMIC_AUXADC_REG_AUXADC_CON0_SET,
	MTK_PMIC_AUXADC_REG_AUXADC_CON0_CLR,
	MTK_PMIC_AUXADC_REG_AUXADC_CON2,
	MTK_PMIC_AUXADC_REG_AUXADC_CON3,
	MTK_PMIC_AUXADC_REG_AUXADC_CON4,
	MTK_PMIC_AUXADC_REG_AUXADC_CON6,
	MTK_PMIC_AUXADC_REG_AUXADC_CON7,
	MTK_PMIC_AUXADC_REG_AUXADC_IMP0,
	MTK_PMIC_AUXADC_REG_AUXADC_IMP1,
	MTK_PMIC_AUXADC_REG_TOP_CKPDN_CON2,
	MTK_PMIC_AUXADC_REG_MAX
};

/**
 * struct mtk_pmic_auxadc_data - PMIC specific description
 * @name:		PMIC name, used as the IIO device name
 * @regs:		absolute register offsets, see enum mtk_pmic_auxadc_regs
 * @avg_num_sel:	AUXADC_CON2[13:10] averaging selector
 * @spl_num_sleep:	AUXADC_CON3 sampling number (sleep / single shot)
 * @sel_12_15:		AUXADC_CON4 channel selector that routes the shared
 *			channels 12..15 onto the ADC12/ADC15 data registers
 * @dcxo_con2_large:	AUXADC_CON2[3], unused on these PMICs
 * @reads_batid:	the BATID ladder is reachable over the shared channel
 * @reads_chrtemp:	channel 13 carries the charger NTC temperature
 * @no_pwdb:		the PMIC cannot be brought out of power down by
 *			software (the bootloader leaves the ADC running)
 * @isense_r_bat:	sense resistor in micro ohm for the ISENSE channel
 * @vcdt_r1:		divider upper resistor (ohm) installed on the board
 * @vcdt_r2:		divider lower resistor (ohm) installed on the board
 * @battemp_pull_up:	battery NTC pull-up resistor (ohm), 0 when absent
 * @battemp_pull_volt:	battery NTC bias voltage (millivolt), 0 when absent
 */
struct mtk_pmic_auxadc_data {
	const char *name;
	u16 regs[MTK_PMIC_AUXADC_REG_MAX];
	u8 avg_num_sel;
	u8 spl_num_sleep;
	u8 sel_12_15;
	bool dcxo_con2_large;
	bool reads_batid;
	bool reads_chrtemp;
	bool no_pwdb;
	u32 isense_r_bat;
	u32 vcdt_r1;
	u32 vcdt_r2;
	u32 battemp_pull_up;
	u32 battemp_pull_volt;
};

#if IS_ENABLED(CONFIG_MEDIATEK_PMIC_AUXADC)
struct mediatek_pmic_auxadc *
mediatek_pmic_auxadc_alloc(struct device *dev, struct regmap *regmap,
			   const struct mtk_pmic_auxadc_data *data);
struct mediatek_pmic_auxadc *mediatek_pmic_auxadc_get(struct device_node *np);
void mediatek_pmic_auxadc_put(struct mediatek_pmic_auxadc *adc);

int mediatek_pmic_auxadc_init(struct mediatek_pmic_auxadc *adc);
int mediatek_pmic_auxadc_power_on(struct mediatek_pmic_auxadc *adc);
int mediatek_pmic_auxadc_read(struct mediatek_pmic_auxadc *adc, unsigned int chan,
			      u32 *raw);
int mediatek_pmic_auxadc_read_ptim(struct mediatek_pmic_auxadc *adc,
				   u32 *vbat, s32 *ibat);
int mediatek_pmic_auxadc_read_batid(struct mediatek_pmic_auxadc *adc,
				    unsigned int chan, u32 *raw);
#else
static inline struct mediatek_pmic_auxadc *
mediatek_pmic_auxadc_alloc(struct device *dev, struct regmap *regmap,
			   const struct mtk_pmic_auxadc_data *data)
{
	return NULL;
}

static inline struct mediatek_pmic_auxadc *
mediatek_pmic_auxadc_get(struct device_node *np)
{
	return NULL;
}

static inline void mediatek_pmic_auxadc_put(struct mediatek_pmic_auxadc *adc) { }

static inline int mediatek_pmic_auxadc_init(struct mediatek_pmic_auxadc *adc)
{
	return -ENODEV;
}

static inline int mediatek_pmic_auxadc_power_on(struct mediatek_pmic_auxadc *adc)
{
	return 0;
}

static inline int mediatek_pmic_auxadc_read(struct mediatek_pmic_auxadc *adc,
					    unsigned int chan, u32 *raw)
{
	return -ENODEV;
}

static inline int mediatek_pmic_auxadc_read_ptim(struct mediatek_pmic_auxadc *adc,
						 u32 *vbat, s32 *ibat)
{
	return -ENODEV;
}

static inline int mediatek_pmic_auxadc_read_batid(struct mediatek_pmic_auxadc *adc,
						  unsigned int chan, u32 *raw)
{
	return -ENODEV;
}
#endif

#endif /* __LINUX_IIO_ADC_MEDIATEK_PMIC_AUXADC_H__ */
