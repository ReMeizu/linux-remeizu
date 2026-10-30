/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOC_MEDIATEK_MT6797_MMSYS_H
#define __SOC_MEDIATEK_MT6797_MMSYS_H

/*
 * MT6797 DDP routing registers (offsets into MMSYS_CONFIG @ 0x14000000).
 *
 * VERIFIED-FROM-STOCK:
 *   register offsets: meizu_mx6_m95 kernel/m685 3.18 tree,
 *     drivers/misc/mediatek/video/mt6797/dispsys/ddp_reg.h lines 1439-1466
 *   field widths:     same file, lines 1590-1606
 *   mout bit meanings and selector indices:
 *     same tree, dispsys/ddp_path.c lines 150-220 (mout_map[],
 *     sel_out_map[], sel_in_map[]); a SEL register is written with the
 *     INDEX into that map's option list (ddp_path.c ddp_connect_path_l)
 *   primary path order: ddp_path.c lines 70-77 (module_list_scenario
 *     [DDP_SCENARIO_PRIMARY_DISP])
 *
 * OVL cascade: MT6797 has no direct OVL0 -> COLOR0 mux. The stock chain is
 *
 *     OVL0 -> OVL0_2L -> OVL1_2L -> OVL0_VIRTUAL -> COLOR0
 *
 * where OVL0_VIRTUAL is a mux output, not a register block. mtk-drm can only
 * feed two overlay components per CRTC (mtk_crtc_num_comp_planes() returns 0
 * for comp_idx > 1, and comp[1] is the one that gets bgclr_in_on()), so this
 * table selects the two-stage variant that the same muxes already offer:
 *
 *     OVL0 -> OVL0_2L -> OVL0_VIRTUAL -> COLOR0
 *
 * i.e. OVL0_SOUT_SEL_IN = 0 ({OVL0_VIRTUAL, OVL1_2L}) and OVL0_SEL_IN = 0
 * ({OVL0_2L, OVL1, OVL1_2L}), leaving OVL1_2L out of the path. That gives
 * 4 + 2 = 6 overlay layers instead of the stock 8, and is the only cascade
 * shape mtk-drm can express today.
 *
 * OVL1_2L -> COLOR0 would instead need OVL1_SOUT_SEL_IN = 1 and
 * OVL0_SEL_IN = 2; both routes are valid hardware configurations, this table
 * just picks the one that matches the driver's component limit.
 *
 * The three OVL0_VIRTUAL writes are attached to the (OVL_2L0, COLOR0) route,
 * which is the last real block before COLOR0 (same technique as the mt6755
 * table uses for its blockless UFOE mout). DISP_PATH0 between RDMA0 and UFOE
 * is likewise a pair of muxes with no block, folded into (RDMA0, UFOE).
 */

/* MOUT_EN */
#define MT6797_DISP_REG_CONFIG_DISP_OVL0_MOUT_EN	0x034
#define MT6797_DISP_REG_CONFIG_DISP_OVL1_MOUT_EN	0x038
#define MT6797_DISP_REG_CONFIG_DISP_DITHER_MOUT_EN	0x03c
#define MT6797_DISP_REG_CONFIG_DISP_UFOE_MOUT_EN	0x040
#define MT6797_DISP_REG_CONFIG_DISP_DSC_MOUT_EN		0x044
/* SEL_IN */
#define MT6797_DISP_REG_CONFIG_DISP_COLOR0_SEL_IN	0x068
#define MT6797_DISP_REG_CONFIG_DISP_UFOE_SEL_IN		0x074
#define MT6797_DISP_REG_CONFIG_DISP_DSC_SEL_IN		0x078
#define MT6797_DISP_REG_CONFIG_DSI0_SEL_IN		0x07c
#define MT6797_DISP_REG_CONFIG_DPI0_SEL_IN		0x084
#define MT6797_DISP_REG_CONFIG_DISP_PATH0_SEL_IN	0x088
#define MT6797_DISP_REG_CONFIG_DISP_PATH0_SOUT_SEL_IN	0x08c
#define MT6797_DISP_REG_CONFIG_DISP_RDMA0_SOUT_SEL_IN	0x090
#define MT6797_DISP_REG_CONFIG_DISP_RDMA1_SOUT_SEL_IN	0x094
#define MT6797_DISP_REG_CONFIG_DISP_OVL0_SOUT_SEL_IN	0x098
#define MT6797_DISP_REG_CONFIG_DISP_OVL0_SEL_IN		0x09c
#define MT6797_DISP_REG_CONFIG_DISP_OVL1_SOUT_SEL_IN	0x0a0

