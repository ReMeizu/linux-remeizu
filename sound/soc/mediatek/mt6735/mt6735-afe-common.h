/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mt6735-afe-common.h  --  MediaTek MT6735/MT6737/MT6753 audio driver definitions
 */

#ifndef _MT_6735_AFE_COMMON_H_
#define _MT_6735_AFE_COMMON_H_

#include <linux/list.h>
#include <linux/regmap.h>
#include <sound/soc.h>

#include "../common/mtk-base-afe.h"

enum {
	MT6735_MEMIF_DL1,
	MT6735_MEMIF_DL2,
	MT6735_MEMIF_AWB,
	MT6735_MEMIF_VUL,
	MT6735_MEMIF_VUL2,
	MT6735_MEMIF_DAI,
	MT6735_MEMIF_MOD_DAI,
	MT6735_MEMIF_NUM,
	MT6735_DAI_ADDA = MT6735_MEMIF_NUM,
	MT6735_DAI_PCM_1,
	MT6735_DAI_HOSTLESS_LPBK,
	MT6735_DAI_HOSTLESS_SPEECH,
	MT6735_DAI_NUM,
};

enum {
	MT6735_IRQ_1,
	MT6735_IRQ_2,
	MT6735_IRQ_NUM,
};

struct clk;

struct mt6735_afe_private {
	struct clk **clk;
};

unsigned int mt6735_general_rate_transform(struct device *dev,
					   unsigned int rate);
unsigned int mt6735_rate_transform(struct device *dev,
				   unsigned int rate, int aud_blk);

/* dai registration callbacks */
int mt6735_dai_adda_register(struct mtk_base_afe *afe);
int mt6735_dai_pcm_register(struct mtk_base_afe *afe);
int mt6735_dai_hostless_register(struct mtk_base_afe *afe);

#endif
