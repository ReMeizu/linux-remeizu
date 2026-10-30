// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Author: Flora Fu, MediaTek
 */

#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/core.h>
#include <linux/mfd/mt6323/core.h>
#include <linux/mfd/mt6328/core.h>
#include <linux/mfd/mt6331/core.h>
#include <linux/mfd/mt6351/core.h>
#include <linux/mfd/mt6353/registers.h>
#include <linux/mfd/mt6357/core.h>
#include <linux/mfd/mt6358/core.h>
#include <linux/mfd/mt6359/core.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6323/registers.h>
#include <linux/mfd/mt6328/registers.h>
#include <linux/mfd/mt6331/registers.h>
#include <linux/mfd/mt6351/registers.h>
#include <linux/mfd/mt6357/registers.h>
#include <linux/mfd/mt6358/registers.h>
#include <linux/mfd/mt6359/registers.h>
#include <linux/mfd/mt6397/registers.h>

#define MT6323_RTC_BASE		0x8000
#define MT6323_RTC_SIZE		0x40

#define MT6357_RTC_BASE		0x0588
#define MT6357_RTC_SIZE		0x3c

#define MT6331_RTC_BASE		0x4000
#define MT6331_RTC_SIZE		0x40

#define MT6358_RTC_BASE		0x0588
#define MT6358_RTC_SIZE		0x3c

#define MT6397_RTC_BASE		0xe000
#define MT6397_RTC_SIZE		0x3e

#define MT6323_PWRC_BASE	0x8000
#define MT6323_PWRC_SIZE	0x40

static const struct resource mt6323_rtc_resources[] = {
	DEFINE_RES_MEM(MT6323_RTC_BASE, MT6323_RTC_SIZE),
	DEFINE_RES_IRQ(MT6323_IRQ_STATUS_RTC),
};

static const struct resource mt6357_rtc_resources[] = {
	DEFINE_RES_MEM(MT6357_RTC_BASE, MT6357_RTC_SIZE),
	DEFINE_RES_IRQ(MT6357_IRQ_RTC),
};

static const struct resource mt6331_rtc_resources[] = {
	DEFINE_RES_MEM(MT6331_RTC_BASE, MT6331_RTC_SIZE),
	DEFINE_RES_IRQ(MT6331_IRQ_STATUS_RTC),
};

static const struct resource mt6358_rtc_resources[] = {
	DEFINE_RES_MEM(MT6358_RTC_BASE, MT6358_RTC_SIZE),
	DEFINE_RES_IRQ(MT6358_IRQ_RTC),
};

static const struct resource mt6397_rtc_resources[] = {
	DEFINE_RES_MEM(MT6397_RTC_BASE, MT6397_RTC_SIZE),
	DEFINE_RES_IRQ(MT6397_IRQ_RTC),
};

static const struct resource mt6358_keys_resources[] = {
	DEFINE_RES_IRQ_NAMED(MT6358_IRQ_PWRKEY, "powerkey"),
	DEFINE_RES_IRQ_NAMED(MT6358_IRQ_HOMEKEY, "homekey"),
	DEFINE_RES_IRQ_NAMED(MT6358_IRQ_PWRKEY_R, "powerkey_r"),
	DEFINE_RES_IRQ_NAMED(MT6358_IRQ_HOMEKEY_R, "homekey_r"),
};

static const struct resource mt6359_keys_resources[] = {
	DEFINE_RES_IRQ_NAMED(MT6359_IRQ_PWRKEY, "powerkey"),
	DEFINE_RES_IRQ_NAMED(MT6359_IRQ_HOMEKEY, "homekey"),
	DEFINE_RES_IRQ_NAMED(MT6359_IRQ_PWRKEY_R, "powerkey_r"),
	DEFINE_RES_IRQ_NAMED(MT6359_IRQ_HOMEKEY_R, "homekey_r"),
};

static const struct resource mt6359_accdet_resources[] = {
	DEFINE_RES_IRQ_NAMED(MT6359_IRQ_ACCDET, "accdet_irq"),
	DEFINE_RES_IRQ_NAMED(MT6359_IRQ_ACCDET_EINT0, "accdet_eint0"),
	DEFINE_RES_IRQ_NAMED(MT6359_IRQ_ACCDET_EINT1, "accdet_eint1"),
};

