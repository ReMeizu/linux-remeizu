// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6328 interrupt controller
 *
 * The MT6328 PMIC (MT6735/MT6737M) exposes its 16 interrupt status and
 * enable bits per bank through register pairs: MT6328_INT_STATUSn (read to
 * acknowledge) and MT6328_INT_CONn (enable), both with SET/CLR companions.
 * The register offsets come from the vendor 4.9 UPmu register map
 * (drivers/misc/mediatek/include/mt-plat/mt6735/include/mach/upmu_hw.h,
 * MT6328_INT_CON0 = 0x02AC ... MT6328_INT_STATUS2 = 0x02C8).
 *
 * Bits 9..11 of the first bank are the audio and headset detection sources
 * that the ASoC codec and mtk-accdet drivers consume as sub-interrupts.
 */

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/mfd/mt6328/core.h>
#include <linux/mfd/mt6328/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define MT6328_IRQ(_irq) \
	REGMAP_IRQ_REG(_irq, ((_irq) / 16) * 2, BIT((_irq) % 16))

static const struct regmap_irq mt6328_irqs[MT6328_IRQ_NR] = {
	MT6328_IRQ(MT6328_IRQ_STATUS_PWRKEY),
	MT6328_IRQ(MT6328_IRQ_STATUS_HOMEKEY),
	MT6328_IRQ(MT6328_IRQ_STATUS_PWRKEY_R),
	MT6328_IRQ(MT6328_IRQ_STATUS_HOMEKEY_R),
	MT6328_IRQ(MT6328_IRQ_STATUS_THR_H),
	MT6328_IRQ(MT6328_IRQ_STATUS_THR_L),
	MT6328_IRQ(MT6328_IRQ_STATUS_BAT_H),
	MT6328_IRQ(MT6328_IRQ_STATUS_BAT_L),
	MT6328_IRQ(MT6328_IRQ_STATUS_RTC),
	MT6328_IRQ(MT6328_IRQ_STATUS_AUDIO),
	MT6328_IRQ(MT6328_IRQ_STATUS_ACCDET),
	MT6328_IRQ(MT6328_IRQ_STATUS_ACCDET_EINT),
	MT6328_IRQ(MT6328_IRQ_STATUS_ACCDET_NEGV),
	MT6328_IRQ(MT6328_IRQ_STATUS_NI_LBAT_INT),
	MT6328_IRQ(MT6328_IRQ_STATUS_VPROC_OC),
	MT6328_IRQ(MT6328_IRQ_STATUS_VSYS_OC),
	MT6328_IRQ(MT6328_IRQ_STATUS_VLTE_OC),
	MT6328_IRQ(MT6328_IRQ_STATUS_VCORE_OC),
	MT6328_IRQ(MT6328_IRQ_STATUS_VPA_OC),
	MT6328_IRQ(MT6328_IRQ_STATUS_LDO_OC),
	MT6328_IRQ(MT6328_IRQ_STATUS_BAT2_H),
	MT6328_IRQ(MT6328_IRQ_STATUS_BAT2_L),
	MT6328_IRQ(MT6328_IRQ_STATUS_VISMPS0_H),
	MT6328_IRQ(MT6328_IRQ_STATUS_VISMPS0_L),
	MT6328_IRQ(MT6328_IRQ_STATUS_AUXADC_IMP),
	MT6328_IRQ(MT6328_IRQ_STATUS_OV),
	MT6328_IRQ(MT6328_IRQ_STATUS_BVALID_DET),
	MT6328_IRQ(MT6328_IRQ_STATUS_VBATON_HV),
	MT6328_IRQ(MT6328_IRQ_STATUS_VBATON_UNDET),
	MT6328_IRQ(MT6328_IRQ_STATUS_WATCHDOG),
	MT6328_IRQ(MT6328_IRQ_STATUS_PCHR_CM_VDEC),
	MT6328_IRQ(MT6328_IRQ_STATUS_CHRDET),
	MT6328_IRQ(MT6328_IRQ_STATUS_PCHR_CM_VINC),
	MT6328_IRQ(MT6328_IRQ_STATUS_FG_BAT_H),
	MT6328_IRQ(MT6328_IRQ_STATUS_FG_BAT_L),
	MT6328_IRQ(MT6328_IRQ_STATUS_FG_CUR_H),
	MT6328_IRQ(MT6328_IRQ_STATUS_FG_CUR_L),
	MT6328_IRQ(MT6328_IRQ_STATUS_FG_ZCV),
	MT6328_IRQ(MT6328_IRQ_STATUS_SPKL_D),
	MT6328_IRQ(MT6328_IRQ_STATUS_SPKL_AB),
};

static unsigned int mt6328_irq_reg(struct regmap_irq_chip_data *data,
				   unsigned int base, int index)
{
	/* Enable banks have SET/CLR companions; status banks do not. */
	return base + index * (base == MT6328_INT_CON0 ? 6 : 2);
}

static const struct regmap_irq_chip mt6328_irq_chip = {
	.name = "mt6328-irq",
	.status_base = MT6328_INT_STATUS0,
	.unmask_base = MT6328_INT_CON0,
	.ack_base = MT6328_INT_STATUS0,
	.num_regs = 3,
	.irqs = mt6328_irqs,
	.num_irqs = ARRAY_SIZE(mt6328_irqs),
	.get_irq_reg = mt6328_irq_reg,
};

int mt6328_irq_init(struct mt6397_chip *chip)
{
	struct regmap_irq_chip_data *data;
	int ret;

	ret = devm_regmap_add_irq_chip_fwnode(chip->dev,
			dev_fwnode(chip->dev), chip->regmap, chip->irq,
			IRQF_ONESHOT, 0, &mt6328_irq_chip, &data);
	if (ret)
		return ret;

	chip->irq_data = data;
	chip->irq_domain = regmap_irq_get_domain(data);

	return 0;
}

void mt6328_irq_exit(struct mt6397_chip *chip)
{
	devm_regmap_del_irq_chip(chip->dev, chip->irq, chip->irq_data);
}