/* MOUT_EN bits, from ddp_path.c mout_map[] */
#define MT6797_OVL0_MOUT_EN_COLOR0		BIT(0)	/* {COLOR0 b0, WDMA0 b1} */
#define MT6797_DITHER_MOUT_EN_RDMA0		BIT(0)	/* {RDMA0 b0, PATH0 b1, WDMA0 b2} */
/* UFOE_MOUT_EN bits: {DSI0 b0, SPLIT0 b1, DPI b2, WDMA0 b3, DSI1 b4} */
#define MT6797_UFOE_MOUT_EN_DSI0		BIT(0)

/* selector values = index into the matching ddp_path.c option list */
#define MT6797_OVL0_SOUT_OVL0_VIRTUAL		0x0	/* {OVL0_VIRTUAL, OVL1_2L} */
#define MT6797_OVL0_SEL_IN_OVL0_2L		0x0	/* {OVL0_2L, OVL1, OVL1_2L} */
#define MT6797_COLOR0_SEL_IN_OVL0_VIRTUAL	0x1	/* {RDMA0, OVL0_VIRTUAL} */
#define MT6797_PATH0_SOUT_UFOE			0x0	/* {UFOE, DSC} */
#define MT6797_RDMA0_SOUT_PATH0			0x0	/* {PATH0, COLOR0, DSI0, DSI1, DPI} */
#define MT6797_DSI0_SEL_IN_UFOE			0x0	/* {UFOE, SPLIT0, RDMA0, DSC} */

static const struct mtk_mmsys_routes mt6797_mmsys_routing_table[] = {
	/* OVL0 -> OVL0_2L is a hardwired link (no mux, no register). */
	/* OVL0_2L -> OVL0_VIRTUAL -> COLOR0 */
	MMSYS_ROUTE(OVL_2L0, COLOR0,
		    MT6797_DISP_REG_CONFIG_DISP_OVL0_SOUT_SEL_IN, BIT(0),
		    MT6797_OVL0_SOUT_OVL0_VIRTUAL),
	MMSYS_ROUTE(OVL_2L0, COLOR0,
		    MT6797_DISP_REG_CONFIG_DISP_OVL0_SEL_IN, GENMASK(1, 0),
		    MT6797_OVL0_SEL_IN_OVL0_2L),
	MMSYS_ROUTE(OVL_2L0, COLOR0,
		    MT6797_DISP_REG_CONFIG_DISP_OVL0_MOUT_EN, GENMASK(1, 0),
		    MT6797_OVL0_MOUT_EN_COLOR0),
	MMSYS_ROUTE(OVL_2L0, COLOR0,
		    MT6797_DISP_REG_CONFIG_DISP_COLOR0_SEL_IN, BIT(0),
		    MT6797_COLOR0_SEL_IN_OVL0_VIRTUAL),
	/* DITHER -> RDMA0 */
	MMSYS_ROUTE(DITHER0, RDMA0,
		    MT6797_DISP_REG_CONFIG_DISP_DITHER_MOUT_EN, GENMASK(2, 0),
		    MT6797_DITHER_MOUT_EN_RDMA0),
	/* RDMA0 -> PATH0 -> UFOE */
	MMSYS_ROUTE(RDMA0, UFOE,
		    MT6797_DISP_REG_CONFIG_DISP_RDMA0_SOUT_SEL_IN, BIT(0),
		    MT6797_RDMA0_SOUT_PATH0),
	MMSYS_ROUTE(RDMA0, UFOE,
		    MT6797_DISP_REG_CONFIG_DISP_PATH0_SOUT_SEL_IN, BIT(0),
		    MT6797_PATH0_SOUT_UFOE),
	/* UFOE -> DSI0 */
	MMSYS_ROUTE(UFOE, DSI0,
		    MT6797_DISP_REG_CONFIG_DISP_UFOE_MOUT_EN, GENMASK(2, 0),
		    MT6797_UFOE_MOUT_EN_DSI0),
	MMSYS_ROUTE(UFOE, DSI0,
		    MT6797_DISP_REG_CONFIG_DSI0_SEL_IN, BIT(0),
		    MT6797_DSI0_SEL_IN_UFOE),
};

#endif /* __SOC_MEDIATEK_MT6797_MMSYS_H */
