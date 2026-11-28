// SPDX-License-Identifier: GPL-2.0-only
/*
 * mt6735-mt6328.c  --  MT6735/MT6737/MT6753 + MT6328/MT6353 machine driver
 *
 * Sound card for the MT6735 family phones whose audio codec lives in the
 * MT6328 (MT6735/MT6737M) or MT6353 (MT6753) PMIC. Both codecs expose the
 * same register map, so both bind to the mt6328 ASoC component.
 *
 * The front-end DAIs keep the names used by the vendor MediaTek audio HAL
 * (DL1_PCM, UL1_PCM, Voice_MD1_Playback/Voice_MD1_Capture) so that the
 * userspace mixer/PCM naming stays stable.
 *
 * DL1_PCM/UL1_PCM/UL2_PCM/Capture_Mono_1 are fully dynamic front-end links
 * whose graph is built at runtime by DPCM; the static "Voice_MD1" link
 * bridges the AFE PCM port 1 to the PMIC codec and is what voice calls and
 * BT SCO streams reach through DPCM.
 */

#include <linux/input.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mt6735-afe-common.h"

#define MT6735_CODEC_DAI	"mt6328-snd-codec-aif1"

enum {
	LINK_DL1_PCM,
	LINK_UL1_PCM,
	LINK_VOICE_MD1,
};

static const struct snd_soc_dapm_widget mt6735_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
};

static const struct snd_soc_dapm_route mt6735_routes[] = {
	/* captured from the PMIC ADC, referenced to the codec's mic bias */
	{"Headset Mic", NULL, "Mic Bias"},
	{"Main Mic", NULL, "Mic Bias"},
};

static struct snd_soc_jack mt6735_headset_jack;

SND_SOC_DAILINK_DEFS(playback_1,
	DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture_1,
	DAILINK_COMP_ARRAY(COMP_CPU("UL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(voice_md1,
	DAILINK_COMP_ARRAY(COMP_CPU("PCM 1")),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, MT6735_CODEC_DAI)),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link mt6735_mt6328_dai_links[] = {
	/* FE */
	[LINK_DL1_PCM] = {
		.name = "DL1_PCM",
		.stream_name = "DL1_PCM",
		.trigger = { SND_SOC_DPCM_TRIGGER_PRE,
			     SND_SOC_DPCM_TRIGGER_PRE },
		.dynamic = 1,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(playback_1),
	},
	[LINK_UL1_PCM] = {
		.name = "UL1_PCM",
		.stream_name = "UL1_PCM",
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

static int mt6735_mt6328_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component =
		snd_soc_rtd_to_codec(rtd, 0)->component;
	int ret;

	ret = snd_soc_card_jack_new_pins(rtd->card, "Headset Jack",
					 SND_JACK_HEADSET |
					 SND_JACK_BTN_0 | SND_JACK_BTN_1 |
					 SND_JACK_BTN_2 | SND_JACK_BTN_3,
					 &mt6735_headset_jack, NULL, 0);
	if (ret)
		return ret;

	snd_jack_set_key(mt6735_headset_jack.jack, SND_JACK_BTN_0,
			 KEY_PLAYPAUSE);
	snd_jack_set_key(mt6735_headset_jack.jack, SND_JACK_BTN_1,
			 KEY_VOLUMEDOWN);
	snd_jack_set_key(mt6735_headset_jack.jack, SND_JACK_BTN_2,
			 KEY_VOLUMEUP);
	snd_jack_set_key(mt6735_headset_jack.jack, SND_JACK_BTN_3,
			 KEY_VOICECOMMAND);

	return snd_soc_component_set_jack(component, &mt6735_headset_jack,
					  NULL);
}

static struct snd_soc_card mt6735_mt6328_card = {
	.name = "mt6735-mt6328",
	.owner = THIS_MODULE,
	.dai_link = mt6735_mt6328_dai_links,
	.num_links = ARRAY_SIZE(mt6735_mt6328_dai_links),
	.dapm_widgets = mt6735_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt6735_widgets),
	.dapm_routes = mt6735_routes,
	.num_dapm_routes = ARRAY_SIZE(mt6735_routes),
};

static int mt6735_mt6328_dev_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &mt6735_mt6328_card;
	struct snd_soc_dai_link *links = mt6735_mt6328_dai_links;
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

	for (i = 0; i < ARRAY_SIZE(mt6735_mt6328_dai_links); i++)
		links[i].platforms->of_node = platform_node;

	links[LINK_VOICE_MD1].codecs->of_node = codec_node;
	links[LINK_VOICE_MD1].init = mt6735_mt6328_init;

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret)
		dev_err(&pdev->dev, "failed to register sound card: %d\n", ret);

	of_node_put(codec_node);
err_put_platform:
	of_node_put(platform_node);

	return ret;
}

static const struct of_device_id mt6735_mt6328_dt_match[] = {
	{ .compatible = "mediatek,mt6735-mt6328-sound" },
	{ .compatible = "mediatek,mt6735-mt6353-sound" },
	{ .compatible = "mediatek,mt6737m-mt6328-sound" },
	{}
};
MODULE_DEVICE_TABLE(of, mt6735_mt6328_dt_match);

static struct platform_driver mt6735_mt6328_driver = {
	.driver = {
		.name = "mt6735-mt6328",
		.of_match_table = mt6735_mt6328_dt_match,
	},
	.probe = mt6735_mt6328_dev_probe,
};

module_platform_driver(mt6735_mt6328_driver);

MODULE_DESCRIPTION("MT6735/MT6753 MT6328/MT6353 ALSA SoC machine driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:mt6735-mt6328");
