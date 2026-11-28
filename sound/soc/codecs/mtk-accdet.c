// SPDX-License-Identifier: GPL-2.0-only
/*
 * mtk-accdet.c  --  MediaTek PMIC ACCDET headset jack detection
 *
 * The MT6328 (MT6735, MT6737M) and MT6353 (MT6753, MT6750) PMICs contain an
 * ACCDET block that senses the accessory plugged into the 3.5 mm jack. The
 * block drives the jack pins with a PWM current source and samples the
 * resulting voltage; its state machine walks the plug-in sequence and
 * raises an interrupt on every transition. The two comparison results
 * ("A" and "B") encode the detected accessory:
 *
 *   A = 0, B = 0 : microphone pin pulled low   -> headset with microphone
 *   A = 0, B = 1 : microphone bias loaded only -> headset with microphone
 *   A = 1, B = 1 : no microphone contact       -> headphones (3-pole)
 *
 * The driver exposes the result as a standard ALSA jack (SND_JACK_HEADSET,
 * SND_JACK_HEADPHONE, SND_JACK_MICROPHONE and the four hook-switch buttons)
 * and reports it through the machine driver's snd_soc_jack, the same way
 * the upstream MT6359 ACCDET driver works.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <sound/jack.h>
#include <sound/soc.h>

#include "mt6328.h"

/* PMIC page-0 ACCDET registers (see mt6328.h for the register map) */
#define MTK_ACCDET_RSV			0x0f14
#define MTK_ACCDET_CTRL			0x0f16
#define MTK_ACCDET_STATE_SWCTRL		0x0f18
#define MTK_ACCDET_PWM_WIDTH		0x0f1a
#define MTK_ACCDET_PWM_THRESH		0x0f1c
#define MTK_ACCDET_EN_DELAY_NUM		0x0f1e
#define MTK_ACCDET_DEBOUNCE0		0x0f20
#define MTK_ACCDET_DEBOUNCE1		0x0f22
#define MTK_ACCDET_DEBOUNCE2		0x0f24
#define MTK_ACCDET_DEBOUNCE3		0x0f26
#define MTK_ACCDET_DEBOUNCE4		0x0f28
#define MTK_ACCDET_IRQ_STS		0x0f2c
#define MTK_ACCDET_STATE_RG		0x0f30

/* ACCDET clock gate lives in TOP_CKPDN_CON2 */
#define MTK_TOP_CKPDN_CON2		0x0248
#define MTK_RG_ACCDET_CK_PDN_SFT	9

/* ACCDET_CTRL */
#define ACCDET_CTRL_EN_SFT		0

/* ACCDET_IRQ_STS */
#define ACCDET_IRQ_STS_SFT		0
#define ACCDET_IRQ_CLR_SFT		8
#define ACCDET_IRQ_EINT_CLR_SFT		10

/* ACCDET_STATE_SWCTRL: sample PWM, VTH, MIC and comparator */
#define ACCDET_SWCTRL_EN		0x0007

/* ACCDET_STATE_RG: A and B comparator results in bits [7:6] */
#define ACCDET_STATE_AB_SFT		6
#define ACCDET_STATE_AB_MASK		0x3

/* Defaults, mirroring the vendor "headset-mode-setting" property */
#define ACCDET_DEFAULT_PWM_WIDTH	0x500
#define ACCDET_DEFAULT_PWM_THRESH	0x500
#define ACCDET_DEFAULT_DEBOUNCE0	1
#define ACCDET_DEFAULT_DEBOUNCE1	0x1f0
#define ACCDET_DEFAULT_DEBOUNCE3	0x400
#define ACCDET_DEFAULT_DEBOUNCE4	0x42
#define ACCDET_DEFAULT_RISE_DELAY	0x800

struct mtk_accdet {
	struct device *dev;
	struct regmap *regmap;
	struct snd_soc_jack *jack;
	struct input_dev *idev;

	unsigned int pwm_width;
	unsigned int pwm_thresh;
	unsigned int debounce0;
	unsigned int debounce1;
	unsigned int debounce3;
	unsigned int debounce4;
	unsigned int rise_delay;
};

static void mtk_accdet_wr(struct mtk_accdet *priv, unsigned int reg,
			  unsigned int val)
{
	regmap_write(priv->regmap, reg >> 1, val);
}

static void mtk_accdet_mask(struct mtk_accdet *priv, unsigned int reg,
			    unsigned int mask, unsigned int val)
{
	regmap_update_bits(priv->regmap, reg >> 1, mask, val);
}

static unsigned int mtk_accdet_rd(struct mtk_accdet *priv, unsigned int reg)
{
	unsigned int val = 0;

	regmap_read(priv->regmap, reg >> 1, &val);

	return val;
}

