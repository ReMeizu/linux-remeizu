// SPDX-License-Identifier: GPL-2.0-only
/*
 * mt6328.c  --  MediaTek MT6328/MT6353 PMIC audio codec driver
 *
 * The MT6328 PMIC (MT6735, MT6737M) and the register-compatible MT6353
 * (MT6753, MT6750) carry the audio codec of that phone generation: a 24 bit
 * DAC with loudspeaker, headphone and handset amplifiers, a 24 bit ADC with
 * three preamplifiers and integrated mic bias, and the MTKAIF serial link
 * that connects it to the SoC audio front-end (sound/soc/mediatek/mt6735 or
 * sound/soc/mediatek/mt6755).
 *
 * Mixer control names follow the vendor MediaTek audio HAL and the open
 * ReMeizu HAL (meizu-fleet open-mtk-hal/audio_hw_remeizu_open.c):
 * Audio_Speaker_Amp_Switch, Audio_Amp_L_Switch, Audio_Amp_R_Switch,
 * Voice_Amp_Switch, Speech_MD_Usage, Audio_MicSource1_Setting.
 *
 * Register access: the PMIC wrapper regmap maps 16 bit words, so every
 * access below goes through the PMIC page-0 (word) offset
 * (MT6328_* / 2), exactly like the vendor UPmu helper does.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#include "mt6328.h"

bool mt6328_is_headset_mic(struct snd_soc_component *component);
bool mt6328_is_speaker_on(struct snd_soc_component *component);

enum {
	SUPPLY_SUBSEQ_SETTING,
	SUPPLY_SUBSEQ_ENABLE,
	SUPPLY_SUBSEQ_MICBIAS,
};

enum {
	AUDIO_ANALOG_VOLUME_HSOUT,
	AUDIO_ANALOG_VOLUME_HPOUTL,
	AUDIO_ANALOG_VOLUME_HPOUTR,
	AUDIO_ANALOG_VOLUME_MICAMP1,
	AUDIO_ANALOG_VOLUME_MICAMP2,
	AUDIO_ANALOG_VOLUME_TYPE_MAX,
};

struct mt6328_priv {
	struct device *dev;
	struct regmap *regmap;

	unsigned int dl_rate;
	unsigned int ul_rate;

	/* mixer states */
	bool spk_amp;
	bool amp_l;
	bool amp_r;
	bool voice_amp;
	unsigned int speech_md_usage;
	unsigned int mic_source;
	bool mic_bias2;

	int ana_gain[AUDIO_ANALOG_VOLUME_TYPE_MAX];
	int hp_en_counter;

	struct snd_soc_jack *jack;
};

/* ------------------------------------------------------------------------ */
/* Register helpers                                                          */
/* ------------------------------------------------------------------------ */

static void mt6328_set_bits(struct mt6328_priv *priv, unsigned int reg,
			    unsigned int mask, unsigned int val)
{
	regmap_update_bits(priv->regmap, reg >> 1, mask, val);
}

static void mt6328_write(struct mt6328_priv *priv, unsigned int reg,
			 unsigned int val)
{
	regmap_write(priv->regmap, reg >> 1, val);
}

/* ------------------------------------------------------------------------ */
/* Mixer controls                                                            */
/* ------------------------------------------------------------------------ */

static int mt6328_speaker_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct mt6328_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol));

	ucontrol->value.integer.value[0] = priv->spk_amp;

	return 0;
}

static int mt6328_speaker_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct mt6328_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol));
	unsigned int val = !!ucontrol->value.integer.value[0];

	if (val == priv->spk_amp)
		return 0;

	priv->spk_amp = val;

	/* Loudspeaker (LINEOUT) driver and its stability enhancer. */
	mt6328_set_bits(priv, MT6328_AUDDEC_ANA_CON3,
			BIT(RG_AUDLOLPWRUP_VAUDP32_SFT) |
			BIT(RG_LOOUTPUTSTBENH_VAUDP32_SFT),
			val ? BIT(RG_AUDLOLPWRUP_VAUDP32_SFT) |
			      BIT(RG_LOOUTPUTSTBENH_VAUDP32_SFT) : 0);

	/* HPL/HPR pick the loudspeaker (LINEOUT) source. */
	mt6328_set_bits(priv, MT6328_AUDDEC_ANA_CON0,
			(RG_AUDHPLMUXINPUTSEL_VAUDP32_MASK <<
			 RG_AUDHPLMUXINPUTSEL_VAUDP32_SFT) |
			(RG_AUDHPRMUXINPUTSEL_VAUDP32_MASK <<
			 RG_AUDHPRMUXINPUTSEL_VAUDP32_SFT),
			val ? (0x1 << RG_AUDHPLMUXINPUTSEL_VAUDP32_SFT) |
			      (0x1 << RG_AUDHPRMUXINPUTSEL_VAUDP32_SFT) : 0);

	return 1;
}

static int mt6328_hp_channel_put(struct mt6328_priv *priv, bool right,
				 unsigned int val)
{
	bool *state = right ? &priv->amp_r : &priv->amp_l;
	unsigned int mux_sft = right ? RG_AUDHPRMUXINPUTSEL_VAUDP32_SFT :
				       RG_AUDHPLMUXINPUTSEL_VAUDP32_SFT;
	unsigned int pwr_sft = right ? RG_AUDHPRPWRUP_VAUDP32_SFT :
				       RG_AUDHPLPWRUP_VAUDP32_SFT;
	unsigned int bias_sft = right ? RG_ABIDEC_RSVD0_VAUDP32_HPR_SFT :
					RG_ABIDEC_RSVD0_VAUDP32_HPL_SFT;

	val = !!val;

	/* Headphones take the normal audio (DAC) source. */
	mt6328_set_bits(priv, MT6328_AUDDEC_ANA_CON0,
			0x3 << mux_sft, 0x2 << mux_sft);

	if (val == *state)
		return 0;

	*state = val;

	mt6328_set_bits(priv, MT6328_AUDDEC_ANA_CON0,
			BIT(RG_HSOUTPUTSTBENH_VAUDP32_SFT) | BIT(pwr_sft),
			val ? BIT(RG_HSOUTPUTSTBENH_VAUDP32_SFT) |
			      BIT(pwr_sft) : 0);

	mt6328_set_bits(priv, MT6328_AUDDEC_ANA_CON6, BIT(bias_sft),
			val ? BIT(bias_sft) : 0);

	return 1;
}

static int mt6328_hp_l_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	return mt6328_hp_channel_put(
		snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol)),
		false, ucontrol->value.integer.value[0]);
}

