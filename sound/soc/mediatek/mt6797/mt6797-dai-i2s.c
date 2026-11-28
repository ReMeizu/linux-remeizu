// SPDX-License-Identifier: GPL-2.0-only
/*
 * mt6797-dai-i2s.c  --  MediaTek MT6797 I2S output DAI
 *
 * The MT6797 audio front-end drives two external I2S ports (I2S1 and I2S2)
 * in addition to the MTKAIF link to the PMIC codec. On the Meizu MX6 the
 * loudspeaker is an NXP TFA98xx SmartPA fed from I2S2, while I2S1 is left
 * unconnected; both ports are registered here so a machine driver can use a
 * plain I2S codec as the speaker backend.
 *
 * Register map (vendor 4.9 BSP AudDrv_Afe.h):
 *   AFE_I2S_CON  (0x0018) : I2S1 control
 *   AFE_I2S_CON1 (0x0034) : I2S2 control
 * The control word is: [0] enable, [1] format (0 = I2S, 1 = PCM),
 * [2] bit clock master, [3] clock polarity, [5:4] word length,
 * [7:6] sample rate, [8] LR clock master.
 */

#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

#include "mt6797-afe-common.h"
#include "mt6797-interconnection.h"
#include "mt6797-reg.h"

/* AFE_I2S_CON / AFE_I2S_CON1 fields */
#define I2S_EN_SFT			0
#define I2S_FMT_SFT			1
#define I2S_BCLK_MASTER_SFT		2
#define I2S_LRCLK_INV_SFT		3
#define I2S_WLEN_SFT			4
#define I2S_WLEN_MASK			0x3
#define I2S_FS_SFT			6
#define I2S_FS_MASK			0x3
#define I2S_LRCLK_MASTER_SFT		8
#define I2S_CON_MASK			GENMASK(11, 0)

/* dai component */
static const struct snd_kcontrol_new mtk_i2s1_out_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN5, I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN5, I_DL2_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH1", AFE_CONN5, I_DL3_CH1, 1, 0),
};

static const struct snd_kcontrol_new mtk_i2s1_out_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN6, I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN6, I_DL2_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH2", AFE_CONN6, I_DL3_CH2, 1, 0),
};

static const struct snd_kcontrol_new mtk_i2s2_out_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN5, I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN5, I_DL2_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH1", AFE_CONN5, I_DL3_CH1, 1, 0),
};

static const struct snd_kcontrol_new mtk_i2s2_out_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN6, I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN6, I_DL2_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH2", AFE_CONN6, I_DL3_CH2, 1, 0),
};

static const struct snd_soc_dapm_widget mtk_dai_i2s_widgets[] = {
	SND_SOC_DAPM_MIXER("I2S1_OUT_CH1", SND_SOC_NOPM, 0, 0,
			   mtk_i2s1_out_ch1_mix,
			   ARRAY_SIZE(mtk_i2s1_out_ch1_mix)),
	SND_SOC_DAPM_MIXER("I2S1_OUT_CH2", SND_SOC_NOPM, 0, 0,
			   mtk_i2s1_out_ch2_mix,
			   ARRAY_SIZE(mtk_i2s1_out_ch2_mix)),
	SND_SOC_DAPM_MIXER("I2S2_OUT_CH1", SND_SOC_NOPM, 0, 0,
			   mtk_i2s2_out_ch1_mix,
			   ARRAY_SIZE(mtk_i2s2_out_ch1_mix)),
	SND_SOC_DAPM_MIXER("I2S2_OUT_CH2", SND_SOC_NOPM, 0, 0,
			   mtk_i2s2_out_ch2_mix,
			   ARRAY_SIZE(mtk_i2s2_out_ch2_mix)),

	SND_SOC_DAPM_SUPPLY("I2S1_BCLK", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("I2S2_BCLK", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_OUTPUT("I2S1_OUT"),
	SND_SOC_DAPM_OUTPUT("I2S2_OUT"),
};

static const struct snd_soc_dapm_route mtk_dai_i2s_routes[] = {
	{"I2S1_OUT", NULL, "I2S1_OUT_CH1"},
	{"I2S1_OUT", NULL, "I2S1_OUT_CH2"},
	{"I2S1_OUT", NULL, "I2S1_BCLK"},

	{"I2S2_OUT", NULL, "I2S2_OUT_CH1"},
	{"I2S2_OUT", NULL, "I2S2_OUT_CH2"},
	{"I2S2_OUT", NULL, "I2S2_BCLK"},

	{"I2S1_OUT_CH1", "DL1_CH1", "DL1"},
	{"I2S1_OUT_CH2", "DL1_CH2", "DL1"},
	{"I2S1_OUT_CH1", "DL2_CH1", "DL2"},
	{"I2S1_OUT_CH2", "DL2_CH2", "DL2"},
	{"I2S1_OUT_CH1", "DL3_CH1", "DL3"},
	{"I2S1_OUT_CH2", "DL3_CH2", "DL3"},

	{"I2S2_OUT_CH1", "DL1_CH1", "DL1"},
	{"I2S2_OUT_CH2", "DL1_CH2", "DL1"},
	{"I2S2_OUT_CH1", "DL2_CH1", "DL2"},
	{"I2S2_OUT_CH2", "DL2_CH2", "DL2"},
	{"I2S2_OUT_CH1", "DL3_CH1", "DL3"},
	{"I2S2_OUT_CH2", "DL3_CH2", "DL3"},
};

/* dai ops */
static int mtk_dai_i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	unsigned int val = 0;

	/* I2S format (not PCM), MSB first, no inverted frame sync */
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S)
		return -EINVAL;
	val |= 0 << I2S_FMT_SFT;

	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_CBC_CFC:
		val |= BIT(I2S_BCLK_MASTER_SFT) | BIT(I2S_LRCLK_MASTER_SFT);
		break;
	case SND_SOC_DAIFMT_CBP_CFC:
		val |= BIT(I2S_BCLK_MASTER_SFT);
		break;
	case SND_SOC_DAIFMT_CBC_CFP:
		val |= BIT(I2S_LRCLK_MASTER_SFT);
		break;
	case SND_SOC_DAIFMT_CBP_CFP:
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_IB_NF:
	case SND_SOC_DAIFMT_NB_IF:
		val |= BIT(I2S_LRCLK_INV_SFT);
		break;
	default:
		return -EINVAL;
	}

	dev_dbg(afe->dev, "%s(), fmt 0x%x -> 0x%x\n", __func__, fmt, val);

	if (dai->id == MT6797_DAI_I2S_1)
		regmap_update_bits(afe->regmap, AFE_I2S_CON, I2S_CON_MASK, val);
	else
		regmap_update_bits(afe->regmap, AFE_I2S_CON1, I2S_CON_MASK, val);

	return 0;
}

