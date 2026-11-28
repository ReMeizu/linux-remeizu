// SPDX-License-Identifier: GPL-2.0-only
/*
 * mt6735-dai-pcm.c  --  MediaTek MT6735/MT6737/MT6753 PCM interface DAI
 *
 * The MT6735 AFE exposes two identical PCM ports ("PCM 1" and "PCM 2") for
 * the cellular modem (Voice_MD1) and for Bluetooth SCO. Both are configured
 * through PCM_INTF_CON1: clock master/slave, PCM mode A/B, frame width and
 * sample rate.
 */

#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

#include "mt6735-afe-common.h"
#include "mt6735-interconnection.h"
#include "mt6735-reg.h"

enum {
	AUD_PCM_MODE_8K = 0,
	AUD_PCM_MODE_16K = 1,
	AUD_PCM_MODE_32K = 2,
	AUD_PCM_MODE_48K = 3,
};

#define PCM_INTF_CON1_MASK	0xfffffffe

/* dai component: interconnections feeding the PCM output port */
static const struct snd_kcontrol_new mtk_pcm_1_playback_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN7,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN7, I_DL2_CH1, 1, 0),
};

static const struct snd_kcontrol_new mtk_pcm_1_playback_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN8,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN8, I_DL2_CH2, 1, 0),
};

static const struct snd_soc_dapm_widget mtk_dai_pcm_widgets[] = {
	SND_SOC_DAPM_MIXER("PCM_1_PB_CH1", SND_SOC_NOPM, 0, 0,
			   mtk_pcm_1_playback_ch1_mix,
			   ARRAY_SIZE(mtk_pcm_1_playback_ch1_mix)),
	SND_SOC_DAPM_MIXER("PCM_1_PB_CH2", SND_SOC_NOPM, 0, 0,
			   mtk_pcm_1_playback_ch2_mix,
			   ARRAY_SIZE(mtk_pcm_1_playback_ch2_mix)),

	SND_SOC_DAPM_SUPPLY("PCM_1_EN", PCM_INTF_CON1, PCM_EN_SFT, 0,
			    NULL, 0),

	SND_SOC_DAPM_INPUT("MD1_TO_AFE"),
	SND_SOC_DAPM_OUTPUT("AFE_TO_MD1"),
};

static const struct snd_soc_dapm_route mtk_dai_pcm_routes[] = {
	{"PCM 1 Playback", NULL, "PCM_1_PB_CH1"},
	{"PCM 1 Playback", NULL, "PCM_1_PB_CH2"},

	{"PCM 1 Playback", NULL, "PCM_1_EN"},
	{"PCM 1 Capture", NULL, "PCM_1_EN"},

	{"AFE_TO_MD1", NULL, "PCM 1 Playback"},
	{"PCM 1 Capture", NULL, "MD1_TO_AFE"},

	{"PCM_1_PB_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"PCM_1_PB_CH2", "ADDA_UL_CH2", "ADDA Capture"},
	{"PCM_1_PB_CH1", "DL2_CH1", "DL2"},
	{"PCM_1_PB_CH2", "DL2_CH2", "DL2"},
};

/* dai ops */
static int mtk_dai_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct snd_soc_dapm_widget *pw =
		snd_soc_dai_get_widget_playback(dai);
	struct snd_soc_dapm_widget *cw =
		snd_soc_dai_get_widget_capture(dai);
	unsigned int rate = params_rate(params);
	unsigned int rate_reg;
	unsigned int pcm_con;

	dev_dbg(afe->dev, "%s(), id %d, stream %d, rate %u, widgets p %d c %d\n",
		__func__, dai->id, substream->stream, rate,
		pw ? pw->active : -1, cw ? cw->active : -1);

	/* the interface is already running with the negotiated rate */
	if ((pw && pw->active) || (cw && cw->active))
		return 0;

	switch (rate) {
	case 8000:
		rate_reg = AUD_PCM_MODE_8K;
		break;
	case 16000:
		rate_reg = AUD_PCM_MODE_16K;
		break;
	case 32000:
		rate_reg = AUD_PCM_MODE_32K;
		break;
	case 48000:
		rate_reg = AUD_PCM_MODE_48K;
		break;
	default:
		dev_err(afe->dev, "%s(), rate %u not supported by the PCM port\n",
			__func__, rate);
		return -EINVAL;
	}

	/*
	 * AFE is the PCM clock master, 4 kHz sync, PCM mode B, 32 BCK cycles
	 * per frame, TX channel repeat disabled, dual mic on TX and AFIFO
	 * (same crystal) resynchronisation.
	 */
	pcm_con = 0x3 << 1;		/* FMT = PCM mode B */
	pcm_con |= rate_reg << 3;	/* MODE */
	pcm_con |= 0x7 << 9;		/* SYNC length */

	if (dai->id != MT6735_DAI_PCM_1) {
		dev_warn(afe->dev, "%s(), PCM DAI %d is not wired on this SoC\n",
			 __func__, dai->id);
		return 0;
	}

	regmap_update_bits(afe->regmap, PCM_INTF_CON1, PCM_INTF_CON1_MASK,
			   pcm_con);

	return 0;
}

static const struct snd_soc_dai_ops mtk_dai_pcm_ops = {
	.hw_params = mtk_dai_pcm_hw_params,
};

/* dai driver */
#define MTK_PCM_RATES	(SNDRV_PCM_RATE_8000 |\
			 SNDRV_PCM_RATE_16000 |\
			 SNDRV_PCM_RATE_32000 |\
			 SNDRV_PCM_RATE_48000)

#define MTK_PCM_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE |\
			 SNDRV_PCM_FMTBIT_S24_LE |\
			 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver mtk_dai_pcm_driver[] = {
	{
		.name = "PCM 1",
		.id = MT6735_DAI_PCM_1,
		.playback = {
			.stream_name = "PCM 1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.capture = {
			.stream_name = "PCM 1 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_dai_pcm_ops,
		.symmetric_rate = 1,
		.symmetric_sample_bits = 1,
	},
};

int mt6735_dai_pcm_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	list_add(&dai->list, &afe->sub_dais);

	dai->dai_drivers = mtk_dai_pcm_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mtk_dai_pcm_driver);

	dai->dapm_widgets = mtk_dai_pcm_widgets;
	dai->num_dapm_widgets = ARRAY_SIZE(mtk_dai_pcm_widgets);
	dai->dapm_routes = mtk_dai_pcm_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mtk_dai_pcm_routes);

	return 0;
}