static int mt6328_hp_r_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	return mt6328_hp_channel_put(
		snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol)),
		true, ucontrol->value.integer.value[0]);
}

static int mt6328_hp_l_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct mt6328_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol));

	ucontrol->value.integer.value[0] = priv->amp_l;

	return 0;
}

static int mt6328_hp_r_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct mt6328_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol));

	ucontrol->value.integer.value[0] = priv->amp_r;

	return 0;
}

static int mt6328_voice_get(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct mt6328_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol));

	ucontrol->value.integer.value[0] = priv->voice_amp;

	return 0;
}

static int mt6328_voice_put(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct mt6328_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol));
	unsigned int val = !!ucontrol->value.integer.value[0];

	if (val == priv->voice_amp)
		return 0;

	priv->voice_amp = val;

	/* Handset (receiver) driver and its stability enhancer. */
	mt6328_set_bits(priv, MT6328_AUDDEC_ANA_CON0,
			BIT(RG_AUDHSPWRUP_VAUDP32_SFT),
			val ? BIT(RG_AUDHSPWRUP_VAUDP32_SFT) : 0);
	mt6328_set_bits(priv, MT6328_AUDDEC_ANA_CON6,
			BIT(RG_ABIDEC_RSVD0_VAUDP32_HS_SFT),
			val ? BIT(RG_ABIDEC_RSVD0_VAUDP32_HS_SFT) : 0);

	return 1;
}

static const char * const mt6328_speech_md_usage_text[] = {
	"Off", "On",
};

static SOC_ENUM_SINGLE_EXT_DECL(mt6328_speech_md_usage_enum,
				mt6328_speech_md_usage_text);

static int mt6328_speech_md_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct mt6328_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol));

	ucontrol->value.enumerated.item[0] = priv->speech_md_usage;

	return 0;
}

static int mt6328_speech_md_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct mt6328_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol));
	unsigned int val = ucontrol->value.enumerated.item[0];

	if (val > 1)
		val = 1;
	if (val == priv->speech_md_usage)
		return 0;

	priv->speech_md_usage = val;

	/*
	 * Speech shares the MTKAIF downlink with normal audio; the only
	 * hardware difference is the handset amplifier input, which selects
	 * "voice playback" while a call is up and stays muted otherwise.
	 */
	mt6328_set_bits(priv, MT6328_AUDDEC_ANA_CON0,
			RG_AUDHSMUXINPUTSEL_VAUDP32_MASK <<
			RG_AUDHSMUXINPUTSEL_VAUDP32_SFT,
			(val ? 0x2 : 0x1) <<
			RG_AUDHSMUXINPUTSEL_VAUDP32_SFT);

	return 1;
}

static const char * const mt6328_mic_source_text[] = {
	"Main Mic", "Headset Mic",
};

static SOC_ENUM_SINGLE_EXT_DECL(mt6328_mic_source_enum,
				mt6328_mic_source_text);

static int mt6328_mic_source_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct mt6328_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol));

	ucontrol->value.enumerated.item[0] = priv->mic_source;

	return 0;
}

static int mt6328_mic_source_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct mt6328_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol));
	unsigned int val = ucontrol->value.enumerated.item[0];

	if (val > 1)
		val = 1;
	if (val == priv->mic_source)
		return 0;

	priv->mic_source = val;

	/*
	 * Headset microphone: MICBIAS1 and the second preamplifier input.
	 * Main microphone: MICBIAS0 and the first preamplifier input. The
	 * DAPM graph keeps both preamplifiers powered, so switching here only
	 * moves the capture source.
	 */
	mt6328_set_bits(priv, MT6328_AUDENC_ANA_CON0,
			RG_AUDPREAMPLINPUTSEL_MASK <<
			RG_AUDPREAMPLINPUTSEL_SFT,
			(val ? 0x2 : 0x1) << RG_AUDPREAMPLINPUTSEL_SFT);
	mt6328_set_bits(priv, MT6328_AUDENC_ANA_CON1,
			RG_AUDPREAMPRINPUTSEL_MASK <<
			RG_AUDPREAMPRINPUTSEL_SFT,
			(val ? 0x2 : 0x1) << RG_AUDPREAMPRINPUTSEL_SFT);

	return 1;
}

static const unsigned int mt6328_hp_gain_tlv[] = {
	TLV_DB_RANGE_HEAD(2),
	0x00, 0x12, TLV_DB_SCALE_ITEM(-4000, 200, 0),
	0x1f, 0x1f, TLV_DB_SCALE_ITEM(600, 0, 0),
};

static const DECLARE_TLV_DB_SCALE(mt6328_pga_tlv, 0, 600, 0);

static const struct snd_kcontrol_new mt6328_snd_controls[] = {
	/* Amplifier switches, named as in the vendor / ReMeizu audio HAL */
	SOC_SINGLE_EXT("Audio_Speaker_Amp_Switch", 0, 0, 1, 0,
		       mt6328_speaker_get, mt6328_speaker_put),
	SOC_SINGLE_EXT("Speaker_Amp_Switch", 0, 0, 1, 0,
		       mt6328_speaker_get, mt6328_speaker_put),
	SOC_SINGLE_EXT("Audio_Amp_L_Switch", 0, 0, 1, 0,
		       mt6328_hp_l_get, mt6328_hp_l_put),
	SOC_SINGLE_EXT("Audio_Amp_R_Switch", 0, 0, 1, 0,
		       mt6328_hp_r_get, mt6328_hp_r_put),
	SOC_SINGLE_EXT("Voice_Amp_Switch", 0, 0, 1, 0,
		       mt6328_voice_get, mt6328_voice_put),

	/* Vendor HAL routing requests */
	SOC_ENUM_EXT("Speech_MD_Usage", mt6328_speech_md_usage_enum,
		     mt6328_speech_md_get, mt6328_speech_md_put),
	SOC_ENUM_EXT("Audio_MicSource1_Setting", mt6328_mic_source_enum,
		     mt6328_mic_source_get, mt6328_mic_source_put),

	/* Analog gains */
	SOC_DOUBLE_TLV("Headphone Volume", MT6328_ZCD_CON2 >> 1, 0, 8, 0x12, 1,
		       mt6328_hp_gain_tlv),
	SOC_SINGLE_TLV("Handset Volume", MT6328_ZCD_CON3 >> 1, 0, 0x12, 1,
		       mt6328_hp_gain_tlv),
	SOC_DOUBLE_R_TLV("PGA Volume", MT6328_AUDENC_ANA_CON0 >> 1,
			 MT6328_AUDENC_ANA_CON1 >> 1, 8, 4, 0,
			 mt6328_pga_tlv),
};

