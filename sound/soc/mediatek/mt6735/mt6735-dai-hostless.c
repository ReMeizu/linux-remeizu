// SPDX-License-Identifier: GPL-2.0-only
/*
 * mt6735-dai-hostless.c  --  MediaTek MT6735/MT6737/MT6753 hostless DAIs
 *
 * Hostless DAIs keep an in-SoC audio path running without userspace timing
 * it: "Hostless LPBK" (ADDA uplink looped back into the ADDA downlink, used
 * by the voice-call self-loop test) and "Hostless Speech" (the modem PCM
 * port bridged to the PMIC codec while the voice call is up).
 */

#include <sound/soc.h>
#include <sound/soc-dapm.h>

#include "mt6735-afe-common.h"

/* dai component */
static const struct snd_soc_dapm_route mtk_dai_hostless_routes[] = {
	/* Hostless ADDA loopback */
	{"ADDA_DL_CH1", "ADDA_UL_CH1", "Hostless LPBK DL"},
	{"ADDA_DL_CH1", "ADDA_UL_CH2", "Hostless LPBK DL"},
	{"ADDA_DL_CH2", "ADDA_UL_CH1", "Hostless LPBK DL"},
	{"ADDA_DL_CH2", "ADDA_UL_CH2", "Hostless LPBK DL"},
	{"Hostless LPBK UL", NULL, "ADDA Capture"},

	/* Hostless speech: modem PCM <-> ADDA */
	{"ADDA_DL_CH1", "PCM_1_CAP_CH1", "Hostless Speech DL"},
	{"ADDA_DL_CH2", "PCM_1_CAP_CH1", "Hostless Speech DL"},
	{"ADDA_DL_CH2", "PCM_1_CAP_CH2", "Hostless Speech DL"},
	{"PCM_1_PB_CH1", "ADDA_UL_CH1", "Hostless Speech DL"},
	{"PCM_1_PB_CH2", "ADDA_UL_CH2", "Hostless Speech DL"},

	{"Hostless Speech UL", NULL, "PCM 1 Capture"},
	{"Hostless Speech UL", NULL, "ADDA Capture"},
};

/* dai ops */
static int mtk_dai_hostless_startup(struct snd_pcm_substream *substream,
				    struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);

	return snd_soc_set_runtime_hwparams(substream, afe->mtk_afe_hardware);
}

static const struct snd_soc_dai_ops mtk_dai_hostless_ops = {
	.startup = mtk_dai_hostless_startup,
};

/* dai driver */
#define MTK_HOSTLESS_RATES	(SNDRV_PCM_RATE_8000_48000 |\
				 SNDRV_PCM_RATE_88200 |\
				 SNDRV_PCM_RATE_96000 |\
				 SNDRV_PCM_RATE_176400 |\
				 SNDRV_PCM_RATE_192000)

#define MTK_HOSTLESS_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE |\
				 SNDRV_PCM_FMTBIT_S24_LE |\
				 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver mtk_dai_hostless_driver[] = {
	{
		.name = "Hostless LPBK DAI",
		.id = MT6735_DAI_HOSTLESS_LPBK,
		.playback = {
			.stream_name = "Hostless LPBK DL",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_HOSTLESS_RATES,
			.formats = MTK_HOSTLESS_FORMATS,
		},
		.capture = {
			.stream_name = "Hostless LPBK UL",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_HOSTLESS_RATES,
			.formats = MTK_HOSTLESS_FORMATS,
		},
		.ops = &mtk_dai_hostless_ops,
	},
	{
		.name = "Hostless Speech DAI",
		.id = MT6735_DAI_HOSTLESS_SPEECH,
		.playback = {
			.stream_name = "Hostless Speech DL",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_HOSTLESS_RATES,
			.formats = MTK_HOSTLESS_FORMATS,
		},
		.capture = {
			.stream_name = "Hostless Speech UL",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_HOSTLESS_RATES,
			.formats = MTK_HOSTLESS_FORMATS,
		},
		.ops = &mtk_dai_hostless_ops,
	},
};

int mt6735_dai_hostless_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	list_add(&dai->list, &afe->sub_dais);

	dai->dai_drivers = mtk_dai_hostless_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mtk_dai_hostless_driver);

	dai->dapm_routes = mtk_dai_hostless_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mtk_dai_hostless_routes);

	return 0;
}
