// SPDX-License-Identifier: GPL-2.0-only
/*
 * mt6755-mt6351.c  --  MT6755 + MT6351 ALSA SoC machine driver
 *
 * Sound card for the MT6755 phones whose audio codec is the MT6351 PMIC.
 * The front-end DAIs keep the vendor HAL names: the playback path is
 * "Playback_1" (AFE DL1, the MT6351 "normal audio" path) and "Playback_2"
 * (AFE DL2, the "speech" path used by voice calls), both rendered by the
 * MT6351 through the ADDA (MTKAIF) block. Voice_MD1 is the modem PCM port
 * 1, bridged to the same codec in the same way.
 */

#include <linux/input.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mt6755-afe-common.h"

#define MT6755_CODEC_DAI	"mt6351-snd-codec-aif1"

enum {
	LINK_PLAYBACK_1,
	LINK_PLAYBACK_2,
	LINK_CAPTURE_1,
	LINK_VOICE_MD1,
};

static const struct snd_soc_dapm_widget mt6755_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
};

static const struct snd_soc_dapm_route mt6755_routes[] = {
	{"Headset Mic", NULL, "Mic Bias"},
	{"Main Mic", NULL, "Mic Bias"},
};

static struct snd_soc_jack mt6755_headset_jack;

SND_SOC_DAILINK_DEFS(playback_1,
	DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback_2,
	DAILINK_COMP_ARRAY(COMP_CPU("DL2")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture_1,
	DAILINK_COMP_ARRAY(COMP_CPU("UL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(voice_md1,
	DAILINK_COMP_ARRAY(COMP_CPU("PCM 1")),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, MT6755_CODEC_DAI)),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link mt6755_mt6351_dai_links[] = {
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
	[LINK_CAPTURE_1] = {
		.name = "Capture_1",
		.stream_name = "Capture_1",
		.trigger = { SND_SOC_DPCM_TRIGGER_PRE,
			     SND_SOC_DPCM_TRIGGER_PRE },
		.dynamic = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(capture_1),
	},
	/* BE */
	[LINK_VOICE_MD1] = {
		.name = "Voice_MD1",
		.stream_name = "Voice_MD1_Playback",
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(voice_md1),
	},
};

static int mt6755_mt6351_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component =
		snd_soc_rtd_to_codec(rtd, 0)->component;
	int ret;

	ret = snd_soc_card_jack_new_pins(rtd->card, "Headset Jack",
					 SND_JACK_HEADSET |
					 SND_JACK_BTN_0 | SND_JACK_BTN_1 |
					 SND_JACK_BTN_2 | SND_JACK_BTN_3,
					 &mt6755_headset_jack, NULL, 0);
	if (ret)
		return ret;

	snd_jack_set_key(mt6755_headset_jack.jack, SND_JACK_BTN_0,
			 KEY_PLAYPAUSE);
	snd_jack_set_key(mt6755_headset_jack.jack, SND_JACK_BTN_1,
			 KEY_VOLUMEDOWN);
	snd_jack_set_key(mt6755_headset_jack.jack, SND_JACK_BTN_2,
			 KEY_VOLUMEUP);
	snd_jack_set_key(mt6755_headset_jack.jack, SND_JACK_BTN_3,
			 KEY_VOICECOMMAND);

	return snd_soc_component_set_jack(component, &mt6755_headset_jack,
					  NULL);
}

static struct snd_soc_card mt6755_mt6351_card = {
	.name = "mt6755-mt6351",
	.owner = THIS_MODULE,
	.dai_link = mt6755_mt6351_dai_links,
	.num_links = ARRAY_SIZE(mt6755_mt6351_dai_links),
	.dapm_widgets = mt6755_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt6755_widgets),
	.dapm_routes = mt6755_routes,
	.num_dapm_routes = ARRAY_SIZE(mt6755_routes),
};

static int mt6755_mt6351_dev_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &mt6755_mt6351_card;
	struct snd_soc_dai_link *links = mt6755_mt6351_dai_links;
	struct device_node *platform_node, *codec_node;
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

	for (i = 0; i < ARRAY_SIZE(mt6755_mt6351_dai_links); i++)
		links[i].platforms->of_node = platform_node;

	links[LINK_VOICE_MD1].codecs->of_node = codec_node;
	links[LINK_VOICE_MD1].init = mt6755_mt6351_init;

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret)
		dev_err(&pdev->dev, "failed to register sound card: %d\n", ret);

	of_node_put(codec_node);
err_put_platform:
	of_node_put(platform_node);

	return ret;
}

static const struct of_device_id mt6755_mt6351_dt_match[] = {
	{ .compatible = "mediatek,mt6755-mt6351-sound" },
	{}
};
MODULE_DEVICE_TABLE(of, mt6755_mt6351_dt_match);

static struct platform_driver mt6755_mt6351_driver = {
	.driver = {
		.name = "mt6755-mt6351",
		.of_match_table = mt6755_mt6351_dt_match,
	},
	.probe = mt6755_mt6351_dev_probe,
};

module_platform_driver(mt6755_mt6351_driver);

MODULE_DESCRIPTION("MT6755 MT6351 ALSA SoC machine driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:mt6755-mt6351");