/* ------------------------------------------------------------------------ */
/* DAPM helpers                                                              */
/* ------------------------------------------------------------------------ */

static int mt_reg_set_clr_event(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(cmpnt);
	unsigned int mask = BIT(w->shift);
	unsigned int val;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		val = w->off_val;
		break;
	case SND_SOC_DAPM_PRE_PMD:
		val = w->on_val;
		break;
	default:
		return 0;
	}

	mt6328_set_bits(priv, w->reg, mask, val ? mask : 0);

	return 0;
}

static int mt_ncp_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(cmpnt);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* 26 MHz CLKSQ on, AUDGLB out of reset */
		mt6328_set_bits(priv, MT6328_TOP_CLKSQ, 0x0001, 0x0001);
		usleep_range(250, 270);
		mt6328_set_bits(priv, MT6328_AUDDEC_ANA_CON9,
				BIT(RG_AUDGLB_PWRDN_VA32_SFT), 0);
		break;
	case SND_SOC_DAPM_POST_PMU:
		usleep_range(250, 270);
		break;
	default:
		break;
	}

	return 0;
}

static int mt_sgen_event(struct snd_soc_dapm_widget *w,
			 struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(cmpnt);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		mt6328_set_bits(priv, MT6328_AFE_SGEN_CFG0, 0x0010, 0x0008);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		mt6328_set_bits(priv, MT6328_AFE_SGEN_CFG0, 0x0010, 0x0000);
		break;
	default:
		break;
	}

	return 0;
}

static unsigned int mt6328_get_dl_src_val(unsigned int rate)
{
	switch (rate) {
	case 8000:
		return 0;
	case 16000:
		return 3;
	case 32000:
		return 6;
	case 44100:
		return 7;
	case 96000:
	case 192000:
	case 48000:
	default:
		return 8;
	}
}

static unsigned int mt6328_get_ul_src_val(unsigned int rate)
{
	switch (rate) {
	case 8000:
		return 0;
	case 16000:
		return 1;
	case 32000:
		return 2;
	case 96000:
		return 4;
	case 192000:
		return 5;
	case 48000:
	default:
		return 3;
	}
}

static int mt_aif_in_event(struct snd_soc_dapm_widget *w,
			   struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(cmpnt);
	unsigned int src = mt6328_get_dl_src_val(priv->dl_rate);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* uplink/downlink front-end on */
		mt6328_set_bits(priv, MT6328_AFE_UL_DL_CON0,
				BIT(RG_AFE_ON_SFT), BIT(RG_AFE_ON_SFT));

		/* SDM FIFO clock, scrambler, SDM on */
		mt6328_write(priv, MT6328_AFUNC_AUD_CON2, 0x0006);
		mt6328_write(priv, MT6328_AFUNC_AUD_CON0, 0xc3a1);
		mt6328_write(priv, MT6328_AFUNC_AUD_CON2, 0x0003);
		mt6328_write(priv, MT6328_AFUNC_AUD_CON2, 0x000b);
		/* attenuation gain */
		mt6328_write(priv, MT6328_AFE_DL_SDM_CON1, 0x001e);

		/* downlink sample rate on the MTKAIF link */
		mt6328_write(priv, MT6328_AFE_PMIC_NEWIF_CFG0,
			     (src << 12) | 0x0330);
		mt6328_write(priv, MT6328_AFE_PMIC_NEWIF_CFG2, 0x8300);

		/* unmute and start the downlink source */
		mt6328_write(priv, MT6328_AFE_DL_SRC2_CON0_L,
			     (src << 12) | 0x0803 | BIT(RG_DL_2_MUTE_OFF_SFT));
		break;
	case SND_SOC_DAPM_POST_PMD:
		mt6328_set_bits(priv, MT6328_AFE_DL_SRC2_CON0_L,
				BIT(RG_DL_2_SRC_ON_TMP_CTL_PRE_SFT), 0);
		mt6328_set_bits(priv, MT6328_AFE_UL_DL_CON0,
				BIT(RG_AFE_ON_SFT), 0);
		break;
	default:
		break;
	}

	return 0;
}

static int mt_aif_out_event(struct snd_soc_dapm_widget *w,
			    struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(cmpnt);
	unsigned int src = mt6328_get_ul_src_val(priv->ul_rate);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/*
		 * DC offset compensation clock generator, using the same
		 * divider and reference clock as the vendor BSP sequence
		 * (dcclk_div = 11'b00100000011, dcclk_ref_ck_sel = 2'b00).
		 */
		regmap_write(priv->regmap, MT6328_AFE_DCCLK_CFG0 >> 1, 0x2062);
		regmap_write(priv->regmap, MT6328_AFE_DCCLK_CFG0 >> 1, 0x2060);
		regmap_write(priv->regmap, MT6328_AFE_DCCLK_CFG0 >> 1,
			     0x2060 | BIT(RG_DCCLK_GEN_ON_SFT));

		/* downlink front-end as well: capture needs the MTKAIF clock */
		mt6328_set_bits(priv, MT6328_AFE_UL_DL_CON0,
				BIT(RG_AFE_ON_SFT), BIT(RG_AFE_ON_SFT));

		/* uplink sample rate */
		mt6328_set_bits(priv, MT6328_AFE_UL_SRC0_CON0_H,
				0x000e, src << RG_UL_SRC0_MODE_SFT);
		break;
	case SND_SOC_DAPM_POST_PMD:
		mt6328_set_bits(priv, MT6328_AFE_DCCLK_CFG0,
				BIT(RG_DCCLK_GEN_ON_SFT), 0);
		break;
	default:
		break;
	}

	return 0;
}

static int mt_adc_clkgen_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(cmpnt);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* ADC clock generator: divided by 2 (normal) */
		mt6328_set_bits(priv, MT6328_AUDENC_ANA_CON3,
				0x3 << RG_AUDADCLK_MODE_SFT, 0);
		break;
	case SND_SOC_DAPM_POST_PMU:
		/* ADC clock from the 13 MHz CLKSQ */
		mt6328_set_bits(priv, MT6328_AUDENC_ANA_CON3,
				0x3 << RG_AUDADCLK_SRC_SFT, 0);
		break;
	default:
		break;
	}

	return 0;
}

