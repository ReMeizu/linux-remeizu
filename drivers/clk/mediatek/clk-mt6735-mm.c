// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6735/MT6737M (m5c) MMSYS clock gates.
 *
 * Register offsets and bit positions are taken from the vendor 4.9 clock
 * manager:
 *   drivers/misc/mediatek/include/mt-plat/mt6735/include/mach/mt_clkmgr*.h
 *     DISP_CG_CON0/SET0/CLR0 = mmsys + 0x100/0x104/0x108
 *     DISP_CG_CON1/SET1/CLR1 = mmsys + 0x110/0x114/0x118
 *   drivers/misc/mediatek/base/power/mt6735/mt_clk_id.h
 *     DISP0 bits 0,1,10..18; DISP1 bits 2 (DSI engine), 3 (DSI digital)
 *
 * SMI_COMMON and SMI_LARB0 are CLK_IS_CRITICAL.  The mainline mtk-smi
 * larb0 device now exists, but it is only enabled through an IOMMU master
 * and MT6735 has no mainline IOMMU yet; nothing else would keep these gates
 * on for the DDP DMA engines (OVL/RDMA -> LARB0 -> SMI common).
 * This is the same bring-up arrangement as clk-mt6755-mm.c.
 */

#include <linux/clk-provider.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mediatek,mt6735-clk.h>

static const struct mtk_gate_regs mm0_cg_regs = {
	.set_ofs = 0x104,
	.clr_ofs = 0x108,
	.sta_ofs = 0x100,
};

static const struct mtk_gate_regs mm1_cg_regs = {
	.set_ofs = 0x114,
	.clr_ofs = 0x118,
	.sta_ofs = 0x110,
};

#define GATE_MM0(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &mm0_cg_regs, _shift,		\
		 &mtk_clk_gate_ops_setclr)

#define GATE_MM0_CRIT(_id, _name, _parent, _shift)			\
	GATE_MTK_FLAGS(_id, _name, _parent, &mm0_cg_regs, _shift,	\
		       &mtk_clk_gate_ops_setclr, CLK_IS_CRITICAL)

#define GATE_MM1(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &mm1_cg_regs, _shift,		\
		 &mtk_clk_gate_ops_setclr)

static const struct mtk_gate mm_clks[] = {
	/* DISP0 */
	GATE_MM0_CRIT(CLK_MM_DISP0_SMI_COMMON, "mm_smi_common", "mm_sel", 0),
	GATE_MM0_CRIT(CLK_MM_DISP0_SMI_LARB0, "mm_smi_larb0", "mm_sel", 1),
	GATE_MM0(CLK_MM_DISP0_DISP_OVL0, "mm_disp_ovl0", "mm_sel", 10),
	GATE_MM0(CLK_MM_DISP0_DISP_RDMA0, "mm_disp_rdma0", "mm_sel", 11),
	GATE_MM0(CLK_MM_DISP0_DISP_COLOR, "mm_disp_color", "mm_sel", 14),
	GATE_MM0(CLK_MM_DISP0_DISP_CCORR, "mm_disp_ccorr", "mm_sel", 15),
	GATE_MM0(CLK_MM_DISP0_DISP_AAL, "mm_disp_aal", "mm_sel", 16),
	GATE_MM0(CLK_MM_DISP0_DISP_GAMMA, "mm_disp_gamma", "mm_sel", 17),
	GATE_MM0(CLK_MM_DISP0_DISP_DITHER, "mm_disp_dither", "mm_sel", 18),
	/* DISP1 */
	GATE_MM1(CLK_MM_DISP1_DSI_ENGINE, "mm_dsi_engine", "mm_sel", 2),
	GATE_MM1(CLK_MM_DISP1_DSI_DIGITAL, "mm_dsi_digital", "mm_sel", 3),
};

static const struct mtk_clk_desc mm_desc = {
	.clks = mm_clks,
	.num_clks = ARRAY_SIZE(mm_clks),
};

static const struct platform_device_id clk_mt6735_mm_id_table[] = {
	{ .name = "clk-mt6735-mm", .driver_data = (kernel_ulong_t)&mm_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, clk_mt6735_mm_id_table);

static struct platform_driver clk_mt6735_mm_drv = {
	.probe = mtk_clk_pdev_probe,
	.remove = mtk_clk_pdev_remove,
	.driver = {
		.name = "clk-mt6735-mm",
	},
	.id_table = clk_mt6735_mm_id_table,
};
module_platform_driver(clk_mt6735_mm_drv);

MODULE_DESCRIPTION("MediaTek MT6735 MultiMedia clocks driver");
MODULE_LICENSE("GPL");
