// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6755 (Helio P10) MMSYS clock gates.
 *
 * VERIFIED-FROM-STOCK: register offsets and bit positions are taken 1:1 from
 * the stock 3.18 BSP gate table:
 *   kernel-m681-m6base-3.18.140/kernel-3.18/drivers/clk/mediatek/clk-mt6755.c
 *     mm0_cg_regs  (sta 0x100 / set 0x104 / clr 0x108)   -> lines 1474-1478
 *     mm1_cg_regs  (sta 0x110 / set 0x114 / clr 0x118)   -> lines 1480-1484
 *     mm_clks[]    (bit positions, parents)              -> lines 1486-1534
 *
 * Only the *canonical* gate per CG bit is registered.  The stock tree also
 * registered ~15 alias clock IDs that map onto the same bits (e.g.
 * CLK_MM_DISP0_SMI_COMMON_M4U == bit 0); registering several CCF gates on one
 * set/clr bit is dangerous in mainline (disabling one alias would gate the
 * bit even while the canonical clock is enabled), so aliases are left
 * unregistered on purpose.  The dt-binding IDs for them still exist in
 * include/dt-bindings/clock/mediatek,mt6755-clk.h but must not be referenced.
 *
 * Because the alias IDs are skipped, the gate IDs used here are *sparse* and
 * reach up to CLK_MM_DISP1_DPI_PIXEL (44) while only 28 gates are registered.
 * mtk_clk_register_gates() writes clk_data->hws[gate->id] with no bounds
 * check, and the generic mtk_clk_pdev_probe() sizes the onecell array to the
 * *number of gate entries* (28) - that would overflow for our sparse IDs.  So
 * this driver uses a small custom probe that allocates CLK_MM_NR_CLK slots and
 * then registers the (sparse) gate table into it.  (The generic probe is only
 * safe when entry_count == max_id + 1, which is why clk-mt6795-mm.c can use
 * it: it registers every ID densely.)
 *
 * BRING-UP HACK (marked): SMI_COMMON, SMI_LARB0, DISP_OVL0_MOUT and
 * DISP_UFOE_MOUT are CLK_IS_CRITICAL for now:
 *  - smi_common/smi_larb0: no mtk-smi/iommu integration yet, but every DDP
 *    DMA engine (OVL/RDMA) moves data through LARB0 -> SMI common.  If these
 *    gates close, OVL/RDMA stall (and register access into the larb can hang
 *    the bus).
 *  - ovl0_mout/ufoe_mout: pure routing stages of the main path
 *    OVL0->...->RDMA0->UFOE(mout)->DSI0.  Mainline mtk-drm has no consumer
 *    for "mout" gates; stock dispsys always enabled them
 *    (stock mt6755.dtsi dispsys node, clocks DISP0_DISP_OVL0_MOUT /
 *    DISP0_DISP_UFOE_MOUT).
 * Proper fix later: mtk-smi larb/common devices + hanging the mout gates off
 * the mmsys/mutex node.
 *
 * NOTE: enabling the CLK_IS_CRITICAL gates writes the MMSYS CG set register at
 * 0x14000104, which only works while the DISP MTCMOS domain is powered.  LK
 * leaves DISP on and the kernel must be booted with "pd_ignore_unused" so the
 * genpd unused-sweep does not power it off (see BRINGUP_PLAN.md).
 */

#include <linux/clk-provider.h>
#include <linux/of_clk.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <dt-bindings/clock/mediatek,mt6755-clk.h>

#include "clk-mtk.h"
#include "clk-gate.h"

static const struct mtk_gate_regs mm0_cg_regs = {
	.set_ofs = 0x0104,
	.clr_ofs = 0x0108,
	.sta_ofs = 0x0100,
};