static int mt_pga_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(cmpnt);
	bool right = !strcmp(w->name, "PGA R");
	unsigned int dcc = right ? RG_AUDPREAMPRDCCEN_SFT :
				   RG_AUDPREAMPLDCCEN_SFT;
	unsigned int pre = right ? RG_AUDPREAMPRDCPRECHARGE_SFT :
				   RG_AUDPREAMPLDCPRECHARGE_SFT;
	unsigned int reg = right ? MT6328_AUDENC_ANA_CON1 :
				   MT6328_AUDENC_ANA_CON0;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* DCC mode with precharge */
		mt6328_set_bits(priv, reg, 0x3 << pre, 0x1 << pre);
		mt6328_set_bits(priv, reg, 0x3 << dcc, BIT(dcc));
		break;
	case SND_SOC_DAPM_POST_PMU:
		usleep_range(100, 120);
		mt6328_set_bits(priv, reg, 0x3 << pre, 0);
		break;
	default:
		break;
	}

	return 0;
}

static int mt_mic_bias_0_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(cmpnt);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* LowPower 0 = normal, MICBIAS0 = 1.9 V */
		mt6328_set_bits(priv, MT6328_AUDENC_ANA_CON9,
				0x3 << RG_AUDMICBIAS0LOWPEN_SFT, 0);
		mt6328_set_bits(priv, MT6328_AUDENC_ANA_CON9,
				0x7 << RG_AUDMICBIAS0VREF_SFT,
				0x2 << RG_AUDMICBIAS0VREF_SFT);
		break;
	case SND_SOC_DAPM_POST_PMD:
		mt6328_set_bits(priv, MT6328_AUDENC_ANA_CON9,
				0x7 << RG_AUDMICBIAS0VREF_SFT, 0);
		break;
	default:
		break;
	}

	return 0;
}

static int mt_mic_bias_1_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(cmpnt);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* headset mic bias: LowPower 0, MICBIAS1 = 2.7 V */
		mt6328_set_bits(priv, MT6328_AUDENC_ANA_CON10,
				0x3 << RG_AUDMICBIAS1LOWPEN_SFT, 0);
		mt6328_set_bits(priv, MT6328_AUDENC_ANA_CON10,
				0x7 << RG_AUDMICBIAS1VREF_SFT,
				0x7 << RG_AUDMICBIAS1VREF_SFT);
		break;
	case SND_SOC_DAPM_POST_PMD:
		mt6328_set_bits(priv, MT6328_AUDENC_ANA_CON10,
				0x7 << RG_AUDMICBIAS1VREF_SFT, 0);
		break;
	default:
		break;
	}

	return 0;
}

static int mt_mic_bias_2_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(cmpnt);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* reference mic bias: LowPower 0, MICBIAS2 = 1.9 V */
		mt6328_set_bits(priv, MT6328_AUDENC_ANA_CON9,
				0x3 << RG_AUDMICBIAS2LOWPEN_SFT, 0);
		mt6328_set_bits(priv, MT6328_AUDENC_ANA_CON9,
				0x7 << RG_AUDMICBIAS2VREF_SFT,
				0x2 << RG_AUDMICBIAS2VREF_SFT);
		break;
	case SND_SOC_DAPM_POST_PMD:
		mt6328_set_bits(priv, MT6328_AUDENC_ANA_CON9,
				0x7 << RG_AUDMICBIAS2VREF_SFT, 0);
		break;
	default:
		break;
	}

	return 0;
}

/* DAPM mux controls */
static const char * const hp_in_mux_map[] = {
	"Open", "LoudSPK Playback", "Audio Playback", "Test Mode",
};
static const unsigned int hp_in_mux_map_value[] = { 0x0, 0x1, 0x2, 0x3 };

static SOC_VALUE_ENUM_SINGLE_DECL(hpl_in_mux_map_enum, MT6328_AUDDEC_ANA_CON0 >> 1,
				  RG_AUDHPLMUXINPUTSEL_VAUDP32_SFT,
				  RG_AUDHPLMUXINPUTSEL_VAUDP32_MASK,
				  hp_in_mux_map, hp_in_mux_map_value);
static const struct snd_kcontrol_new hpl_in_mux_control =
	SOC_DAPM_ENUM("HPL Select", hpl_in_mux_map_enum);

static SOC_VALUE_ENUM_SINGLE_DECL(hpr_in_mux_map_enum, MT6328_AUDDEC_ANA_CON0 >> 1,
				  RG_AUDHPRMUXINPUTSEL_VAUDP32_SFT,
				  RG_AUDHPRMUXINPUTSEL_VAUDP32_MASK,
				  hp_in_mux_map, hp_in_mux_map_value);
static const struct snd_kcontrol_new hpr_in_mux_control =
	SOC_DAPM_ENUM("HPR Select", hpr_in_mux_map_enum);

static const char * const rcv_in_mux_map[] = {
	"Open", "Mute", "Voice Playback", "Test Mode",
};
static const unsigned int rcv_in_mux_map_value[] = { 0x0, 0x1, 0x2, 0x3 };

static SOC_VALUE_ENUM_SINGLE_DECL(rcv_in_mux_map_enum, MT6328_AUDDEC_ANA_CON0 >> 1,
				  RG_AUDHSMUXINPUTSEL_VAUDP32_SFT,
				  RG_AUDHSMUXINPUTSEL_VAUDP32_MASK,
				  rcv_in_mux_map, rcv_in_mux_map_value);
static const struct snd_kcontrol_new rcv_in_mux_control =
	SOC_DAPM_ENUM("RCV Select", rcv_in_mux_map_enum);

static const char * const dac_in_mux_map[] = {
	"Normal Path", "Sgen",
};
static const unsigned int dac_in_mux_map_value[] = { 0x0, 0x1 };

static SOC_VALUE_ENUM_SINGLE_DECL(dac_in_mux_map_enum, MT6328_AFE_TOP_CON0 >> 1,
				  RG_DL_SINE_ON_SFT, RG_DL_SINE_ON_MASK,
				  dac_in_mux_map, dac_in_mux_map_value);
static const struct snd_kcontrol_new dac_in_mux_control =
	SOC_DAPM_ENUM("DAC Select", dac_in_mux_map_enum);

static SOC_VALUE_ENUM_SINGLE_DECL(aif_out_mux_map_enum, MT6328_AFE_TOP_CON0 >> 1,
				  RG_UL_SINE_ON_SFT, RG_UL_SINE_ON_MASK,
				  dac_in_mux_map, dac_in_mux_map_value);
static const struct snd_kcontrol_new aif_out_mux_control =
	SOC_DAPM_ENUM("AIF Out Select", aif_out_mux_map_enum);

static const char * const adc_left_mux_map[] = {
	"Idle", "AIN0", "Left Preamplifier", "Idle_1",
};
static const unsigned int adc_left_mux_map_value[] = { 0x0, 0x1, 0x2, 0x3 };

