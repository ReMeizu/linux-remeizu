// SPDX-License-Identifier: GPL-2.0-only
/* MT6351 storage LDOs, using register/voltage data from MediaTek's BSP. */
/* Copyright (C) 2015 MediaTek Inc. */

#include <linux/module.h>
#include <linux/mfd/mt6351/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

static const unsigned int mt6351_vemc_voltages[] = { 3000000, 3300000 };
static const unsigned int mt6351_vmch_voltages[] = { 3000000, 3300000 };
static const unsigned int mt6351_vmc_voltages[] = {
	1200000, 1300000, 1500000, 1800000,
	2000000, 2900000, 3000000, 3300000,
};

static const struct regulator_ops mt6351_ldo_ops = {
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

#define MT6351_LDO(_id, _name, _en, _vsel, _mask) { \
	.name = #_name, \
	.of_match = "ldo-" #_name, \
	.regulators_node = "regulators", \
	.id = _id, \
	.ops = &mt6351_ldo_ops, \
	.type = REGULATOR_VOLTAGE, \
	.owner = THIS_MODULE, \
	.n_voltages = ARRAY_SIZE(mt6351_##_name##_voltages), \
	.volt_table = mt6351_##_name##_voltages, \
	.enable_reg = _en, \
	.enable_mask = BIT(1), \
	.vsel_reg = _vsel, \
	.vsel_mask = _mask, \
}

static const struct regulator_desc mt6351_regulators[] = {
	MT6351_LDO(0, vemc, MT6351_LDO_VEMC_CON0,
		   MT6351_VEMC_ANA_CON0, BIT(8)),
	MT6351_LDO(1, vmch, MT6351_LDO_VMCH_CON0,
		   MT6351_VMCH_ANA_CON0, BIT(8)),
	MT6351_LDO(2, vmc, MT6351_LDO_VMC_CON0,
		   MT6351_VMC_ANA_CON0, GENMASK(10, 8)),
};

static int mt6351_regulator_probe(struct platform_device *pdev)
{
	struct mt6397_chip *chip = dev_get_drvdata(pdev->dev.parent);
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	unsigned int revision;
	int i, ret;

	if (!chip || !chip->regmap)
		return -ENODEV;

	ret = regmap_read(chip->regmap, MT6351_SWCID, &revision);
	if (ret)
		return ret;

	/* E1 has different VMC tables; only the proven E2/E3 map is exposed. */
	if (revision != 0x5120 && revision != 0x5130)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "unsupported PMIC revision %#x\n", revision);

	device_set_of_node_from_dev(&pdev->dev, pdev->dev.parent);
	config.dev = &pdev->dev;
	config.regmap = chip->regmap;

	for (i = 0; i < ARRAY_SIZE(mt6351_regulators); i++) {
		rdev = devm_regulator_register(&pdev->dev,
					      &mt6351_regulators[i], &config);
		if (IS_ERR(rdev))
			return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
					     "failed to register %s\n",
					     mt6351_regulators[i].name);
	}

	return 0;
}

static const struct platform_device_id mt6351_regulator_ids[] = {
	{ "mt6351-regulator" },
	{ }
};
MODULE_DEVICE_TABLE(platform, mt6351_regulator_ids);

static struct platform_driver mt6351_regulator_driver = {
	.driver = { .name = "mt6351-regulator" },
	.probe = mt6351_regulator_probe,
	.id_table = mt6351_regulator_ids,
};
module_platform_driver(mt6351_regulator_driver);

MODULE_DESCRIPTION("MediaTek MT6351 storage regulator driver");
MODULE_LICENSE("GPL");