static const struct resource mt6323_keys_resources[] = {
	DEFINE_RES_IRQ_NAMED(MT6323_IRQ_STATUS_PWRKEY, "powerkey"),
	DEFINE_RES_IRQ_NAMED(MT6323_IRQ_STATUS_FCHRKEY, "homekey"),
};

static const struct resource mt6328_keys_resources[] = {
	DEFINE_RES_IRQ_NAMED(MT6328_IRQ_STATUS_PWRKEY, "powerkey"),
	DEFINE_RES_IRQ_NAMED(MT6328_IRQ_STATUS_HOMEKEY, "homekey"),
	DEFINE_RES_IRQ_NAMED(MT6328_IRQ_STATUS_PWRKEY_R, "powerkey_r"),
	DEFINE_RES_IRQ_NAMED(MT6328_IRQ_STATUS_HOMEKEY_R, "homekey_r"),
};

static const struct resource mt6357_keys_resources[] = {
	DEFINE_RES_IRQ_NAMED(MT6357_IRQ_PWRKEY, "powerkey"),
	DEFINE_RES_IRQ_NAMED(MT6357_IRQ_HOMEKEY, "homekey"),
	DEFINE_RES_IRQ_NAMED(MT6357_IRQ_PWRKEY_R, "powerkey_r"),
	DEFINE_RES_IRQ_NAMED(MT6357_IRQ_HOMEKEY_R, "homekey_r"),
};

static const struct resource mt6331_keys_resources[] = {
	DEFINE_RES_IRQ_NAMED(MT6331_IRQ_STATUS_PWRKEY, "powerkey"),
	DEFINE_RES_IRQ_NAMED(MT6331_IRQ_STATUS_HOMEKEY, "homekey"),
};

static const struct resource mt6397_keys_resources[] = {
	DEFINE_RES_IRQ_NAMED(MT6397_IRQ_PWRKEY, "powerkey"),
	DEFINE_RES_IRQ_NAMED(MT6397_IRQ_HOMEKEY, "homekey"),
};

static const struct resource mt6323_pwrc_resources[] = {
	DEFINE_RES_MEM(MT6323_PWRC_BASE, MT6323_PWRC_SIZE),
};

static const struct mfd_cell mt6323_devs[] = {
	{
		.name = "mt6323-rtc",
		.num_resources = ARRAY_SIZE(mt6323_rtc_resources),
		.resources = mt6323_rtc_resources,
		.of_compatible = "mediatek,mt6323-rtc",
	}, {
		.name = "mt6323-regulator",
		.of_compatible = "mediatek,mt6323-regulator"
	}, {
		.name = "mt6323-led",
		.of_compatible = "mediatek,mt6323-led"
	}, {
		.name = "mt6323-keys",
		.num_resources = ARRAY_SIZE(mt6323_keys_resources),
		.resources = mt6323_keys_resources,
		.of_compatible = "mediatek,mt6323-keys"
	}, {
		.name = "mt6323-pwrc",
		.num_resources = ARRAY_SIZE(mt6323_pwrc_resources),
		.resources = mt6323_pwrc_resources,
		.of_compatible = "mediatek,mt6323-pwrc"
	},
};

static const struct mfd_cell mt6328_devs[] = {
	{
		.name = "mt6328-regulator",
		.of_compatible = "mediatek,mt6328-regulator"
	}, {
		.name = "mt6328-keys",
		.num_resources = ARRAY_SIZE(mt6328_keys_resources),
		.resources = mt6328_keys_resources,
		.of_compatible = "mediatek,mt6328-keys"
	},
};

static const struct mfd_cell mt6351_devs[] = {
	{ .name = "mt6351-regulator" },
};

/*
 * MT6353 storage-only support. The cell name matches the platform driver in
 * drivers/regulator/mt6353-regulator.c; no of_compatible is used, following
 * the MT6351 cell convention above (the regulator driver inherits the PMIC
 * node and looks up its "regulators" child).
 */
static const struct mfd_cell mt6353_devs[] = {
	{ .name = "mt6353-regulator" },
};

