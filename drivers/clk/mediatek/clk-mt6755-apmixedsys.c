// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Copyright (c) 2023 Arseniy Velikanov <adomerlee@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "clk-mtk.h"

#include <dt-bindings/clock/mediatek,mt6755-clk.h>

/*
 * Boot firmware may retain FHCTL ownership of the PLL DDS registers.
 * Expose measured PCW/postdivider rates without changing ownership or power.
 * Rate changes and enabling a powered-down PLL are deliberately unsupported
 * until FHCTL, tuner and power transitions have their own implementation.
 */
struct mt6755_pll_data {
	unsigned int id;
	const char *name;
	u16 con0;
	u16 power;
	u16 pd_reg;
	u8 pd_shift;
	u8 pcw_bits;
	u8 pcw_shift;
	u8 fractional_bits;
};

struct mt6755_pll {
	struct clk_hw hw;
	void __iomem *base;
	const struct mt6755_pll_data *data;
};

#define to_mt6755_pll(_hw) container_of(_hw, struct mt6755_pll, hw)
#define PLL(_id, _name, _con0, _power, _pd, _shift, _bits, _pcwshift, _frac) \
	{ _id, _name, _con0, _power, _pd, _shift, _bits, _pcwshift, _frac }

static const struct mt6755_pll_data mt6755_plls[] = {
	PLL(CLK_APMIXED_ARMBPLL, "armbpll", 0x200, 0x20c, 0x204, 24, 21, 0, 14),
	PLL(CLK_APMIXED_ARMSPLL, "armspll", 0x210, 0x21c, 0x214, 24, 21, 0, 14),
	PLL(CLK_APMIXED_MAINPLL, "mainpll", 0x220, 0x22c, 0x220, 4, 21, 0, 14),
	PLL(CLK_APMIXED_UNIVPLL, "univpll", 0x230, 0x23c, 0x230, 4, 7, 14, 0),
	PLL(CLK_APMIXED_MMPLL, "mmpll", 0x240, 0x24c, 0x244, 24, 21, 0, 14),
	PLL(CLK_APMIXED_MSDCPLL, "msdcpll", 0x250, 0x25c, 0x250, 4, 21, 0, 14),
	PLL(CLK_APMIXED_VENCPLL, "vencpll", 0x260, 0x26c, 0x260, 4, 21, 0, 14),
	PLL(CLK_APMIXED_TVDPLL, "tvdpll", 0x270, 0x27c, 0x270, 4, 21, 0, 14),
	PLL(CLK_APMIXED_APLL1, "apll1", 0x2a0, 0x2b0, 0x2a0, 4, 31, 0, 24),
	PLL(CLK_APMIXED_APLL2, "apll2", 0x2b4, 0x2c4, 0x2b4, 4, 31, 0, 24),
};

static int mt6755_pll_is_prepared(struct clk_hw *hw)
{
	struct mt6755_pll *pll = to_mt6755_pll(hw);
	u32 power = readl(pll->base + pll->data->power);

	return (power & (BIT(0) | BIT(1))) == BIT(0) &&
	       (readl(pll->base + pll->data->con0) & BIT(0));
}

static int mt6755_pll_prepare(struct clk_hw *hw)
{
	return mt6755_pll_is_prepared(hw) ? 0 : -EOPNOTSUPP;
}

static unsigned long mt6755_pll_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	struct mt6755_pll *pll = to_mt6755_pll(hw);
	const struct mt6755_pll_data *data = pll->data;
	u32 pcw = readl(pll->base + data->con0 + 4);
	u32 postdiv = (readl(pll->base + data->pd_reg) >> data->pd_shift) & 7;
	u64 rate;

	/* Only postdiv encodings 0..4 are defined by the hardware. */
	if (postdiv > 4 || !parent_rate)
		return 0;
	pcw = (pcw >> data->pcw_shift) & GENMASK(data->pcw_bits - 1, 0);
	rate = (u64)parent_rate * pcw;
	return DIV_ROUND_UP_ULL(rate, 1ULL << (data->fractional_bits + postdiv));
}

static const struct clk_ops mt6755_pll_ops = {
	.prepare = mt6755_pll_prepare,
	.is_prepared = mt6755_pll_is_prepared,
	.recalc_rate = mt6755_pll_recalc_rate,
};

static int mt6755_apmixedsys_probe(struct platform_device *pdev)
{
	const struct clk_parent_data parent = { .fw_name = "clk26m" };
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *clocks;
	void __iomem *base;
	int i, ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	clocks = devm_kzalloc(dev, struct_size(clocks, hws, CLK_APMIXED_NR_CLK),
			      GFP_KERNEL);
	if (!clocks)
		return -ENOMEM;
	clocks->num = CLK_APMIXED_NR_CLK;
	for (i = 0; i < clocks->num; i++)
		clocks->hws[i] = ERR_PTR(-ENOENT);

	for (i = 0; i < ARRAY_SIZE(mt6755_plls); i++) {
		const struct mt6755_pll_data *data = &mt6755_plls[i];
		struct clk_init_data init = {
			.name = data->name,
			.ops = &mt6755_pll_ops,
			.parent_data = &parent,
			.num_parents = 1,
			.flags = CLK_GET_RATE_NOCACHE,
		};
		struct mt6755_pll *pll;

		pll = devm_kzalloc(dev, sizeof(*pll), GFP_KERNEL);
		if (!pll)
			return -ENOMEM;
		pll->base = base;
		pll->data = data;
		pll->hw.init = &init;
		ret = devm_clk_hw_register(dev, &pll->hw);
		if (ret)
			return dev_err_probe(dev, ret, "cannot register %s\n", data->name);
		clocks->hws[data->id] = &pll->hw;
	}

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clocks);
}

static const struct of_device_id mt6755_apmixedsys_match[] = {
	{ .compatible = "mediatek,mt6755-apmixedsys" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6755_apmixedsys_match);

static struct platform_driver mt6755_apmixedsys_driver = {
	.probe = mt6755_apmixedsys_probe,
	.driver = {
		.name = "clk-mt6755-apmixedsys",
		.of_match_table = mt6755_apmixedsys_match,
	},
};
module_platform_driver(mt6755_apmixedsys_driver);

MODULE_DESCRIPTION("MediaTek MT6755 boot PLL rate provider");
MODULE_LICENSE("GPL");
