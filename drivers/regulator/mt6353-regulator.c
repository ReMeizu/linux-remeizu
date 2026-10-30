// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6353 storage LDOs: VEMC, VMCH and VMC.
 *
 * Register addresses, enable bits and voltage tables come from the
 * MediaTek 3.18 MT6353 BSP driver, not from the MT6351/MT6358 mainline
 * drivers (their register maps are different):
 *
 *   VEMC: MT6353_LDO_VEMC33_CON0 (0x0a8a) bit 1 enable,
 *         MT6353_DLDO_ANA_CON8 (0x0ab6) bits [9:8] voltage;
 *         table vemc33_voltages[] = {2.9, 3.0, 3.3} V
 *         (drivers/misc/mediatek/pmic/mt6353/regulator_codegen.c:222-226,
 *          mt6353_hw.h:448,470,6020-6021,6254-6256)
 *   VMCH: MT6353_LDO_VMCH_CON0 (0x0a80) bit 1 enable,
 *         MT6353_DLDO_ANA_CON6 (0x0ab2) bits [9:8] voltage;
 *         table vmch_voltages[] = {2.9, 3.0, 3.3} V
 *         (regulator_codegen.c:210-214, mt6353_hw.h:443,468,5937-5938,
 *          6230-6232)
 *   VMC:  MT6353_LDO_VMC_CON0 (0x0a7a) bit 1 enable,
 *         MT6353_DLDO_ANA_CON14 (0x0ac2) bits [10:8] voltage;
 *         table vmc_voltages[] = {1.2, 1.3, 1.5, 1.8, 2.5, 2.9, 3.0,
 *          3.3} V (regulator_codegen.c:198-208, mt6353_hw.h:440,476,
 *          5883-5884, 6311-6313)
 *
 * The 2-bit VEMC/VMCH selectors encode only three of their four values in
 * the BSP table; selector 3 has no documented voltage, so n_voltages is 3
 * and regulator_map_voltage_iterate() will never pick it. HYPOTHESIS: the
 * BSP treats selector 3 as reserved (it would read past the table).
 *
 * This is a source port. It has not been run on hardware.
 */

#include <linux/module.h>
#include <linux/mfd/mt6353/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

static const unsigned int mt6353_vemc_voltages[] = {
	2900000, 3000000, 3300000,
};

static const unsigned int mt6353_vmch_voltages[] = {
	2900000, 3000000, 3300000,
};

static const unsigned int mt6353_vmc_voltages[] = {
	1200000, 1300000, 1500000, 1800000,
	2500000, 2900000, 3000000, 3300000,
};

static const struct regulator_ops mt6353_ldo_ops = {
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

/*
 * All three rails have their enable bit at bit 1 of their LDO*_CON0
 * register (mt6353_hw.h:5884, 5938, 6021).
 */
#define MT6353_LDO(_id, _name, _en, _vsel, _mask) {		\
	.name = #_name,						\
	.of_match = "ldo-" #_name,				\
	.regulators_node = "regulators",			\
	.id = _id,						\
	.ops = &mt6353_ldo_ops,					\
	.type = REGULATOR_VOLTAGE,				\
	.owner = THIS_MODULE,					\
	.n_voltages = ARRAY_SIZE(mt6353_##_name##_voltages),	\
	.volt_table = mt6353_##_name##_voltages,		\
	.enable_reg = _en,					\
	.enable_mask = BIT(1),					\
	.vsel_reg = _vsel,					\
	.vsel_mask = _mask,					\
}

static const struct regulator_desc mt6353_regulators[] = {
	MT6353_LDO(0, vemc, MT6353_LDO_VEMC33_CON0,
		   MT6353_VEMC33_ANA_CON0, GENMASK(9, 8)),
	MT6353_LDO(1, vmch, MT6353_LDO_VMCH_CON0,
		   MT6353_VMCH_ANA_CON0, GENMASK(9, 8)),
	MT6353_LDO(2, vmc, MT6353_LDO_VMC_CON0,
		   MT6353_VMC_ANA_CON0, GENMASK(10, 8)),
};

static int mt6353_regulator_probe(struct platform_device *pdev)
{
	struct mt6397_chip *chip = dev_get_drvdata(pdev->dev.parent);
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	unsigned int swcid;
	int i, ret;

	if (!chip || !chip->regmap)
		return -ENODEV;

	/*
	 * Do not reject the chip here: the SWCID register address is proven
	 * (0x0202, mt6353_hw.h:78) but the expected MT6353 SWCID value is
	 * not documented in the BSP. Log it instead; UNKNOWN.
	 */
	ret = regmap_read(chip->regmap, MT6353_SWCID, &swcid);
	if (ret)
		return ret;
	dev_info(&pdev->dev, "MT6353 SWCID %#06x\n", swcid);

	/*
	 * The MFD cell carries no of_compatible (same convention as the
	 * MT6351 cell in mt6397-core.c), so inherit the PMIC node from the
	 * parent and look up the "regulators" child from there.
	 */
	device_set_of_node_from_dev(&pdev->dev, pdev->dev.parent);
	config.dev = &pdev->dev;
	config.regmap = chip->regmap;

	for (i = 0; i < ARRAY_SIZE(mt6353_regulators); i++) {
		rdev = devm_regulator_register(&pdev->dev, &mt6353_regulators[i],
					       &config);
		if (IS_ERR(rdev))
			return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
					     "failed to register %s\n",
					     mt6353_regulators[i].name);
	}

	return 0;
}

static const struct platform_device_id mt6353_regulator_ids[] = {
	{ "mt6353-regulator" },
	{ }
};
MODULE_DEVICE_TABLE(platform, mt6353_regulator_ids);

static struct platform_driver mt6353_regulator_driver = {
	.driver = { .name = "mt6353-regulator" },
	.probe = mt6353_regulator_probe,
	.id_table = mt6353_regulator_ids,
};
module_platform_driver(mt6353_regulator_driver);

MODULE_DESCRIPTION("MediaTek MT6353 storage regulator driver");
MODULE_LICENSE("GPL");
