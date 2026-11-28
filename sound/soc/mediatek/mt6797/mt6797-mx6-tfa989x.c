// SPDX-License-Identifier: GPL-2.0-only
/*
 * mt6797-mx6-tfa989x.c  --  Meizu MX6 sound card (MT6797 + MT6351 + TFA98xx)
 *
 * The MX6 is an MT6797 phone with the MT6351 PMIC audio codec (handset,
 * headphones, microphones) and a single NXP TFA98xx "SmartPA" driving the
 * loudspeaker over I2S2. The MT6351 itself has no loudspeaker amplifier on
 * this board, so the speaker path is taken over the AFE I2S2 link to the
 * TFA98xx while the DAC/headphone paths stay on the PMIC codec.
 *
 * Links:
 *   Playback_1  AFE DL1 -> I2S2 -> tfa989x   (loudspeaker / SmartPA)
 *   Playback_2  AFE DL2 -> ADDA -> MT6351    (headset / receiver)
 *   Capture_1   MT6351 ADC -> ADDA -> AFE UL1
 *   Headset jack is reported by the MT6351 ACCDET block.
 */

#include <linux/input.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#include "mt6797-afe-common.h"

#define MX6_CODEC_DAI		"mt6351-snd-codec-aif1"
#define MX6_SMARTPA_DAI		"tfa989x-hifi"

enum {
	LINK_PLAYBACK_1,
	LINK_PLAYBACK_2,
	LINK_PLAYBACK_3,
	LINK_CAPTURE_1,
	LINK_CAPTURE_2,
	LINK_SPEAKER,
	LINK_SPEECH,
	LINK_NUM,
};

/* I2S2 must run at a fixed rate: the SmartPA has no clock master role. */
#define MX6_SMARTPA_BCLK_FS_RATIO	64

static const struct snd_soc_dapm_widget mx6_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
};

static const struct snd_soc_dapm_route mx6_routes[] = {
	{"Headset Mic", NULL, "Mic Bias"},
	{"Main Mic", NULL, "Mic Bias"},
};

static struct snd_soc_jack mx6_headset_jack;