static void mtk_accdet_clear_irq(struct mtk_accdet *priv)
{
	unsigned int reg;
	int retry = 0;

	reg = mtk_accdet_rd(priv, MTK_ACCDET_IRQ_STS);
	reg |= BIT(ACCDET_IRQ_CLR_SFT) | BIT(ACCDET_IRQ_EINT_CLR_SFT);
	mtk_accdet_wr(priv, MTK_ACCDET_IRQ_STS, reg);

	/* the clear bit latches until the IRQ source is released */
	while (mtk_accdet_rd(priv, MTK_ACCDET_IRQ_STS) &
	       BIT(ACCDET_IRQ_STS_SFT)) {
		if (++retry > 20)
			break;
		usleep_range(100, 120);
	}
}

static void mtk_accdet_report_state(struct mtk_accdet *priv, unsigned int ab)
{
	unsigned int state = ab & ACCDET_STATE_AB_MASK;
	bool hp, mic;
	int report;

	dev_dbg(priv->dev, "%s(): A/B = %u\n", __func__, state);

	switch (state) {
	case 0:
	case 1:
		/* microphone-equipped (four-pole) headset */
		hp = true;
		mic = true;
		report = SND_JACK_HEADSET | SND_JACK_MICROPHONE |
			 SND_JACK_HEADPHONE;
		break;
	case 3:
		/* three-pole headphones, no microphone contact */
		hp = true;
		mic = false;
		report = SND_JACK_HEADPHONE;
		break;
	default:
		hp = false;
		mic = false;
		report = 0;
		break;
	}

	/* Standard ALSA switch events so userspace sees the accessory. */
	if (priv->idev) {
		input_report_switch(priv->idev, SW_HEADPHONE_INSERT, hp);
		input_report_switch(priv->idev, SW_MICROPHONE_INSERT, mic);
		input_sync(priv->idev);
	}

	if (!priv->jack)
		return;

	snd_soc_jack_report(priv->jack, report,
			    SND_JACK_HEADSET | SND_JACK_MICROPHONE |
			    SND_JACK_HEADPHONE);
}

static irqreturn_t mtk_accdet_irq(int irq, void *data)
{
	struct mtk_accdet *priv = data;
	unsigned int ab;

	ab = (mtk_accdet_rd(priv, MTK_ACCDET_STATE_RG) >> ACCDET_STATE_AB_SFT) &
	     ACCDET_STATE_AB_MASK;

	mtk_accdet_wr(priv, MTK_ACCDET_DEBOUNCE0, priv->debounce0);
	mtk_accdet_wr(priv, MTK_ACCDET_DEBOUNCE1, priv->debounce1);

	mtk_accdet_report_state(priv, ab);
	mtk_accdet_clear_irq(priv);

	return IRQ_HANDLED;
}

static void mtk_accdet_hw_init(struct mtk_accdet *priv)
{
	/* ACCDET clock on */
	mtk_accdet_mask(priv, MTK_TOP_CKPDN_CON2,
			BIT(MTK_RG_ACCDET_CK_PDN_SFT), 0);

	/* PWM frequency and duty, debounces, rise/fall delay */
	mtk_accdet_wr(priv, MTK_ACCDET_PWM_WIDTH, priv->pwm_width - 1);
	mtk_accdet_wr(priv, MTK_ACCDET_PWM_THRESH, priv->pwm_thresh - 1);
	mtk_accdet_wr(priv, MTK_ACCDET_EN_DELAY_NUM, priv->rise_delay);
	mtk_accdet_wr(priv, MTK_ACCDET_DEBOUNCE0, priv->debounce0);
	mtk_accdet_wr(priv, MTK_ACCDET_DEBOUNCE1, priv->debounce1);
	mtk_accdet_wr(priv, MTK_ACCDET_DEBOUNCE2, 0);
	mtk_accdet_wr(priv, MTK_ACCDET_DEBOUNCE3, priv->debounce3);
	mtk_accdet_wr(priv, MTK_ACCDET_DEBOUNCE4, priv->debounce4);

	/* sample all comparators and run the state machine */
	mtk_accdet_wr(priv, MTK_ACCDET_STATE_SWCTRL, ACCDET_SWCTRL_EN);
	mtk_accdet_mask(priv, MTK_ACCDET_CTRL, BIT(ACCDET_CTRL_EN_SFT),
			BIT(ACCDET_CTRL_EN_SFT));

	mtk_accdet_clear_irq(priv);
}

static int mtk_accdet_set_jack(struct snd_soc_component *component,
			       struct snd_soc_jack *jack, void *data)
{
	struct mtk_accdet *priv = snd_soc_component_get_drvdata(component);

	priv->jack = jack;

