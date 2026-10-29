// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6755 mfgcfg (GPU) clock gate — m681 bring-up.
 *
 * VERIFIED-FROM-STOCK (kernel-m681-m6base-3.18.140/kernel-3.18/
 * drivers/clk/mediatek/clk-mt6755.c):
 *   mfg_cg_regs  = { set 0x4, clr 0x8, sta 0x0 }   (line 1427)
 *   GATE(MFG_BG3D, "mfg_bg3d", "mfg_sel", shift 0) (line 1434)
 * mfgcfg base 0x13000000 lives in the MFG_ASYNC MTCMOS domain, so the DT
 * node carries power-domains and the desc uses need_runtime_pm.
 */

#include <linux/clk-provider.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/mediatek,mt6755-clk.h>

#include "clk-mtk.h"
#include "clk-gate.h"

static const struct mtk_gate_regs mfg_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_MFG(_id, _name, _parent, _shift)				\
	GATE_MTK_FLAGS(_id, _name, _parent, &mfg_cg_regs, _shift,	\
		       &mtk_clk_gate_ops_setclr, CLK_SET_RATE_PARENT)

static const struct mtk_gate mfg_clks[] = {
	GATE_MFG(CLK_MFG_BG3D, "mfg_bg3d", "mfg_sel", 0),
};

static const struct mtk_clk_desc mfg_desc = {
	.clks = mfg_clks,
	.num_clks = ARRAY_SIZE(mfg_clks),
	/*
	 * NO need_runtime_pm (unlike mt8183-mfgcfg), ON PURPOSE for m681:
	 * mtk_clk_simple_probe does pm_runtime_resume_and_get() at probe when
	 * need_runtime_pm is set, which powers the MFG_ASYNC MTCMOS on/off at
	 * BOOT. On m681 that power-on sequence intermittently hard-hangs boot
	 * (r48-r53 all wedged at the logo with it enabled; r49 only booted on
	 * a lucky attempt — the r53 net-diff vs booting-r49 was JUST the clk
	 * id, proving the hang is the boot-time MFG power-on, not the id).
	 * Instead the provider registers INERTLY (no domain touch at boot) and
	 * MFG_ASYNC is powered by the CONSUMER: panfrost owns power-domains=MFG
	 * (child of MFG_ASYNC); genpd powers MFG_ASYNC->MFG at panfrost's probe
	 * BEFORE it enables BG3D, so the gate write at 0x13000000 is covered.
	 * This exercises the MFG power-on only at `modprobe panfrost` over live
	 * ssh (observable), never at boot. clk_ignore_unused keeps the boot
	 * disable-sweep off this gate too.
	 */
};

static const struct of_device_id of_match_clk_mt6755_mfg[] = {
	{
		.compatible = "mediatek,mt6755-mfgcfg",
		.data = &mfg_desc,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6755_mfg);

static struct platform_driver clk_mt6755_mfg_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6755-mfg",
		.of_match_table = of_match_clk_mt6755_mfg,
	},
};
module_platform_driver(clk_mt6755_mfg_drv);

MODULE_DESCRIPTION("MediaTek MT6755 GPU mfg clocks driver");
MODULE_LICENSE("GPL");
