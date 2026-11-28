// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6797 (Helio X20) mfgcfg clock driver - Meizu MX6 / m95.
 *
 * The Mali-T880MP4 core clock is the single mfgcfg gate:
 *
 *   apmixedsys MFGPLL -> topckgen mfg_sel (CLK_TOP_MUX_MFG)
 *                     -> mfgcfg MFG_BG3D (CG 0x0, set 0x4, clr 0x8, bit 0)
 *
 * apmixedsys and topckgen are provided by clk-mt6797.c; this driver only
 * adds the mfgcfg gate, parented on the topckgen "mfg_sel" mux.
 *
 * VERIFIED-FROM-STOCK (kernel-m685-3.18
 * drivers/clk/mediatek/clk-mt6797.c): mfg_cg_regs = { sta 0x0, set 0x4,
 * clr 0x8 } and GATE(MFG_BG3D, "mfg_bg3d", "mfg_sel", 0) - the same
 * layout as the mt6735/mt6755 mfgcfg blocks.
 *
 * Like clk-mt6735-mfgcfg and clk-mt6755-mfg this provider registers
 * inertly (no need_runtime_pm): mfgcfg sits in the MFG_ASYNC MTCMOS
 * domain and is brought up by the GPU consumer (panfrost, through the
 * "mfg" power domain nested under "mfg_async") before it enables BG3D.
 */

#include <linux/clk-provider.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/mediatek,mt6797-mfgcfg.h>

#include "clk-gate.h"
#include "clk-mtk.h"

#define MFG_CG_CON			0x00
#define MFG_CG_SET			0x04
#define MFG_CG_CLR			0x08

static const struct mtk_gate_regs mfg_cg_regs = {
	.sta_ofs = MFG_CG_CON,
	.set_ofs = MFG_CG_SET,
	.clr_ofs = MFG_CG_CLR,
};

static const struct mtk_gate mfg_clks[] = {
	GATE_MTK(CLK_MFG_BG3D, "mfg_bg3d", "mfg_sel", &mfg_cg_regs, 0,
		 &mtk_clk_gate_ops_setclr),
};

static const struct mtk_clk_desc mfg_desc = {
	.clks = mfg_clks,
	.num_clks = ARRAY_SIZE(mfg_clks),
};

static const struct of_device_id of_match_clk_mt6797_mfg[] = {
	{
		.compatible = "mediatek,mt6797-mfgcfg",
		.data = &mfg_desc,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6797_mfg);

static struct platform_driver clk_mt6797_mfg_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6797-mfg",
		.of_match_table = of_match_clk_mt6797_mfg,
	},
};
module_platform_driver(clk_mt6797_mfg_drv);

MODULE_DESCRIPTION("MediaTek MT6797 GPU mfg clocks driver");
MODULE_LICENSE("GPL");
