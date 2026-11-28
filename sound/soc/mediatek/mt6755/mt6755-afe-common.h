/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mt6755-afe-common.h  --  Mediatek 6797 audio driver definitions
 *
 * Copyright (c) 2018 MediaTek Inc.
 * Author: KaiChieh Chuang <kaichieh.chuang@mediatek.com>
 */

#ifndef _MT_6797_AFE_COMMON_H_
#define _MT_6797_AFE_COMMON_H_

#include <sound/soc.h>
#include <linux/list.h>
#include <linux/regmap.h>
#include "../common/mtk-base-afe.h"

enum {
	MT6755_MEMIF_DL1,
	MT6755_MEMIF_DL2,
	MT6755_MEMIF_DL3,
	MT6755_MEMIF_VUL,
	MT6755_MEMIF_AWB,
	MT6755_MEMIF_VUL12,
	MT6755_MEMIF_DAI,
	MT6755_MEMIF_MOD_DAI,
	MT6755_MEMIF_NUM,
	MT6755_DAI_ADDA = MT6755_MEMIF_NUM,
	MT6755_DAI_PCM_1,
	MT6755_DAI_PCM_2,
	MT6755_DAI_HOSTLESS_LPBK,
	MT6755_DAI_HOSTLESS_SPEECH,
	MT6755_DAI_NUM,
};

enum {
	MT6755_IRQ_1,
	MT6755_IRQ_2,
	MT6755_IRQ_3,
	MT6755_IRQ_4,
	MT6755_IRQ_7,
	MT6755_IRQ_NUM,
};

struct clk;

struct mt6755_afe_private {
	struct clk **clk;
};

unsigned int mt6755_general_rate_transform(struct device *dev,
					   unsigned int rate);
unsigned int mt6755_rate_transform(struct device *dev,
				   unsigned int rate, int aud_blk);

/* dai register */
int mt6755_dai_adda_register(struct mtk_base_afe *afe);
int mt6755_dai_pcm_register(struct mtk_base_afe *afe);
int mt6755_dai_hostless_register(struct mtk_base_afe *afe);
#endif