static SOC_VALUE_ENUM_SINGLE_DECL(adc_left_mux_map_enum, MT6328_AUDENC_ANA_CON0 >> 1,
				  RG_AUDADCLINPUTSEL_SFT,
				  RG_AUDADCLINPUTSEL_MASK,
				  adc_left_mux_map, adc_left_mux_map_value);
static const struct snd_kcontrol_new adc_left_mux_control =
	SOC_DAPM_ENUM("ADC L Select", adc_left_mux_map_enum);

static const char * const adc_right_mux_map[] = {
	"Idle", "AIN0", "Right Preamplifier", "Idle_1",
};
static const unsigned int adc_right_mux_map_value[] = { 0x0, 0x1, 0x2, 0x3 };

static SOC_VALUE_ENUM_SINGLE_DECL(adc_right_mux_map_enum, MT6328_AUDENC_ANA_CON1 >> 1,
				  RG_AUDADCRINPUTSEL_SFT,
				  RG_AUDADCRINPUTSEL_MASK,
				  adc_right_mux_map, adc_right_mux_map_value);
static const struct snd_kcontrol_new adc_right_mux_control =
	SOC_DAPM_ENUM("ADC R Select", adc_right_mux_map_enum);

static const char * const pga_left_mux_map[] = {
	"MUTE", "AIN0", "AIN1", "AIN2",
};
static const unsigned int pga_left_mux_map_value[] = { 0x0, 0x1, 0x2, 0x3 };

static SOC_VALUE_ENUM_SINGLE_DECL(pga_left_mux_map_enum, MT6328_AUDENC_ANA_CON0 >> 1,
				  RG_AUDPREAMPLINPUTSEL_SFT,
				  RG_AUDPREAMPLINPUTSEL_MASK,
				  pga_left_mux_map, pga_left_mux_map_value);
static const struct snd_kcontrol_new pga_left_mux_control =
	SOC_DAPM_ENUM("PGA L Select", pga_left_mux_map_enum);

static const char * const pga_right_mux_map[] = {
	"MUTE", "AIN0", "AIN3", "AIN2",
};
static const unsigned int pga_right_mux_map_value[] = { 0x0, 0x1, 0x2, 0x3 };

static SOC_VALUE_ENUM_SINGLE_DECL(pga_right_mux_map_enum, MT6328_AUDENC_ANA_CON1 >> 1,
				  RG_AUDPREAMPRINPUTSEL_SFT,
				  RG_AUDPREAMPRINPUTSEL_MASK,
				  pga_right_mux_map, pga_right_mux_map_value);
static const struct snd_kcontrol_new pga_right_mux_control =
	SOC_DAPM_ENUM("PGA R Select", pga_right_mux_map_enum);