	if (jack) {
		snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
		snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_VOLUMEDOWN);
		snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
		snd_jack_set_key(jack->jack, SND_JACK_BTN_3, KEY_VOICECOMMAND);
	}

	return 0;
}

static int mtk_accdet_suspend(struct snd_soc_component *component)
{
	struct mtk_accdet *priv = snd_soc_component_get_drvdata(component);

	mtk_accdet_mask(priv, MTK_ACCDET_CTRL, BIT(ACCDET_CTRL_EN_SFT), 0);

	return 0;
}

static int mtk_accdet_resume(struct snd_soc_component *component)
{
	struct mtk_accdet *priv = snd_soc_component_get_drvdata(component);

	mtk_accdet_hw_init(priv);

	return 0;
}

static const struct snd_soc_component_driver mtk_accdet_soc_component = {
	.name		= "mtk-accdet",
	.set_jack	= mtk_accdet_set_jack,
	.suspend	= mtk_accdet_suspend,
	.resume		= mtk_accdet_resume,
};

static int mtk_accdet_parse_dt(struct mtk_accdet *priv)
{
	struct device_node *np = priv->dev->of_node;
	u32 mode[7];
	int ret;

	priv->pwm_width = ACCDET_DEFAULT_PWM_WIDTH;
	priv->pwm_thresh = ACCDET_DEFAULT_PWM_THRESH;
	priv->debounce0 = ACCDET_DEFAULT_DEBOUNCE0;
	priv->debounce1 = ACCDET_DEFAULT_DEBOUNCE1;
	priv->debounce3 = ACCDET_DEFAULT_DEBOUNCE3;
	priv->debounce4 = ACCDET_DEFAULT_DEBOUNCE4;
	priv->rise_delay = ACCDET_DEFAULT_RISE_DELAY;

	/*
	 * <pwm_width pwm_thresh debounce0 debounce1 plugout_debounce
	 *  auxadc_debounce rise_fall_delay>
	 */
	ret = of_property_read_u32_array(np, "mediatek,headset-mode-setting",
					 mode, ARRAY_SIZE(mode));
	if (ret)
		return 0;

	priv->pwm_width = mode[0];
	priv->pwm_thresh = mode[1];
	priv->debounce0 = mode[2];
	priv->debounce1 = mode[3];
	priv->debounce3 = mode[4];
	priv->debounce4 = mode[5];
	priv->rise_delay = mode[6];

	return 0;
}

static int mtk_accdet_probe(struct platform_device *pdev)
{
	struct mtk_accdet *priv;
	struct regmap *regmap;
	struct input_dev *idev;
	int irq, ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!regmap)
		return -ENODEV;

	priv->regmap = regmap;
	priv->dev = &pdev->dev;

	idev = devm_input_allocate_device(&pdev->dev);
	if (!idev)
		return -ENOMEM;

	idev->name = "mtk-accdet";
	idev->phys = "mtk-accdet/input0";
	idev->dev.parent = &pdev->dev;
	input_set_capability(idev, EV_SW, SW_HEADPHONE_INSERT);
	input_set_capability(idev, EV_SW, SW_MICROPHONE_INSERT);
	priv->idev = idev;

	mtk_accdet_parse_dt(priv);
	mtk_accdet_hw_init(priv);

	ret = input_register_device(idev);
	if (ret) {
		dev_err(&pdev->dev, "failed to register input device: %d\n",
			ret);
		return ret;
	}

	/*
	 * The ACCDET interrupt is delivered by the PMIC interrupt
	 * controller. Boards that do not describe it still get the jack
	 * control and the initial (plugged-out) state.
	 */
	irq = platform_get_irq_optional(pdev, 0);
	if (irq > 0) {
		ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
						mtk_accdet_irq,
						IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
						"mtk-accdet", priv);
		if (ret) {
			dev_err(&pdev->dev, "failed to request IRQ %d: %d\n",
				irq, ret);
			return ret;
		}
	}

	platform_set_drvdata(pdev, priv);

	return devm_snd_soc_register_component(&pdev->dev,
					       &mtk_accdet_soc_component,
					       NULL, 0);
}

static const struct of_device_id mtk_accdet_of_match[] = {
	{ .compatible = "mediatek,mtk-accdet", },
	{}
};
MODULE_DEVICE_TABLE(of, mtk_accdet_of_match);

static struct platform_driver mtk_accdet_driver = {
	.driver = {
		.name = "mtk-accdet",
		.of_match_table = mtk_accdet_of_match,
	},
	.probe = mtk_accdet_probe,
};
module_platform_driver(mtk_accdet_driver);

MODULE_DESCRIPTION("MediaTek MT6328/MT6353 ACCDET headset jack detection");
MODULE_LICENSE("GPL");