static const struct mfd_cell mt6357_devs[] = {
	{
		.name = "mt6359-auxadc",
		.of_compatible = "mediatek,mt6357-auxadc"
	}, {
		.name = "mt6357-regulator",
	}, {
		.name = "mt6357-rtc",
		.num_resources = ARRAY_SIZE(mt6357_rtc_resources),
		.resources = mt6357_rtc_resources,
		.of_compatible = "mediatek,mt6357-rtc",
	}, {
		.name = "mt6357-sound",
		.of_compatible = "mediatek,mt6357-sound"
	}, {
		.name = "mt6357-keys",
		.num_resources = ARRAY_SIZE(mt6357_keys_resources),
		.resources = mt6357_keys_resources,
		.of_compatible = "mediatek,mt6357-keys"
	},
};

/* MT6331 is always used in combination with MT6332 */
static const struct mfd_cell mt6331_mt6332_devs[] = {
	{
		.name = "mt6331-rtc",
		.num_resources = ARRAY_SIZE(mt6331_rtc_resources),
		.resources = mt6331_rtc_resources,
		.of_compatible = "mediatek,mt6331-rtc",
	}, {
		.name = "mt6331-regulator",
		.of_compatible = "mediatek,mt6331-regulator"
	}, {
		.name = "mt6332-regulator",
		.of_compatible = "mediatek,mt6332-regulator"
	}, {
		.name = "mt6331-keys",
		.num_resources = ARRAY_SIZE(mt6331_keys_resources),
		.resources = mt6331_keys_resources,
		.of_compatible = "mediatek,mt6331-keys"
	},
};

static const struct mfd_cell mt6358_devs[] = {
	{
		.name = "mt6359-auxadc",
		.of_compatible = "mediatek,mt6358-auxadc"
	}, {
		.name = "mt6358-regulator",
		.of_compatible = "mediatek,mt6358-regulator"
	}, {
		.name = "mt6358-rtc",
		.num_resources = ARRAY_SIZE(mt6358_rtc_resources),
		.resources = mt6358_rtc_resources,
		.of_compatible = "mediatek,mt6358-rtc",
	}, {
		.name = "mt6358-sound",
		.of_compatible = "mediatek,mt6358-sound"
	}, {
		.name = "mt6358-keys",
		.num_resources = ARRAY_SIZE(mt6358_keys_resources),
		.resources = mt6358_keys_resources,
		.of_compatible = "mediatek,mt6358-keys"
	},
};

static const struct mfd_cell mt6359_devs[] = {
	{
		.name = "mt6359-auxadc",
		.of_compatible = "mediatek,mt6359-auxadc"
	},
	{ .name = "mt6359-regulator", },
	{
		.name = "mt6359-rtc",
		.num_resources = ARRAY_SIZE(mt6358_rtc_resources),
		.resources = mt6358_rtc_resources,
		.of_compatible = "mediatek,mt6358-rtc",
	},
	{ .name = "mt6359-sound", },
	{
		.name = "mt6359-keys",
		.num_resources = ARRAY_SIZE(mt6359_keys_resources),
		.resources = mt6359_keys_resources,
		.of_compatible = "mediatek,mt6359-keys"
	},
	{
		.name = "mt6359-accdet",
		.of_compatible = "mediatek,mt6359-accdet",
		.num_resources = ARRAY_SIZE(mt6359_accdet_resources),
		.resources = mt6359_accdet_resources,
	},
};

static const struct mfd_cell mt6397_devs[] = {
	{
		.name = "mt6397-rtc",
		.num_resources = ARRAY_SIZE(mt6397_rtc_resources),
		.resources = mt6397_rtc_resources,
		.of_compatible = "mediatek,mt6397-rtc",
	}, {
		.name = "mt6397-regulator",
		.of_compatible = "mediatek,mt6397-regulator",
	}, {
		.name = "mt6397-codec",
		.of_compatible = "mediatek,mt6397-codec",
	}, {
		.name = "mt6397-clk",
		.of_compatible = "mediatek,mt6397-clk",
	}, {
		.name = "mt6397-pinctrl",
		.of_compatible = "mediatek,mt6397-pinctrl",
	}, {
		.name = "mt6397-keys",
		.num_resources = ARRAY_SIZE(mt6397_keys_resources),
		.resources = mt6397_keys_resources,
		.of_compatible = "mediatek,mt6397-keys"
	}
};

