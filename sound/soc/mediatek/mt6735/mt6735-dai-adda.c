// SPDX-License-Identifier: GPL-2.0-only
/*
 * mt6735-dai-adda.c  --  MediaTek MT6735/MT6737/MT6753 ADDA (MTKAIF) DAI
 *
 * The ADDA block converts between the AFE's internal PCM streams and the
 * MTKAIF serial link that carries audio to and from the PMIC codec
 * (MT6328/MT6353). The register programming sequence is the same one used
 * by the vendor BSP and by the upstream MT6797/MT8183 ADDA DAI.
 */

#include <linux/delay.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

#include "mt6735-afe-common.h"
#include "mt6735-interconnection.h"
#include "mt6735-reg.h"
#include "../common/mtk-dai-adda-common.h"

/* dai component */
static const struct snd_kcontrol_new mtk_adda_dl_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN3, I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN3, I_DL2_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN3,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN3,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH1", AFE_CONN3,
				    I_PCM_1_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("MOD_DAI_CH1", AFE_CONN3,
				    I_MOD_DAI_CH1, 1, 0),
};

static const struct snd_kcontrol_new mtk_adda_dl_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN4, I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN4, I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN4, I_DL2_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN4, I_DL2_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN4,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN4,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH1", AFE_CONN4,
				    I_PCM_1_CAP_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("PCM_1_CAP_CH2", AFE_CONN4,
				    I_PCM_1_CAP_CH2, 1, 0),
};

static int mtk_adda_ul_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol,
			     int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(cmpnt);

	dev_dbg(afe->dev, "%s(), name %s, event 0x%x\n",
		__func__, w->name, event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMD:
		/*
		 * The AFE must stay alive for at least one sample (125 us at
		 * the lowest rate) after the capture supply is brought down.
		 */
		usleep_range(125, 135);
		break;
	default:
		break;
	}

	return 0;
}

enum {
	SUPPLY_SEQ_AUD_TOP_PDN,
	SUPPLY_SEQ_ADDA_AFE_ON,
	SUPPLY_SEQ_ADDA_DL_ON,
	SUPPLY_SEQ_ADDA_UL_ON,
};

