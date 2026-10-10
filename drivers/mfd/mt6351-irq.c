// SPDX-License-Identifier: GPL-2.0-only
/* MT6351 interrupt controller; register layout from MediaTek's MT6797 BSP. */

#include <linux/interrupt.h>
#include <linux/mfd/mt6351/core.h>
#include <linux/mfd/mt6351/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define MT6351_IRQ(_irq) \
	REGMAP_IRQ_REG(_irq, ((_irq) / 16) * 2, BIT((_irq) % 16))

static const struct regmap_irq mt6351_irqs[MT6351_IRQ_NR] = {
	MT6351_IRQ(MT6351_IRQ_PWRKEY),
	MT6351_IRQ(MT6351_IRQ_HOMEKEY),
	MT6351_IRQ(MT6351_IRQ_PWRKEY_R),
	MT6351_IRQ(MT6351_IRQ_HOMEKEY_R),
	MT6351_IRQ(MT6351_IRQ_THR_H),
	MT6351_IRQ(MT6351_IRQ_THR_L),
	MT6351_IRQ(MT6351_IRQ_BAT_H),
	MT6351_IRQ(MT6351_IRQ_BAT_L),
	MT6351_IRQ(MT6351_IRQ_RTC),
	MT6351_IRQ(MT6351_IRQ_AUDIO),
	MT6351_IRQ(MT6351_IRQ_MAD),
	MT6351_IRQ(MT6351_IRQ_ACCDET),
	MT6351_IRQ(MT6351_IRQ_ACCDET_EINT),
	MT6351_IRQ(MT6351_IRQ_ACCDET_NEGV),
	MT6351_IRQ(MT6351_IRQ_NI_LBAT_INT),
	MT6351_IRQ(MT6351_IRQ_VCORE_OC),
	MT6351_IRQ(MT6351_IRQ_VGPU_OC),
	MT6351_IRQ(MT6351_IRQ_VSRAM_MD_OC),
	MT6351_IRQ(MT6351_IRQ_VMODEM_OC),
	MT6351_IRQ(MT6351_IRQ_VM1_OC),
	MT6351_IRQ(MT6351_IRQ_VS1_OC),
	MT6351_IRQ(MT6351_IRQ_VS2_OC),
	MT6351_IRQ(MT6351_IRQ_VPA_OC),
	MT6351_IRQ(MT6351_IRQ_VCORE_PREOC),
	MT6351_IRQ(MT6351_IRQ_VGPU_PREOC),
	MT6351_IRQ(MT6351_IRQ_VSRAM_MD_PREOC),
	MT6351_IRQ(MT6351_IRQ_VMODEM_PREOC),
	MT6351_IRQ(MT6351_IRQ_VM1_PREOC),
	MT6351_IRQ(MT6351_IRQ_VS1_PREOC),
	MT6351_IRQ(MT6351_IRQ_VS2_PREOC),
	MT6351_IRQ(MT6351_IRQ_LDO_OC),
	MT6351_IRQ(MT6351_IRQ_JEITA_HOT),
	MT6351_IRQ(MT6351_IRQ_JEITA_WARM),
	MT6351_IRQ(MT6351_IRQ_JEITA_COOL),
	MT6351_IRQ(MT6351_IRQ_JEITA_COLD),
	MT6351_IRQ(MT6351_IRQ_AUXADC_IMP),
	MT6351_IRQ(MT6351_IRQ_NAG_C_DLTV),
	MT6351_IRQ(MT6351_IRQ_OV),
	MT6351_IRQ(MT6351_IRQ_BVALID_DET),
	MT6351_IRQ(MT6351_IRQ_RGS_BATON_HV),
	MT6351_IRQ(MT6351_IRQ_VBATON_UNDET),
	MT6351_IRQ(MT6351_IRQ_WATCHDOG),
	MT6351_IRQ(MT6351_IRQ_PCHR_CM_VDEC),
	MT6351_IRQ(MT6351_IRQ_CHRDET),
	MT6351_IRQ(MT6351_IRQ_PCHR_CM_VINC),
	MT6351_IRQ(MT6351_IRQ_FG_BAT_H),
	MT6351_IRQ(MT6351_IRQ_FG_BAT_L),
	MT6351_IRQ(MT6351_IRQ_FG_CUR_H),
	MT6351_IRQ(MT6351_IRQ_FG_CUR_L),
	MT6351_IRQ(MT6351_IRQ_FG_ZCV),
};

static unsigned int mt6351_irq_reg(struct regmap_irq_chip_data *data,
				   unsigned int base, int index)
{
	/* Enable banks have SET/CLR companions; status banks do not. */
	return base + index * (base == MT6351_INT_CON0 ? 6 : 2);
}

static const struct regmap_irq_chip mt6351_irq_chip = {
	.name = "mt6351-irq",
	.status_base = MT6351_INT_STATUS0,
	.unmask_base = MT6351_INT_CON0,
	.ack_base = MT6351_INT_STATUS0,
	.num_regs = 4,
	.irqs = mt6351_irqs,
	.num_irqs = ARRAY_SIZE(mt6351_irqs),
	.get_irq_reg = mt6351_irq_reg,
};

int mt6351_irq_init(struct mt6397_chip *chip)
{
	struct regmap_irq_chip_data *data;
	int ret;

	if (chip->chip_id != MT6351_CHIP_ID)
		return -ENODEV;

	ret = devm_regmap_add_irq_chip_fwnode(chip->dev,
			dev_fwnode(chip->dev), chip->regmap, chip->irq,
			IRQF_ONESHOT, 0, &mt6351_irq_chip, &data);
	if (ret)
		return ret;

	chip->irq_data = data;
	chip->irq_domain = regmap_irq_get_domain(data);
	return 0;
}

void mt6351_irq_exit(struct mt6397_chip *chip)
{
	devm_regmap_del_irq_chip(chip->dev, chip->irq, chip->irq_data);
}