struct chip_data {
	u32 cid_addr;
	u32 cid_shift;
	const struct mfd_cell *cells;
	int cell_size;
	int (*irq_init)(struct mt6397_chip *chip);
	void (*irq_exit)(struct mt6397_chip *chip);
	/*
	 * PMIC nIRQ is optional (storage-only support). Only the MT6353 sets
	 * this; every other chip keeps the historical mandatory-IRQ path.
	 */
	bool no_irq;
};

static const struct chip_data mt6323_core = {
	.cid_addr = MT6323_CID,
	.cid_shift = 0,
	.cells = mt6323_devs,
	.cell_size = ARRAY_SIZE(mt6323_devs),
	.irq_init = mt6397_irq_init,
};

static const struct chip_data mt6328_core = {
	.cid_addr = MT6328_HWCID,
	.cid_shift = 0,
	.cells = mt6328_devs,
	.cell_size = ARRAY_SIZE(mt6328_devs),
	.irq_init = mt6397_irq_init,
};

static const struct chip_data mt6351_core = {
	.cid_addr = MT6351_SWCID,
	.cid_shift = 8,
	.cells = mt6351_devs,
	.cell_size = ARRAY_SIZE(mt6351_devs),
	.irq_init = mt6351_irq_init,
	.irq_exit = mt6351_irq_exit,
};

/*
 * MT6353 (PMIC of the MT6750/MT6755 Meizu boards, e.g. U10).
 *
 * Storage-only: no interrupt controller is registered. The legacy MT6353
 * interrupt layout is NOT compatible with any mt6397-family irq_init:
 *  - the legacy bank registers are INT_CON0 = 0x02b8 and INT_STATUS0 =
 *    0x02d6 (mt6353_hw.h:169,184), while MT6351 uses 0x02c2/0x02e0
 *    (include/linux/mfd/mt6351/registers.h);
 *  - bank 0 bit 8 is reserved and RTC sits at bit 9 (pmic_irq.c:109-126),
 *    whereas MT6351 has RTC at bit 8 and MAD at bit 10;
 *  - bank 1 is VCORE/VPROC/VS1/VPA/SPKL_D/SPKL_AB in the MT6353 BSP
 *    (pmic_irq.c:128-145), not MT6351's VCORE/VGPU/VSRAM_MD/VMODEM.
 * VEMC/VMCH/VMC need no PMIC interrupt, so the cell is registered without
 * an IRQ domain. No EINT number for the MT6353 nIRQ line is proven, so the
 * board DT must not describe one (UNKNOWN).
 *
 * HYPOTHESIS: SWCID uses the same [15:8] field position as MT6351 (register
 * 0x0202, cid_shift 8); the BSP only logs the raw 16-bit value.
 */
static const struct chip_data mt6353_core = {
	.cid_addr = MT6353_SWCID,
	.cid_shift = 8,
	.cells = mt6353_devs,
	.cell_size = ARRAY_SIZE(mt6353_devs),
	.no_irq = true,
};

static const struct chip_data mt6357_core = {
	.cid_addr = MT6357_SWCID,
	.cid_shift = 8,
	.cells = mt6357_devs,
	.cell_size = ARRAY_SIZE(mt6357_devs),
	.irq_init = mt6358_irq_init,
};

static const struct chip_data mt6331_mt6332_core = {
	.cid_addr = MT6331_HWCID,
	.cid_shift = 0,
	.cells = mt6331_mt6332_devs,
	.cell_size = ARRAY_SIZE(mt6331_mt6332_devs),
	.irq_init = mt6397_irq_init,
};

static const struct chip_data mt6358_core = {
	.cid_addr = MT6358_SWCID,
	.cid_shift = 8,
	.cells = mt6358_devs,
	.cell_size = ARRAY_SIZE(mt6358_devs),
	.irq_init = mt6358_irq_init,
};

