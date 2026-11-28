/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mt6735-afe-clk.h  --  MediaTek MT6735/MT6737/MT6753 AFE clock control
 */

#ifndef _MT6735_AFE_CLK_H_
#define _MT6735_AFE_CLK_H_

struct mtk_base_afe;

int mt6735_init_clock(struct mtk_base_afe *afe);
int mt6735_afe_enable_clock(struct mtk_base_afe *afe);
int mt6735_afe_disable_clock(struct mtk_base_afe *afe);

#endif