/* DAPM widgets */
static const struct snd_soc_dapm_widget mt6328_dapm_widgets[] = {
	/* Digital clocks */
	SND_SOC_DAPM_SUPPLY("AUDIO_TOP_AFE_CTL", MT6328_AFE_AUDIO_TOP_CON0 >> 1,
			    AUD_TOP_PDN_AFE_CTL_SFT, 1, NULL, 0),
	SND_SOC_DAPM_SUPPLY("AUDIO_TOP_PWR_CLK", MT6328_AFE_AUDIO_TOP_CON0 >> 1,
			    AUD_TOP_PWR_CLK_DIS_CTL_SFT, 1, NULL, 0),

	SND_SOC_DAPM_SUPPLY("NCP", MT6328_AUDDEC_ANA_CON0 >> 1,
			    RG_AUDDACLPWRUP_VAUDP32_SFT, 0,
			    mt_ncp_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),

	SND_SOC_DAPM_SUPPLY("DL Digital Clock", SND_SOC_NOPM, 0, 0, NULL, 0),

	/* Global supplies */
	SND_SOC_DAPM_SUPPLY("AUDGLB", MT6328_AUDDEC_ANA_CON9 >> 1,
			    RG_AUDGLB_PWRDN_VA32_SFT, 1, NULL, 0),
	SND_SOC_DAPM_SUPPLY("CLKSQ Audio", MT6328_TOP_CLKSQ >> 1, 0, 0,
			    mt_reg_set_clr_event,
			    SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_SUPPLY("ZCD13M_CK", MT6328_TOP_CKPDN_CON0 >> 1,
			    RG_ZCD13M_CK_PDN_SFT, 1, mt_reg_set_clr_event,
			    SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_SUPPLY("AUD_CK", MT6328_TOP_CKPDN_CON0 >> 1,
			    RG_AUD_CK_PDN_SFT, 1, mt_reg_set_clr_event,
			    SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_SUPPLY("AUDIF_CK", MT6328_TOP_CKPDN_CON0 >> 1,
			    RG_AUDIF_CK_PDN_SFT, 1, mt_reg_set_clr_event,
			    SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_SUPPLY("AUDNCP_CK", MT6328_TOP_CKPDN_CON0 >> 1,
			    RG_AUDNCP_CK_PDN_SFT, 1, mt_reg_set_clr_event,
			    SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),

	/* AIF RX (downlink from the SoC AFE) */
	SND_SOC_DAPM_AIF_IN_E("AIF_RX", "AIF1 Playback", 0,
			      MT6328_AFE_DL_SRC2_CON0_L >> 1,
			      RG_DL_2_SRC_ON_TMP_CTL_PRE_SFT, 0,
			      mt_aif_in_event,
			      SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	/* Downlink supplies */
	SND_SOC_DAPM_SUPPLY("DL Power Supply", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("NV Regulator", MT6328_AUDDEC_ANA_CON9 >> 1,
			    RG_LCLDO_DEC_EN_VA32_SFT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("AUD_CLK", MT6328_AUDDEC_ANA_CON9 >> 1,
			    RG_RSTB_DECODER_VA32_SFT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("IBIST", MT6328_AUDDEC_ANA_CON9 >> 1,
			    RG_AUDIBIASPWRDN_VAUDP32_SFT, 1, NULL, 0),

	/* DAC */
	SND_SOC_DAPM_MUX("DAC In Mux", SND_SOC_NOPM, 0, 0,
			 &dac_in_mux_control),
	SND_SOC_DAPM_DAC("DACL", NULL, MT6328_AUDDEC_ANA_CON0 >> 1,
			 RG_AUDDACLPWRUP_VAUDP32_SFT, 0),
	SND_SOC_DAPM_SUPPLY("DACL_BIASGEN", MT6328_AUDDEC_ANA_CON0 >> 1,
			    RG_AUD_DAC_PWL_UP_VA32_SFT, 0, NULL, 0),
	SND_SOC_DAPM_DAC("DACR", NULL, MT6328_AUDDEC_ANA_CON0 >> 1,
			 RG_AUDDACRPWRUP_VAUDP32_SFT, 0),
	SND_SOC_DAPM_SUPPLY("DACR_BIASGEN", MT6328_AUDDEC_ANA_CON0 >> 1,
			    RG_AUD_DAC_PWR_UP_VA32_SFT, 0, NULL, 0),

	/* Loudspeaker (LINEOUT) */
	SND_SOC_DAPM_SUPPLY("LO Stability Enh", MT6328_AUDDEC_ANA_CON3 >> 1,
			    RG_LOOUTPUTSTBENH_VAUDP32_SFT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("LOL Bias Gen", MT6328_AUDDEC_ANA_CON6 >> 1,
			    RG_ABIDEC_RSVD0_VAUDP32_LOL_SFT, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("LOL Buffer", MT6328_AUDDEC_ANA_CON3 >> 1,
			     RG_AUDLOLPWRUP_VAUDP32_SFT, 0, NULL, 0),

	/* Headphone */
	SND_SOC_DAPM_MUX("HPL Mux", SND_SOC_NOPM, 0, 0, &hpl_in_mux_control),
	SND_SOC_DAPM_MUX("HPR Mux", SND_SOC_NOPM, 0, 0, &hpr_in_mux_control),
	SND_SOC_DAPM_OUT_DRV("HPL Power", MT6328_AUDDEC_ANA_CON0 >> 1,
			     RG_AUDHPLPWRUP_VAUDP32_SFT, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("HPR Power", MT6328_AUDDEC_ANA_CON0 >> 1,
			     RG_AUDHPRPWRUP_VAUDP32_SFT, 0, NULL, 0),

	/* Handset (receiver) */
	SND_SOC_DAPM_MUX("RCV Mux", SND_SOC_NOPM, 0, 0, &rcv_in_mux_control),
	SND_SOC_DAPM_SUPPLY("RCV Stability Enh", MT6328_AUDDEC_ANA_CON1 >> 1,
			    RG_HSOUTPUTSTBENH_VAUDP32_SFT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("RCV Bias Gen", MT6328_AUDDEC_ANA_CON6 >> 1,
			    RG_ABIDEC_RSVD0_VAUDP32_HS_SFT, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("RCV Buffer", MT6328_AUDDEC_ANA_CON0 >> 1,
			     RG_AUDHSPWRUP_VAUDP32_SFT, 0, NULL, 0),

	/* Outputs */
	SND_SOC_DAPM_OUTPUT("Receiver"),
	SND_SOC_DAPM_OUTPUT("Headphone L"),
	SND_SOC_DAPM_OUTPUT("Headphone R"),
	SND_SOC_DAPM_OUTPUT("LINEOUT L"),

	/* SGEN (sine generator) */
	SND_SOC_DAPM_SUPPLY("SGEN DL Enable", MT6328_AFE_SGEN_CFG0 >> 1,
			    SGEN_C_DAC_EN_CTL_SFT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("SGEN MUTE", MT6328_AFE_SGEN_CFG0 >> 1,
			    SGEN_C_MUTE_SW_CTL_SFT, 1, mt_sgen_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_SUPPLY("SGEN DL SRC", MT6328_AFE_DL_SRC2_CON0_L >> 1,
			    RG_DL_2_SRC_ON_TMP_CTL_PRE_SFT, 0, NULL, 0),
	SND_SOC_DAPM_INPUT("SGEN DL"),

	/* Uplink */
	SND_SOC_DAPM_AIF_OUT_E("AIF1TX", "AIF1 Capture", 0,
			       MT6328_AFE_UL_SRC0_CON0_L >> 1,
			       RG_UL_SRC_ON_TMP_CTL_SFT, 0,
			       mt_aif_out_event,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SUPPLY_S("ADC CLKGEN", SUPPLY_SUBSEQ_ENABLE,
			      MT6328_AUDENC_ANA_CON3 >> 1, RG_AUDADCCLKRSTB_SFT, 0,
			      mt_adc_clkgen_event,
			      SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),

	/* Uplink muxes and converters */
	SND_SOC_DAPM_MUX("AIF Out Mux", SND_SOC_NOPM, 0, 0,
			 &aif_out_mux_control),
	SND_SOC_DAPM_MUX("ADC L Mux", SND_SOC_NOPM, 0, 0,
			 &adc_left_mux_control),
	SND_SOC_DAPM_MUX("ADC R Mux", SND_SOC_NOPM, 0, 0,
			 &adc_right_mux_control),
	SND_SOC_DAPM_ADC("ADC L", NULL, MT6328_AUDENC_ANA_CON0 >> 1,
			 RG_AUDADCLPWRUP_SFT, 0),
	SND_SOC_DAPM_ADC("ADC R", NULL, MT6328_AUDENC_ANA_CON1 >> 1,
			 RG_AUDADCRPWRUP_SFT, 0),

	SND_SOC_DAPM_MUX("PGA L Mux", SND_SOC_NOPM, 0, 0,
			 &pga_left_mux_control),
	SND_SOC_DAPM_MUX("PGA R Mux", SND_SOC_NOPM, 0, 0,
			 &pga_right_mux_control),
	SND_SOC_DAPM_PGA_E("PGA L", MT6328_AUDENC_ANA_CON0 >> 1,
			   RG_AUDPREAMPLON_SFT, 0, NULL, 0, mt_pga_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_PGA_E("PGA R", MT6328_AUDENC_ANA_CON1 >> 1,
			   RG_AUDPREAMPRON_SFT, 0, NULL, 0, mt_pga_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),

	/* Mic biases */
	SND_SOC_DAPM_SUPPLY_S("Mic Bias 0", SUPPLY_SUBSEQ_MICBIAS,
			      MT6328_AUDENC_ANA_CON9 >> 1, RG_AUDPWDBMICBIAS0_SFT,
			      0, mt_mic_bias_0_event,
			      SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY_S("Mic Bias 1", SUPPLY_SUBSEQ_MICBIAS,
			      MT6328_AUDENC_ANA_CON10 >> 1,
			      RG_AUDPWDBMICBIAS1_SFT, 0, mt_mic_bias_1_event,
			      SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY_S("Mic Bias 2", SUPPLY_SUBSEQ_MICBIAS,
			      MT6328_AUDENC_ANA_CON9 >> 1, RG_AUDPWDBMICBIAS2_SFT,
			      0, mt_mic_bias_2_event,
			      SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY_S("Mic Bias 1 DCC pull high",
			      SUPPLY_SUBSEQ_MICBIAS, MT6328_AUDENC_ANA_CON10 >> 1,
			      RG_AUDMICBIAS1DCSW1NEN_SFT, 0, NULL, 0),

	/* Inputs */
	SND_SOC_DAPM_INPUT("AIN0"),
	SND_SOC_DAPM_INPUT("AIN1"),
	SND_SOC_DAPM_INPUT("AIN2"),
	SND_SOC_DAPM_INPUT("AIN3"),
};

static const struct snd_soc_dapm_route mt6328_dapm_routes[] = {
	/* Capture */
	{"AIF1TX", NULL, "AIF Out Mux"},
	{"AIF1TX", NULL, "AUDGLB"},
	{"AIF1TX", NULL, "CLKSQ Audio"},
	{"AIF1TX", NULL, "AUDIO_TOP_AFE_CTL"},
	{"AIF1TX", NULL, "AUDIO_TOP_PWR_CLK"},

	{"AIF Out Mux", "Normal Path", "ADC L"},
	{"AIF Out Mux", "Normal Path", "ADC R"},

	{"ADC L", NULL, "ADC L Mux"},
	{"ADC L", NULL, "AUD_CK"},
	{"ADC L", NULL, "AUDIF_CK"},
	{"ADC L", NULL, "ADC CLKGEN"},
	{"ADC R", NULL, "ADC R Mux"},
	{"ADC R", NULL, "AUD_CK"},
	{"ADC R", NULL, "AUDIF_CK"},
	{"ADC R", NULL, "ADC CLKGEN"},

	{"ADC L Mux", "AIN0", "AIN0"},
	{"ADC L Mux", "Left Preamplifier", "PGA L"},
	{"ADC R Mux", "AIN0", "AIN0"},
	{"ADC R Mux", "Right Preamplifier", "PGA R"},

	{"PGA L", NULL, "PGA L Mux"},
	{"PGA R", NULL, "PGA R Mux"},
	{"PGA L Mux", "AIN0", "AIN0"},
	{"PGA L Mux", "AIN1", "AIN1"},
	{"PGA L Mux", "AIN2", "AIN2"},
	{"PGA R Mux", "AIN0", "AIN0"},
	{"PGA R Mux", "AIN3", "AIN3"},
	{"PGA R Mux", "AIN2", "AIN2"},

	{"AIN0", NULL, "Mic Bias 0"},
	{"AIN1", NULL, "Mic Bias 0"},
	{"AIN2", NULL, "Mic Bias 1"},
	{"AIN2", NULL, "Mic Bias 2"},
	{"AIN2", NULL, "Mic Bias 1 DCC pull high"},
	{"AIN3", NULL, "Mic Bias 1"},
	{"AIN3", NULL, "Mic Bias 1 DCC pull high"},

	/* Downlink supplies */
	{"DL Power Supply", NULL, "AUDGLB"},
	{"DL Power Supply", NULL, "CLKSQ Audio"},
	{"DL Power Supply", NULL, "ZCD13M_CK"},
	{"DL Power Supply", NULL, "AUD_CK"},
	{"DL Power Supply", NULL, "AUDIF_CK"},
	{"DL Power Supply", NULL, "AUDNCP_CK"},
	{"DL Power Supply", NULL, "NV Regulator"},
	{"DL Power Supply", NULL, "AUD_CLK"},
	{"DL Power Supply", NULL, "IBIST"},

	{"DL Digital Clock", NULL, "AUDIO_TOP_AFE_CTL"},
	{"DL Digital Clock", NULL, "AUDIO_TOP_PWR_CLK"},
	{"DL Digital Clock", NULL, "NCP"},

	{"AIF_RX", NULL, "DL Digital Clock"},

	/* Downlink path */
	{"DAC In Mux", "Normal Path", "AIF_RX"},
	{"DAC In Mux", "Sgen", "SGEN DL"},
	{"SGEN DL", NULL, "SGEN DL SRC"},
	{"SGEN DL", NULL, "SGEN MUTE"},
	{"SGEN DL", NULL, "SGEN DL Enable"},
	{"SGEN DL", NULL, "DL Digital Clock"},

	{"DACL", NULL, "DAC In Mux"},
	{"DACL", NULL, "DL Power Supply"},
	{"DACL", NULL, "DACL_BIASGEN"},
	{"DACR", NULL, "DAC In Mux"},
	{"DACR", NULL, "DL Power Supply"},
	{"DACR", NULL, "DACR_BIASGEN"},

	{"LOL Buffer", NULL, "DACL"},
	{"LOL Buffer", NULL, "LO Stability Enh"},
	{"LOL Buffer", NULL, "LOL Bias Gen"},
	{"LINEOUT L", NULL, "LOL Buffer"},

	{"HPL Mux", "Audio Playback", "DACL"},
	{"HPR Mux", "Audio Playback", "DACR"},
	{"HPL Mux", "LoudSPK Playback", "DACL"},
	{"HPR Mux", "LoudSPK Playback", "DACR"},
	{"HPL Power", NULL, "HPL Mux"},
	{"HPR Power", NULL, "HPR Mux"},
	{"Headphone L", NULL, "HPL Power"},
	{"Headphone R", NULL, "HPR Power"},

	{"RCV Mux", "Voice Playback", "DACL"},
	{"RCV Buffer", NULL, "RCV Mux"},
	{"RCV Buffer", NULL, "RCV Stability Enh"},
	{"RCV Buffer", NULL, "RCV Bias Gen"},
	{"Receiver", NULL, "RCV Buffer"},
};

/* ------------------------------------------------------------------------ */
/* Headset jack / exported state                                             */
/* ------------------------------------------------------------------------ */

/*
 * Plug detection itself is done by the MT6328 ACCDET block (see
 * mtk-accdet). The machine driver only registers its snd_soc_jack here so
 * that the accdet driver can call snd_soc_jack_report().
 */
static int mt6328_component_set_jack(struct snd_soc_component *component,
				     struct snd_soc_jack *jack, void *data)
{
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(component);

	priv->jack = jack;

	return 0;
}

bool mt6328_is_headset_mic(struct snd_soc_component *component)
{
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(component);

	return priv->mic_source == 1;
}
EXPORT_SYMBOL_GPL(mt6328_is_headset_mic);

bool mt6328_is_speaker_on(struct snd_soc_component *component)
{
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(component);

	return priv->spk_amp;
}
EXPORT_SYMBOL_GPL(mt6328_is_speaker_on);

/* ------------------------------------------------------------------------ */
/* DAI                                                                       */
/* ------------------------------------------------------------------------ */

static int mt6328_dai_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct snd_soc_component *cmpnt = dai->component;
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(cmpnt);
	unsigned int rate = params_rate(params);

	dev_dbg(priv->dev, "%s(), stream %d, rate %u\n", __func__,
		substream->stream, rate);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		priv->dl_rate = rate;
	else
		priv->ul_rate = rate;

	return 0;
}

static int mt6328_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	/* The MTKAIF link format itself is programmed by the SoC AFE. */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_DSP_A:
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct snd_soc_dai_ops mt6328_dai_ops = {
	.hw_params = mt6328_dai_hw_params,
	.set_fmt = mt6328_dai_set_fmt,
};

#define MT6328_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | \
			 SNDRV_PCM_FMTBIT_U16_LE | \
			 SNDRV_PCM_FMTBIT_S24_LE | \
			 SNDRV_PCM_FMTBIT_U24_LE | \
			 SNDRV_PCM_FMTBIT_S32_LE | \
			 SNDRV_PCM_FMTBIT_U32_LE)

static struct snd_soc_dai_driver mt6328_dai_driver[] = {
	{
		.name = "mt6328-snd-codec-aif1",
		.playback = {
			.stream_name = "AIF1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_48000 |
				 SNDRV_PCM_RATE_96000 |
				 SNDRV_PCM_RATE_192000,
			.formats = MT6328_FORMATS,
		},
		.capture = {
			.stream_name = "AIF1 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000 |
				 SNDRV_PCM_RATE_16000 |
				 SNDRV_PCM_RATE_32000 |
				 SNDRV_PCM_RATE_48000 |
				 SNDRV_PCM_RATE_96000 |
				 SNDRV_PCM_RATE_192000,
			.formats = MT6328_FORMATS,
		},
		.ops = &mt6328_dai_ops,
	},
};

/* ------------------------------------------------------------------------ */
/* Component driver                                                          */
/* ------------------------------------------------------------------------ */

static int mt6328_codec_init_reg(struct snd_soc_component *cmpnt)
{
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(cmpnt);
	unsigned int gates = BIT(RG_AUDNCP_CK_PDN_SFT) |
			     BIT(RG_AUDIF_CK_PDN_SFT) |
			     BIT(RG_AUD_CK_PDN_SFT) |
			     BIT(RG_ZCD13M_CK_PDN_SFT);

	/* CLKSQ audio 26 MHz off */
	mt6328_set_bits(priv, MT6328_TOP_CLKSQ, 0x0001, 0x0000);
	/* audio clock gates off */
	mt6328_set_bits(priv, MT6328_TOP_CKPDN_CON0, gates, gates);
	/* AUDGLB powered down, decoder held in reset */
	mt6328_set_bits(priv, MT6328_AUDDEC_ANA_CON9,
			BIT(RG_AUDGLB_PWRDN_VA32_SFT) |
			BIT(RG_RSTB_DECODER_VA32_SFT),
			BIT(RG_AUDGLB_PWRDN_VA32_SFT) |
			BIT(RG_RSTB_DECODER_VA32_SFT));
	/* digital block powered down */
	mt6328_set_bits(priv, MT6328_AFE_AUDIO_TOP_CON0,
			BIT(AUD_TOP_PDN_AFE_CTL_SFT) |
			BIT(AUD_TOP_PWR_CLK_DIS_CTL_SFT),
			BIT(AUD_TOP_PDN_AFE_CTL_SFT) |
			BIT(AUD_TOP_PWR_CLK_DIS_CTL_SFT));
	/* MTKAIF clock polarity used by the MT6735 family AFE */
	mt6328_set_bits(priv, MT6328_AFE_PMIC_NEWIF_CFG2, 0x8000, 0x8000);

	return 0;
}

static int mt6328_codec_probe(struct snd_soc_component *cmpnt)
{
	struct mt6328_priv *priv = snd_soc_component_get_drvdata(cmpnt);

	priv->ana_gain[AUDIO_ANALOG_VOLUME_HSOUT] = 0x08;
	priv->ana_gain[AUDIO_ANALOG_VOLUME_HPOUTL] = 0x08;
	priv->ana_gain[AUDIO_ANALOG_VOLUME_HPOUTR] = 0x08;
	priv->ana_gain[AUDIO_ANALOG_VOLUME_MICAMP1] = 8;
	priv->ana_gain[AUDIO_ANALOG_VOLUME_MICAMP2] = 8;

	/* headset microphone is the default capture source */
	mt6328_set_bits(priv, MT6328_AUDENC_ANA_CON0,
			RG_AUDPREAMPLINPUTSEL_MASK <<
			RG_AUDPREAMPLINPUTSEL_SFT,
			0x2 << RG_AUDPREAMPLINPUTSEL_SFT);

	return mt6328_codec_init_reg(cmpnt);
}

static const struct snd_soc_component_driver mt6328_soc_component_driver = {
	.probe			= mt6328_codec_probe,
	.set_jack		= mt6328_component_set_jack,
	.controls		= mt6328_snd_controls,
	.num_controls		= ARRAY_SIZE(mt6328_snd_controls),
	.dapm_widgets		= mt6328_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(mt6328_dapm_widgets),
	.dapm_routes		= mt6328_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(mt6328_dapm_routes),
	.endianness		= 1,
};

/* ------------------------------------------------------------------------ */
/* Platform driver                                                           */
/* ------------------------------------------------------------------------ */

static int mt6328_codec_driver_probe(struct platform_device *pdev)
{
	struct mt6328_priv *priv;
	struct regmap *regmap;
	unsigned int val;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!regmap)
		return -ENODEV;

	priv->regmap = regmap;
	priv->dev = &pdev->dev;
	priv->mic_source = 0;

	dev_set_drvdata(&pdev->dev, priv);

	if (regmap_read(regmap, MT6328_TOP_CLKSQ >> 1, &val))
		return -EIO;

	return devm_snd_soc_register_component(&pdev->dev,
					       &mt6328_soc_component_driver,
					       mt6328_dai_driver,
					       ARRAY_SIZE(mt6328_dai_driver));
}

static const struct of_device_id mt6328_of_match[] = {
	{ .compatible = "mediatek,mt6328-sound", },
	{ .compatible = "mediatek,mt6353-sound", },
	{}
};
MODULE_DEVICE_TABLE(of, mt6328_of_match);

static struct platform_driver mt6328_codec_driver = {
	.driver = {
		.name = "mt6328-sound",
		.of_match_table = mt6328_of_match,
	},
	.probe = mt6328_codec_driver_probe,
};
module_platform_driver(mt6328_codec_driver);

MODULE_DESCRIPTION("MediaTek MT6328/MT6353 PMIC ALSA SoC codec driver");
MODULE_LICENSE("GPL");