static const struct chip_data mt6359_core = {
	.cid_addr = MT6359_SWCID,
	.cid_shift = 8,
	.cells = mt6359_devs,
	.cell_size = ARRAY_SIZE(mt6359_devs),
	.irq_init = mt6358_irq_init,
};

static const struct chip_data mt6397_core = {
	.cid_addr = MT6397_CID,
	.cid_shift = 0,
	.cells = mt6397_devs,
	.cell_size = ARRAY_SIZE(mt6397_devs),
	.irq_init = mt6397_irq_init,
};

static int mt6397_probe(struct platform_device *pdev)
{
	int ret;
	unsigned int id = 0;
	struct mt6397_chip *pmic;
	const struct chip_data *pmic_core;

	pmic = devm_kzalloc(&pdev->dev, sizeof(*pmic), GFP_KERNEL);
	if (!pmic)
		return -ENOMEM;

	pmic->dev = &pdev->dev;

	/*
	 * mt6397 MFD is child device of soc pmic wrapper.
	 * Regmap is set from its parent.
	 */
	pmic->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!pmic->regmap)
		return -ENODEV;

	pmic_core = of_device_get_match_data(&pdev->dev);
	if (!pmic_core)
		return -ENODEV;

	ret = regmap_read(pmic->regmap, pmic_core->cid_addr, &id);
	if (ret) {
		dev_err(&pdev->dev, "Failed to read chip id: %d\n", ret);
		return ret;
	}

	pmic->chip_id = (id >> pmic_core->cid_shift) & 0xff;

	platform_set_drvdata(pdev, pmic);

	if (pmic_core->no_irq) {
		/*
		 * Storage-only PMIC: the nIRQ line is optional. A board may
		 * still wire it later; if it does, the IRQ is recorded but no
		 * irq chip is registered (this chip_data has no irq_init).
		 */
		pmic->irq = platform_get_irq_optional(pdev, 0);
		if (pmic->irq < 0)
			pmic->irq = 0;
	} else {
		pmic->irq = platform_get_irq(pdev, 0);
		if (pmic->irq <= 0)
			return pmic->irq;
	}

	if (pmic_core->irq_init) {
		ret = pmic_core->irq_init(pmic);
		if (ret)
			return ret;
	}

	ret = devm_mfd_add_devices(&pdev->dev, PLATFORM_DEVID_NONE,
				   pmic_core->cells, pmic_core->cell_size,
				   NULL, 0, pmic->irq_domain);
	if (ret) {
		if (pmic_core->irq_exit)
			pmic_core->irq_exit(pmic);
		else if (pmic->irq_domain)
			irq_domain_remove(pmic->irq_domain);
		dev_err(&pdev->dev, "failed to add child devices: %d\n", ret);
	}

	return ret;
}

static const struct of_device_id mt6397_of_match[] = {
	{
		.compatible = "mediatek,mt6323",
		.data = &mt6323_core,
	}, {
		.compatible = "mediatek,mt6328",
		.data = &mt6328_core,
	}, {
		.compatible = "mediatek,mt6331",
		.data = &mt6331_mt6332_core,
	}, {
		.compatible = "mediatek,mt6351",
		.data = &mt6351_core,
	}, {
		.compatible = "mediatek,mt6353",
		.data = &mt6353_core,
	}, {
		.compatible = "mediatek,mt6357",
		.data = &mt6357_core,
	}, {
		.compatible = "mediatek,mt6358",
		.data = &mt6358_core,
	}, {
		.compatible = "mediatek,mt6359",
		.data = &mt6359_core,
	}, {
		.compatible = "mediatek,mt6397",
		.data = &mt6397_core,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, mt6397_of_match);

static const struct platform_device_id mt6397_id[] = {
	{ "mt6397", 0 },
	{ },
};
MODULE_DEVICE_TABLE(platform, mt6397_id);

static struct platform_driver mt6397_driver = {
	.probe = mt6397_probe,
	.driver = {
		.name = "mt6397",
		.of_match_table = mt6397_of_match,
	},
	.id_table = mt6397_id,
};

module_platform_driver(mt6397_driver);

MODULE_AUTHOR("Flora Fu, MediaTek");
MODULE_DESCRIPTION("Driver for MediaTek MT6397 PMIC");
MODULE_LICENSE("GPL");