static const struct snd_soc_dapm_widget mtk_dai_adda_widgets[] = {
	/* adda interconnections */
	SND_SOC_DAPM_MIXER("ADDA_DL_CH1", SND_SOC_NOPM, 0, 0,
			   mtk_adda_dl_ch1_mix,
			   ARRAY_SIZE(mtk_adda_dl_ch1_mix)),
	SND_SOC_DAPM_MIXER("ADDA_DL_CH2", SND_SOC_NOPM, 0, 0,
			   mtk_adda_dl_ch2_mix,
			   ARRAY_SIZE(mtk_adda_dl_ch2_mix)),

	SND_SOC_DAPM_SUPPLY_S("ADDA Enable", SUPPLY_SEQ_ADDA_AFE_ON,
			      AFE_ADDA_UL_DL_CON0, ADDA_AFE_ON_SFT, 0,
			      NULL, 0),

	SND_SOC_DAPM_SUPPLY_S("ADDA Playback Enable", SUPPLY_SEQ_ADDA_DL_ON,
			      AFE_ADDA_DL_SRC2_CON0,
			      DL_2_SRC_ON_TMP_CTL_PRE_SFT, 0,
			      NULL, 0),

	SND_SOC_DAPM_SUPPLY_S("ADDA Capture Enable", SUPPLY_SEQ_ADDA_UL_ON,
			      AFE_ADDA_UL_SRC_CON0,
			      UL_SRC_ON_TMP_CTL_SFT, 0,
			      mtk_adda_ul_event,
			      SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SUPPLY_S("aud_dac_clk", SUPPLY_SEQ_AUD_TOP_PDN,
			      AUDIO_TOP_CON0, PDN_DAC_SFT, 1, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("aud_dac_predis_clk", SUPPLY_SEQ_AUD_TOP_PDN,
			      AUDIO_TOP_CON0, PDN_DAC_PREDIS_SFT, 1, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("aud_adc_clk", SUPPLY_SEQ_AUD_TOP_PDN,
			      AUDIO_TOP_CON0, PDN_ADC_SFT, 1, NULL, 0),
};

static const struct snd_soc_dapm_route mtk_dai_adda_routes[] = {
	/* playback */
	{"ADDA_DL_CH1", "DL1_CH1", "DL1"},
	{"ADDA_DL_CH2", "DL1_CH1", "DL1"},
	{"ADDA_DL_CH2", "DL1_CH2", "DL1"},

	{"ADDA_DL_CH1", "DL2_CH1", "DL2"},
	{"ADDA_DL_CH2", "DL2_CH1", "DL2"},
	{"ADDA_DL_CH2", "DL2_CH2", "DL2"},

	{"ADDA Playback", NULL, "ADDA_DL_CH1"},
	{"ADDA Playback", NULL, "ADDA_DL_CH2"},

	/* adda enables */
	{"ADDA Playback", NULL, "ADDA Enable"},
	{"ADDA Playback", NULL, "ADDA Playback Enable"},
	{"ADDA Capture", NULL, "ADDA Enable"},
	{"ADDA Capture", NULL, "ADDA Capture Enable"},

	/* clocks */
	{"ADDA Playback", NULL, "aud_dac_clk"},
	{"ADDA Playback", NULL, "aud_dac_predis_clk"},
	{"ADDA Capture", NULL, "aud_adc_clk"},
};

/* dai ops */
static int mtk_dai_adda_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	unsigned int rate = params_rate(params);

	dev_dbg(afe->dev, "%s(), id %d, stream %d, rate %u\n",
		__func__, dai->id, substream->stream, rate);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		unsigned int dl_src2_con0, dl_src2_con1;

		/* clean predistortion */
		regmap_write(afe->regmap, AFE_ADDA_PREDIS_CON0, 0);
		regmap_write(afe->regmap, AFE_ADDA_PREDIS_CON1, 0);

		/* set input sampling rate */
		dl_src2_con0 = mtk_adda_dl_rate_transform(afe, rate) <<
			       DL_2_INPUT_MODE_CTL_SFT;

		/* set output up-sampling ratio */
		switch (rate) {
		case 192000:
			dl_src2_con0 |= 0x1 << DL_2_OUTPUT_SEL_CTL_SFT;
			break;
		case 96000:
			dl_src2_con0 |= 0x2 << DL_2_OUTPUT_SEL_CTL_SFT;
			break;
		default:
			dl_src2_con0 |= 0x3 << DL_2_OUTPUT_SEL_CTL_SFT;
			break;
		}

		/* turn off mute */
		dl_src2_con0 |= BIT(DL_2_MUTE_CH1_OFF_CTL_PRE_SFT) |
				BIT(DL_2_MUTE_CH2_OFF_CTL_PRE_SFT);

		if (rate < 96000) {
			/* -0.3 dB, as recommended for the speech path */
			dl_src2_con1 = 0xf74f0000;
		} else {
			/* halve the DL gain for the hires paths */
			dl_src2_con1 = 0x7ba70000;
		}

		/* turn on downlink gain */
		dl_src2_con0 |= BIT(DL_2_GAIN_ON_CTL_PRE_SFT);

		regmap_write(afe->regmap, AFE_ADDA_DL_SRC2_CON0, dl_src2_con0);
		regmap_write(afe->regmap, AFE_ADDA_DL_SRC2_CON1, dl_src2_con1);
	} else {
		unsigned int voice_mode, ul_src_con0 = 0;

		/* use the internal ADC */
		regmap_update_bits(afe->regmap, AFE_ADDA_TOP_CON0, BIT(0), 0);

		voice_mode = mtk_adda_ul_rate_transform(afe, rate);
		ul_src_con0 |= (voice_mode << UL_VOICE_MODE_CH1_CH2_SFT) &
			       (UL_VOICE_MODE_CH1_CH2_MASK <<
				UL_VOICE_MODE_CH1_CH2_SFT);

		/* up8x txif saturation on */
		regmap_write(afe->regmap, AFE_ADDA_NEWIF_CFG0, 0x03f87201);

		if (rate >= 96000) {
			/* hires format [1 0 23] */
			regmap_update_bits(afe->regmap, AFE_ADDA_NEWIF_CFG0,
					   BIT(5), BIT(5));
			regmap_update_bits(afe->regmap, AFE_ADDA_NEWIF_CFG2,
					   0xf << 28, voice_mode << 28);
		} else {
			/* fixed 260 kHz ANC path with CIC output */
			regmap_update_bits(afe->regmap, AFE_ADDA_NEWIF_CFG2,
					   0xf << 28, 8 << 28);
			ul_src_con0 |= BIT(UL_SRC_USE_CIC_OUT_CTL_SFT);
		}

		regmap_update_bits(afe->regmap, AFE_ADDA_UL_SRC_CON0,
				   0xfffffffe, ul_src_con0);
	}

	return 0;
}

static const struct snd_soc_dai_ops mtk_dai_adda_ops = {
	.hw_params = mtk_dai_adda_hw_params,
};

/* dai driver */
#define MTK_ADDA_PLAYBACK_RATES	(SNDRV_PCM_RATE_8000_48000 |\
				 SNDRV_PCM_RATE_96000 |\
				 SNDRV_PCM_RATE_192000)

#define MTK_ADDA_CAPTURE_RATES	(SNDRV_PCM_RATE_8000 |\
				 SNDRV_PCM_RATE_16000 |\
				 SNDRV_PCM_RATE_32000 |\
				 SNDRV_PCM_RATE_48000 |\
				 SNDRV_PCM_RATE_96000 |\
				 SNDRV_PCM_RATE_192000)

#define MTK_ADDA_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE |\
				 SNDRV_PCM_FMTBIT_S24_LE |\
				 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver mtk_dai_adda_driver[] = {
	{
		.name = "ADDA",
		.id = MT6735_DAI_ADDA,
		.playback = {
			.stream_name = "ADDA Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_ADDA_PLAYBACK_RATES,
			.formats = MTK_ADDA_FORMATS,
		},
		.capture = {
			.stream_name = "ADDA Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_ADDA_CAPTURE_RATES,
			.formats = MTK_ADDA_FORMATS,
		},
		.ops = &mtk_dai_adda_ops,
	},
};

int mt6735_dai_adda_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	list_add(&dai->list, &afe->sub_dais);

	dai->dai_drivers = mtk_dai_adda_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mtk_dai_adda_driver);

	dai->dapm_widgets = mtk_dai_adda_widgets;
	dai->num_dapm_widgets = ARRAY_SIZE(mtk_dai_adda_widgets);
	dai->dapm_routes = mtk_dai_adda_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mtk_dai_adda_routes);

	return 0;
}
