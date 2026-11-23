/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOC_MEDIATEK_MT6755_MMSYS_H
#define __SOC_MEDIATEK_MT6755_MMSYS_H

/*
 * MT6755 DDP routing registers (offsets into MMSYS_CONFIG @ 0x14000000).
 *
 * VERIFIED-FROM-STOCK:
 *   register offsets: kernel-m681-m6base-3.18.140/kernel-3.18/
 *     drivers/misc/mediatek/video/mt6755/ddp_reg.h lines 1343-1362
 *   mout bit meanings / sel index meanings: same tree,
 *     drivers/misc/mediatek/video/mt6755/ddp_path.c lines 150-218
 *     (mout_map[] bit assignments, sel_in_map[]/sel_out_map[] input lists;
 *      the value written to a SEL register is the INDEX in that list —
 *      ddp_path.c:~578-604 writes (uint16_t)k).
 *
 * Main path (stock DDP_SCENARIO_PRIMARY_DISP, ddp_path.c:76-79):
 *   OVL0 -> (OVL0_SOUT=0) -> OVL0_MOUT[bit0] -> (COLOR0_SEL_IN=1) -> COLOR0
 *   -> CCORR -> AAL -> GAMMA -> DITHER (direct links, no routing regs)
 *   -> DITHER_MOUT[bit0] -> RDMA0 -> (RDMA0_SOUT=0 UFOE)
 *   -> UFOE_MOUT[bit0 DSI0] -> (DSI0_SEL_IN=0 UFOE) -> DSI0
 * NOTE: on MT6755 "UFOE" has no register block (stock ddp_reg.h:1007
 * DDP_REG_BASE_DISP_UFOE = 0); it exists only as the UFOE_MOUT routing stage
 * whose clock gate is MMSYS CG0 bit 21.  We therefore wire RDMA0->DSI0
 * through the UFOE mout inside the (RDMA0, DSI0) routes and do NOT expose a
 * UFOE component to mtk-drm.  This is byte-identical to what LK + the stock
 * kernel leave programmed, so a warm handoff does not change routing at all.
 */

#define MT6755_DISP_REG_CONFIG_DISP_OVL0_MOUT_EN	0x030
#define MT6755_DISP_REG_CONFIG_DISP_OVL1_MOUT_EN	0x034
#define MT6755_DISP_REG_CONFIG_DISP_DITHER_MOUT_EN	0x038
#define MT6755_DISP_REG_CONFIG_DISP_UFOE_MOUT_EN	0x03c
#define MT6755_DISP_REG_CONFIG_DISP_COLOR0_SEL_IN	0x058
#define MT6755_DISP_REG_CONFIG_DISP_WDMA0_SEL_IN	0x05c
#define MT6755_DISP_REG_CONFIG_DISP_UFOE_SEL_IN		0x060
#define MT6755_DISP_REG_CONFIG_DSI0_SEL_IN		0x064
#define MT6755_DISP_REG_CONFIG_DPI0_SEL_IN		0x068
#define MT6755_DISP_REG_CONFIG_DISP_RDMA0_SOUT_SEL_IN	0x06c
#define MT6755_DISP_REG_CONFIG_DISP_RDMA1_SOUT_SEL_IN	0x070
#define MT6755_DISP_REG_CONFIG_DISP_OVL0_SOUT_SEL_IN	0x074
#define MT6755_DISP_REG_CONFIG_DISP_OVL0_SEL_IN		0x078

/* mout bits (ddp_path.c mout_map[]) */
#define MT6755_OVL0_MOUT_EN_COLOR0		BIT(0)	/* field REG_FLD(2,0) */
#define MT6755_DITHER_MOUT_EN_RDMA0		BIT(0)	/* field REG_FLD(3,0) */
#define MT6755_UFOE_MOUT_EN_DSI0		BIT(0)	/* field REG_FLD(3,0) */

/* sel values = index into sel_in_map/sel_out_map input list */
#define MT6755_COLOR0_SEL_IN_OVL0		0x1	/* {RDMA0, OVL0_VIRT} */
#define MT6755_UFOE_SEL_IN_RDMA0		0x0	/* {RDMA0, DITHER}    */
#define MT6755_DSI0_SEL_IN_UFOE			0x0	/* {UFOE,RDMA0,RDMA1} */
#define MT6755_RDMA0_SOUT_UFOE			0x0	/* {UFOE,COLOR0,DSI0,DPI} */
#define MT6755_OVL0_SOUT_OVL0_VIRT		0x0	/* {OVL0_VIRT,OVL1_2L} */
#define MT6755_OVL0_SEL_IN_OVL0			0x0	/* {OVL0,OVL1,OVL1_2L} */

static const struct mtk_mmsys_routes mt6755_mmsys_routing_table[] = {
	/*
	 * OVL0 -> COLOR0 (via OVL0_SOUT -> OVL0_SEL -> OVL0_MOUT).
	 * The SOUT/SEL pair defaults to this on reset and is what LK leaves,
	 * but force it so a warm/dirty handoff cannot break the path.
	 */
	MMSYS_ROUTE(OVL0, COLOR0,
		    MT6755_DISP_REG_CONFIG_DISP_OVL0_SOUT_SEL_IN, GENMASK(1, 0),
		    MT6755_OVL0_SOUT_OVL0_VIRT),
	MMSYS_ROUTE(OVL0, COLOR0,
		    MT6755_DISP_REG_CONFIG_DISP_OVL0_SEL_IN, GENMASK(1, 0),
		    MT6755_OVL0_SEL_IN_OVL0),
	MMSYS_ROUTE(OVL0, COLOR0,
		    MT6755_DISP_REG_CONFIG_DISP_OVL0_MOUT_EN, GENMASK(1, 0),
		    MT6755_OVL0_MOUT_EN_COLOR0),
	MMSYS_ROUTE(OVL0, COLOR0,
		    MT6755_DISP_REG_CONFIG_DISP_COLOR0_SEL_IN, BIT(0),
		    MT6755_COLOR0_SEL_IN_OVL0),
	/* DITHER -> RDMA0 */
	MMSYS_ROUTE(DITHER0, RDMA0,
		    MT6755_DISP_REG_CONFIG_DISP_DITHER_MOUT_EN, GENMASK(2, 0),
		    MT6755_DITHER_MOUT_EN_RDMA0),
	/* RDMA0 -> DSI0, wired through the (blockless) UFOE mout, as stock */
	MMSYS_ROUTE(RDMA0, DSI0,
		    MT6755_DISP_REG_CONFIG_DISP_RDMA0_SOUT_SEL_IN, GENMASK(1, 0),
		    MT6755_RDMA0_SOUT_UFOE),
	MMSYS_ROUTE(RDMA0, DSI0,
		    MT6755_DISP_REG_CONFIG_DISP_UFOE_SEL_IN, GENMASK(1, 0),
		    MT6755_UFOE_SEL_IN_RDMA0),
	MMSYS_ROUTE(RDMA0, DSI0,
		    MT6755_DISP_REG_CONFIG_DISP_UFOE_MOUT_EN, GENMASK(2, 0),
		    MT6755_UFOE_MOUT_EN_DSI0),
	MMSYS_ROUTE(RDMA0, DSI0,
		    MT6755_DISP_REG_CONFIG_DSI0_SEL_IN, GENMASK(1, 0),
		    MT6755_DSI0_SEL_IN_UFOE),
};

#endif /* __SOC_MEDIATEK_MT6755_MMSYS_H */
