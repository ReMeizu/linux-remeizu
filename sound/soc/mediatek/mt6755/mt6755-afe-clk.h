/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mt6755-afe-clk.h  --  Mediatek 6797 afe clock ctrl definition
 *
 * Copyright (c) 2018 MediaTek Inc.
 * Author: KaiChieh Chuang <kaichieh.chuang@mediatek.com>
 */

#ifndef _MT6755_AFE_CLK_H_
#define _MT6755_AFE_CLK_H_

struct mtk_base_afe;

int mt6755_init_clock(struct mtk_base_afe *afe);
int mt6755_afe_enable_clock(struct mtk_base_afe *afe);
int mt6755_afe_disable_clock(struct mtk_base_afe *afe);
#endif