static int mtk_dai_i2s_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	unsigned int rate = params_rate(params);
	unsigned int wlen;
	unsigned int fs;
	unsigned int reg = (dai->id == MT6797_DAI_I2S_1) ? AFE_I2S_CON :
							   AFE_I2S_CON1;

	switch (params_width(params)) {
	case 16:
		wlen = 0;
		break;
	case 24:
		wlen = 1;
		break;
	case 32:
		wlen = 2;
		break;
	default:
		dev_err(afe->dev, "%s(), unsupported width %d\n", __func__,
			params_width(params));
		return -EINVAL;
	}

	switch (rate) {
	case 8000:
		fs = 0;
		break;
	case 16000:
		fs = 1;
		break;
	case 32000:
		fs = 2;
		break;
	case 48000:
		fs = 3;
		break;
	default:
		dev_err(afe->dev, "%s(), unsupported rate %u\n", __func__, rate);
		return -EINVAL;
	}

	regmap_update_bits(afe->regmap, reg,
			   (I2S_WLEN_MASK << I2S_WLEN_SFT) |
			   (I2S_FS_MASK << I2S_FS_SFT),
			   (wlen << I2S_WLEN_SFT) | (fs << I2S_FS_SFT));

	return 0;
}

static const struct snd_soc_dai_ops mtk_dai_i2s_ops = {
	.set_fmt = mtk_dai_i2s_set_fmt,
	.hw_params = mtk_dai_i2s_hw_params,
};

#define MTK_I2S_RATES	(SNDRV_PCM_RATE_8000 |\
			 SNDRV_PCM_RATE_16000 |\
			 SNDRV_PCM_RATE_32000 |\
			 SNDRV_PCM_RATE_48000)

#define MTK_I2S_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE |\
			 SNDRV_PCM_FMTBIT_S24_LE |\
			 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver mtk_dai_i2s_driver[] = {
	{
		.name = "I2S1",
		.id = MT6797_DAI_I2S_1,
		.playback = {
			.stream_name = "I2S1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_I2S_RATES,
			.formats = MTK_I2S_FORMATS,
		},
		.ops = &mtk_dai_i2s_ops,
	},
	{
		.name = "I2S2",
		.id = MT6797_DAI_I2S_2,
		.playback = {
			.stream_name = "I2S2 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_I2S_RATES,
			.formats = MTK_I2S_FORMATS,
		},
		.ops = &mtk_dai_i2s_ops,
	},
};

int mt6797_dai_i2s_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	list_add(&dai->list, &afe->sub_dais);

	dai->dai_drivers = mtk_dai_i2s_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mtk_dai_i2s_driver);
	dai->dapm_widgets = mtk_dai_i2s_widgets;
	dai->num_dapm_widgets = ARRAY_SIZE(mtk_dai_i2s_widgets);
	dai->dapm_routes = mtk_dai_i2s_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mtk_dai_i2s_routes);

	return 0;
}