SND_SOC_DAILINK_DEFS(playback_1,
	DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback_2,
	DAILINK_COMP_ARRAY(COMP_CPU("DL2")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback_3,
	DAILINK_COMP_ARRAY(COMP_CPU("DL3")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture_1,
	DAILINK_COMP_ARRAY(COMP_CPU("UL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture_2,
	DAILINK_COMP_ARRAY(COMP_CPU("UL2")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

/* Loudspeaker: AFE I2S2 (BE) drives the external TFA98xx over I2S. */
SND_SOC_DAILINK_DEFS(speaker,
	DAILINK_COMP_ARRAY(COMP_CPU("I2S2")),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, MX6_SMARTPA_DAI)),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

/* Phone audio: MTKAIF ADDA link to the MT6351. */
SND_SOC_DAILINK_DEFS(speech,
	DAILINK_COMP_ARRAY(COMP_CPU("ADDA")),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, MX6_CODEC_DAI)),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link mx6_dai_links[] = {
	/* FE */
	[LINK_PLAYBACK_1] = {
		.name = "Playback_1",
		.stream_name = "Playback_1",
		.trigger = { SND_SOC_DPCM_TRIGGER_PRE,
			     SND_SOC_DPCM_TRIGGER_PRE },
		.dynamic = 1,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(playback_1),
	},
	[LINK_PLAYBACK_2] = {
		.name = "Playback_2",
		.stream_name = "Playback_2",
		.trigger = { SND_SOC_DPCM_TRIGGER_PRE,
			     SND_SOC_DPCM_TRIGGER_PRE },
		.dynamic = 1,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(playback_2),
	},
	[LINK_PLAYBACK_3] = {
		.name = "Playback_3",
		.stream_name = "Playback_3",
		.trigger = { SND_SOC_DPCM_TRIGGER_PRE,
			     SND_SOC_DPCM_TRIGGER_PRE },
		.dynamic = 1,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(playback_3),
	},
	[LINK_CAPTURE_1] = {
		.name = "Capture_1",
		.stream_name = "Capture_1",
		.trigger = { SND_SOC_DPCM_TRIGGER_PRE,
			     SND_SOC_DPCM_TRIGGER_PRE },
		.dynamic = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(capture_1),
	},
	[LINK_CAPTURE_2] = {
		.name = "Capture_2",
		.stream_name = "Capture_2",
		.trigger = { SND_SOC_DPCM_TRIGGER_PRE,
			     SND_SOC_DPCM_TRIGGER_PRE },
		.dynamic = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(capture_2),
	},
	/* BE */
	[LINK_SPEAKER] = {
		.name = "Speaker",
		.stream_name = "Speaker Playback",
		.no_pcm = 1,
		.dai_fmt = SND_SOC_DAIFMT_I2S |
			   SND_SOC_DAIFMT_CBC_CFC |
			   SND_SOC_DAIFMT_NB_NF,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(speaker),
	},
	[LINK_SPEECH] = {
		.name = "Primary Codec",
		.stream_name = "Primary Codec",
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(speech),
	},
};

/* SmartPA clocking: I2S2 is the bit-clock/frame-sync master. */
static int mx6_speaker_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	unsigned int rate = params_rate(params);
	int ret;

	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_I2S |
				  SND_SOC_DAIFMT_CBC_CFC |
				  SND_SOC_DAIFMT_NB_NF);
	if (ret && ret != -ENOTSUPP) {
		dev_err(rtd->dev, "failed to set I2S2 format: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(cpu_dai, 0, rate * MX6_SMARTPA_BCLK_FS_RATIO,
				     SND_SOC_CLOCK_OUT);
	if (ret && ret != -ENOTSUPP) {
		dev_err(rtd->dev, "failed to set I2S2 clock: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S |
				  SND_SOC_DAIFMT_CBC_CFC |
				  SND_SOC_DAIFMT_NB_NF);
	if (ret && ret != -ENOTSUPP) {
		dev_err(rtd->dev, "failed to set SmartPA format: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct snd_soc_ops mx6_speaker_ops = {
	.hw_params = mx6_speaker_hw_params,
};

static int mx6_codec_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component =
		snd_soc_rtd_to_codec(rtd, 0)->component;
	int ret;

	ret = snd_soc_card_jack_new_pins(rtd->card, "Headset Jack",
					 SND_JACK_HEADSET |
					 SND_JACK_BTN_0 | SND_JACK_BTN_1 |
					 SND_JACK_BTN_2 | SND_JACK_BTN_3,
					 &mx6_headset_jack, NULL, 0);
	if (ret)
		return ret;

	snd_jack_set_key(mx6_headset_jack.jack, SND_JACK_BTN_0,
			 KEY_PLAYPAUSE);
	snd_jack_set_key(mx6_headset_jack.jack, SND_JACK_BTN_1,
			 KEY_VOLUMEDOWN);
	snd_jack_set_key(mx6_headset_jack.jack, SND_JACK_BTN_2,
			 KEY_VOLUMEUP);
	snd_jack_set_key(mx6_headset_jack.jack, SND_JACK_BTN_3,
			 KEY_VOICECOMMAND);

	return snd_soc_component_set_jack(component, &mx6_headset_jack, NULL);
}

static struct snd_soc_card mx6_card = {
	.name = "mt6797-mx6",
	.owner = THIS_MODULE,
	.dai_link = mx6_dai_links,
	.num_links = ARRAY_SIZE(mx6_dai_links),
	.dapm_widgets = mx6_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mx6_widgets),
	.dapm_routes = mx6_routes,
	.num_dapm_routes = ARRAY_SIZE(mx6_routes),
};

static int mx6_machine_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &mx6_card;
	struct snd_soc_dai_link *links = mx6_dai_links;
	struct device_node *platform_node, *codec_node, *smartpa_node;
	int ret, i;

	card->dev = &pdev->dev;

	platform_node = of_parse_phandle(pdev->dev.of_node,
					 "mediatek,platform", 0);
	if (!platform_node) {
		dev_err(&pdev->dev, "Property 'mediatek,platform' missing or invalid\n");
		return -EINVAL;
	}

	codec_node = of_parse_phandle(pdev->dev.of_node,
				      "mediatek,audio-codec", 0);
	if (!codec_node) {
		dev_err(&pdev->dev, "Property 'mediatek,audio-codec' missing or invalid\n");
		ret = -EINVAL;
		goto err_put_platform;
	}

	/* The SmartPA is optional: a board without it plays through the PMIC. */
	smartpa_node = of_parse_phandle(pdev->dev.of_node,
					"mediatek,audio-speaker-amp", 0);

	for (i = 0; i < ARRAY_SIZE(mx6_dai_links); i++)
		links[i].platforms->of_node = platform_node;

	links[LINK_SPEECH].codecs->of_node = codec_node;
	links[LINK_SPEECH].init = mx6_codec_init;

	if (smartpa_node) {
		links[LINK_SPEAKER].codecs->of_node = smartpa_node;
		links[LINK_SPEAKER].ops = &mx6_speaker_ops;
	} else {
		links[LINK_SPEAKER].codecs = &snd_soc_dummy_dlc;
		links[LINK_SPEAKER].num_codecs = 1;
	}

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret)
		dev_err(&pdev->dev, "failed to register sound card: %d\n", ret);

	of_node_put(smartpa_node);
	of_node_put(codec_node);
err_put_platform:
	of_node_put(platform_node);

	return ret;
}

static const struct of_device_id mx6_machine_of_match[] = {
	{ .compatible = "meizu,mx6-sound" },
	{ .compatible = "mediatek,mt6797-mt6351-tfa989x-sound" },
	{}
};
MODULE_DEVICE_TABLE(of, mx6_machine_of_match);

static struct platform_driver mx6_machine_driver = {
	.driver = {
		.name = "mt6797-mx6-tfa989x",
		.of_match_table = mx6_machine_of_match,
	},
	.probe = mx6_machine_probe,
};

module_platform_driver(mx6_machine_driver);

MODULE_DESCRIPTION("Meizu MX6 MT6797/MT6351/TFA989x ALSA SoC machine driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:mt6797-mx6-tfa989x");
