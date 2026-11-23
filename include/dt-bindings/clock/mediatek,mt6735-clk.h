/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek MT6735/MT6737M MMSYS clock gate IDs.
 *
 * Bit positions are taken from the vendor clock manager
 * (drivers/misc/mediatek/base/power/mt6735/mt_clk_id.h:73-94 and
 * mt_clkmgr.c DISP0/DISP1 groups); the register layout is the standard
 * MMSYS CG set/clr/sta triple at 0x104/0x108/0x100 and 0x114/0x118/0x110.
 */

#ifndef _DT_BINDINGS_CLK_MT6735_MMSYS_H
#define _DT_BINDINGS_CLK_MT6735_MMSYS_H

/* DISP0 group, MMSYS + 0x100 */
#define CLK_MM_DISP0_SMI_COMMON		0
#define CLK_MM_DISP0_SMI_LARB0		1
#define CLK_MM_DISP0_DISP_OVL0		2
#define CLK_MM_DISP0_DISP_RDMA0		3
#define CLK_MM_DISP0_DISP_COLOR		4
#define CLK_MM_DISP0_DISP_CCORR		5
#define CLK_MM_DISP0_DISP_AAL		6
#define CLK_MM_DISP0_DISP_GAMMA		7
#define CLK_MM_DISP0_DISP_DITHER	8

/* DISP1 group, MMSYS + 0x110 */
#define CLK_MM_DISP1_DSI_ENGINE		9
#define CLK_MM_DISP1_DSI_DIGITAL	10

#define CLK_MM_NR_CLK			11

#endif /* _DT_BINDINGS_CLK_MT6735_MMSYS_H */