static const struct mtk_gate_regs mm1_cg_regs = {
	.set_ofs = 0x0114,
	.clr_ofs = 0x0118,
	.sta_ofs = 0x0110,
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
	/* MM0 */
	/* BRING-UP: critical, see header comment */
	GATE_MM0_CRIT(CLK_MM_DISP0_SMI_COMMON, "mm_smi_common", "mm_sel", 0),
	GATE_MM0_CRIT(CLK_MM_DISP0_SMI_LARB0, "mm_smi_larb0", "mm_sel", 1),
	GATE_MM0(CLK_MM_DISP0_CAM_MDP, "mm_cam_mdp", "mm_sel", 2),
	GATE_MM0(CLK_MM_DISP0_MDP_RDMA, "mm_mdp_rdma", "mm_sel", 3),
	GATE_MM0(CLK_MM_DISP0_MDP_RSZ0, "mm_mdp_rsz0", "mm_sel", 4),
	GATE_MM0(CLK_MM_DISP0_MDP_RSZ1, "mm_mdp_rsz1", "mm_sel", 5),
	GATE_MM0(CLK_MM_DISP0_MDP_TDSHP, "mm_mdp_tdshp", "mm_sel", 6),
	GATE_MM0(CLK_MM_DISP0_MDP_WDMA, "mm_mdp_wdma", "mm_sel", 7),
	GATE_MM0(CLK_MM_DISP0_MDP_WROT, "mm_mdp_wrot", "mm_sel", 8),
	GATE_MM0(CLK_MM_DISP0_FAKE_ENG, "mm_fake_eng", "mm_sel", 9),
	GATE_MM0(CLK_MM_DISP0_DISP_OVL0, "mm_disp_ovl0", "mm_sel", 10),
	GATE_MM0(CLK_MM_DISP0_DISP_OVL1, "mm_disp_ovl1", "mm_sel", 11),
	GATE_MM0(CLK_MM_DISP0_DISP_RDMA0, "mm_disp_rdma0", "mm_sel", 12),
	GATE_MM0(CLK_MM_DISP0_DISP_RDMA1, "mm_disp_rdma1", "mm_sel", 13),
	GATE_MM0(CLK_MM_DISP0_DISP_WDMA0, "mm_disp_wdma0", "mm_sel", 14),
	GATE_MM0(CLK_MM_DISP0_DISP_COLOR, "mm_disp_color", "mm_sel", 15),
	GATE_MM0(CLK_MM_DISP0_DISP_CCORR, "mm_disp_ccorr", "mm_sel", 16),
	GATE_MM0(CLK_MM_DISP0_DISP_AAL, "mm_disp_aal", "mm_sel", 17),
	GATE_MM0(CLK_MM_DISP0_DISP_GAMMA, "mm_disp_gamma", "mm_sel", 18),
	GATE_MM0(CLK_MM_DISP0_DISP_DITHER, "mm_disp_dither", "mm_sel", 19),
	GATE_MM0(CLK_MM_DISP0_MDP_COLOR, "mm_mdp_color", "mm_sel", 20),
	/* BRING-UP: critical, routing gates of the main path */
	GATE_MM0_CRIT(CLK_MM_DISP0_DISP_UFOE_MOUT, "mm_ufoe_mout", "mm_sel", 21),
	GATE_MM0(CLK_MM_DISP0_DISP_WDMA1, "mm_disp_wdma1", "mm_sel", 22),
	GATE_MM0(CLK_MM_DISP0_DISP_2L_OVL0, "mm_disp_2lovl0", "mm_sel", 23),
	GATE_MM0(CLK_MM_DISP0_DISP_2L_OVL1, "mm_disp_2lovl1", "mm_sel", 24),
	GATE_MM0_CRIT(CLK_MM_DISP0_DISP_OVL0_MOUT, "mm_ovl0_mout", "mm_sel", 25),
	/* MM1 */
	GATE_MM1(CLK_MM_DISP1_DSI_ENGINE, "mm_dsi_engine", "mm_sel", 0),
	GATE_MM1(CLK_MM_DISP1_DSI_DIGITAL, "mm_dsi_digital", "mm_sel", 1),
	/*
	 * Stock parents dpi_pixel from "dpi0_sel" (clk-mt6755.c:1533); DPI is
	 * unused on m681 but the parent mux exists in topckgen so keep it.
	 */
	GATE_MM1(CLK_MM_DISP1_DPI_PIXEL, "mm_dpi_pixel", "dpi0_sel", 2),
	GATE_MM1(CLK_MM_DISP1_DPI_ENGINE, "mm_dpi_engine", "mm_sel", 3),
};

static int clk_mt6755_mm_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.parent->of_node;
	struct clk_hw_onecell_data *clk_data;
	int ret;

	/*
	 * Allocate the full ID space (CLK_MM_NR_CLK), not the number of gate
	 * entries: our gate IDs are sparse (aliases skipped) and
	 * mtk_clk_register_gates() indexes clk_data->hws[] by gate->id.
	 */
	clk_data = mtk_alloc_clk_data(CLK_MM_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	ret = mtk_clk_register_gates(&pdev->dev, node, mm_clks,
				     ARRAY_SIZE(mm_clks), clk_data);
	if (ret)
		goto free_data;

	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (ret)
		goto unregister_gates;

	platform_set_drvdata(pdev, clk_data);
	return 0;

unregister_gates:
	mtk_clk_unregister_gates(mm_clks, ARRAY_SIZE(mm_clks), clk_data);
free_data:
	mtk_free_clk_data(clk_data);
	return ret;
}

static void clk_mt6755_mm_remove(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);
	struct device_node *node = pdev->dev.parent->of_node;

	of_clk_del_provider(node);
	mtk_clk_unregister_gates(mm_clks, ARRAY_SIZE(mm_clks), clk_data);
	mtk_free_clk_data(clk_data);
}

static struct platform_driver clk_mt6755_mm_drv = {
	.probe = clk_mt6755_mm_probe,
	.remove = clk_mt6755_mm_remove,
	.driver = {
		.name = "clk-mt6755-mm",
	},
};
module_platform_driver(clk_mt6755_mm_drv);

MODULE_DESCRIPTION("MediaTek MT6755 MultiMedia clocks driver");
MODULE_LICENSE("GPL");
