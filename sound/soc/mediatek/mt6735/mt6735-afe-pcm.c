// SPDX-License-Identifier: GPL-2.0-only
/*
 * mt6735-afe-pcm.c  --  MediaTek MT6735/MT6737/MT6753 ALSA SoC AFE driver
 *
 * The MT6735 family audio front-end is the same IP block used by MT6797:
 * seven memory interfaces (DL1, DL2, AWB, VUL, VUL_D2, DAI, MOD_DAI) feeding
 * the MTKAIF serial link to the PMIC audio codec (MT6328 on MT6735/MT6737M,
 * MT6353 on MT6753) through the ADDA block, plus a two-port PCM interface
 * for the modem/Bluetooth. Memory interface and DMA register layout, the
 * sampling-rate encodings and the two AFE IRQ sources are identical to the
 * upstream MT6797 driver, which this file reuses through the generic
 * MediaTek AFE core (sound/soc/mediatek/common).
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mt6735-afe-common.h"
#include "mt6735-afe-clk.h"
#include "mt6735-interconnection.h"
#include "mt6735-reg.h"
#include "../common/mtk-afe-fe-dai.h"
#include "../common/mtk-afe-platform-driver.h"

enum {
	MTK_AFE_RATE_8K = 0,
	MTK_AFE_RATE_11K = 1,
	MTK_AFE_RATE_12K = 2,
	MTK_AFE_RATE_384K = 3,
	MTK_AFE_RATE_16K = 4,
	MTK_AFE_RATE_22K = 5,
	MTK_AFE_RATE_24K = 6,
	MTK_AFE_RATE_130K = 7,
	MTK_AFE_RATE_32K = 8,
	MTK_AFE_RATE_44K = 9,
	MTK_AFE_RATE_48K = 10,
	MTK_AFE_RATE_88K = 11,
	MTK_AFE_RATE_96K = 12,
	MTK_AFE_RATE_174K = 13,
	MTK_AFE_RATE_192K = 14,
	MTK_AFE_RATE_260K = 15,
};

enum {
	MTK_AFE_DAI_MEMIF_RATE_8K = 0,
	MTK_AFE_DAI_MEMIF_RATE_16K = 1,
	MTK_AFE_DAI_MEMIF_RATE_32K = 2,
};

unsigned int mt6735_general_rate_transform(struct device *dev,
					   unsigned int rate)
{
	switch (rate) {
	case 8000:
		return MTK_AFE_RATE_8K;
	case 11025:
		return MTK_AFE_RATE_11K;
	case 12000:
		return MTK_AFE_RATE_12K;
	case 16000:
		return MTK_AFE_RATE_16K;
	case 22050:
		return MTK_AFE_RATE_22K;
	case 24000:
		return MTK_AFE_RATE_24K;
	case 32000:
		return MTK_AFE_RATE_32K;
	case 44100:
		return MTK_AFE_RATE_44K;
	case 48000:
		return MTK_AFE_RATE_48K;
	case 88200:
		return MTK_AFE_RATE_88K;
	case 96000:
		return MTK_AFE_RATE_96K;
	case 130000:
		return MTK_AFE_RATE_130K;
	case 176400:
		return MTK_AFE_RATE_174K;
	case 192000:
		return MTK_AFE_RATE_192K;
	case 260000:
		return MTK_AFE_RATE_260K;
	default:
		dev_warn(dev, "%s(), rate %u invalid, use 48k\n",
			 __func__, rate);
		return MTK_AFE_RATE_48K;
	}
}

static unsigned int dai_memif_rate_transform(struct device *dev,
					     unsigned int rate)
{
	switch (rate) {
	case 8000:
		return MTK_AFE_DAI_MEMIF_RATE_8K;
	case 16000:
		return MTK_AFE_DAI_MEMIF_RATE_16K;
	case 32000:
		return MTK_AFE_DAI_MEMIF_RATE_32K;
	default:
		dev_warn(dev, "%s(), rate %u invalid, use 16k\n",
			 __func__, rate);
		return MTK_AFE_DAI_MEMIF_RATE_16K;
	}
}

unsigned int mt6735_rate_transform(struct device *dev,
				   unsigned int rate, int aud_blk)
{
	switch (aud_blk) {
	case MT6735_MEMIF_DAI:
	case MT6735_MEMIF_MOD_DAI:
		return dai_memif_rate_transform(dev, rate);
	default:
		return mt6735_general_rate_transform(dev, rate);
	}
}

static const struct snd_pcm_hardware mt6735_afe_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE |
		   SNDRV_PCM_FMTBIT_S24_LE |
		   SNDRV_PCM_FMTBIT_S32_LE,
	.period_bytes_min = 256,
	.period_bytes_max = 4 * 48 * 1024,
	.periods_min = 2,
	.periods_max = 256,
	.buffer_bytes_max = 8 * 48 * 1024,
	.fifo_size = 0,
};

static int mt6735_memif_fs(struct snd_pcm_substream *substream,
			   unsigned int rate)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component =
		snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);
	int id = snd_soc_rtd_to_cpu(rtd, 0)->id;

	return mt6735_rate_transform(afe->dev, rate, id);
}

static int mt6735_irq_fs(struct snd_pcm_substream *substream,
			 unsigned int rate)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component =
		snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);

	return mt6735_general_rate_transform(afe->dev, rate);
}

#define MTK_PCM_RATES	(SNDRV_PCM_RATE_8000_48000 |\
			 SNDRV_PCM_RATE_88200 |\
			 SNDRV_PCM_RATE_96000 |\
			 SNDRV_PCM_RATE_176400 |\
			 SNDRV_PCM_RATE_192000)

#define MTK_PCM_DAI_RATES (SNDRV_PCM_RATE_8000 |\
			   SNDRV_PCM_RATE_16000 |\
			   SNDRV_PCM_RATE_32000)

#define MTK_PCM_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE |\
			 SNDRV_PCM_FMTBIT_S24_LE |\
			 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver mt6735_memif_dai_driver[] = {
	/* FE DAIs: memory interfaces to the CPU */
	{
		.name = "DL1",
		.id = MT6735_MEMIF_DL1,
		.playback = {
			.stream_name = "DL1",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "DL2",
		.id = MT6735_MEMIF_DL2,
		.playback = {
			.stream_name = "DL2",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "UL1",
		.id = MT6735_MEMIF_VUL,
		.capture = {
			.stream_name = "UL1",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "UL2",
		.id = MT6735_MEMIF_VUL2,
		.capture = {
			.stream_name = "UL2",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "UL3",
		.id = MT6735_MEMIF_AWB,
		.capture = {
			.stream_name = "UL3",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "UL_MONO_1",
		.id = MT6735_MEMIF_MOD_DAI,
		.capture = {
			.stream_name = "UL_MONO_1",
			.channels_min = 1,
			.channels_max = 1,
			.rates = MTK_PCM_DAI_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "UL_MONO_2",
		.id = MT6735_MEMIF_DAI,
		.capture = {
			.stream_name = "UL_MONO_2",
			.channels_min = 1,
			.channels_max = 1,
			.rates = MTK_PCM_DAI_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
};

/* DMA widgets and routes */
static const struct snd_kcontrol_new memif_ul1_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN7,
				    I_ADDA_UL_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul1_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN8,
				    I_ADDA_UL_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul2_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN9,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN9, I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN9, I_DL2_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul2_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN10,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN10, I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN10, I_DL2_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul3_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN5,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN5, I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN5, I_DL2_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul3_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN6,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN6, I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN6, I_DL2_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul_mono_1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN1,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN1,
				    I_ADDA_UL_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul_mono_2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN2,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN2,
				    I_ADDA_UL_CH2, 1, 0),
};

static const struct snd_soc_dapm_widget mt6735_memif_widgets[] = {
	/* memif */
	SND_SOC_DAPM_MIXER("UL1_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul1_ch1_mix,
			   ARRAY_SIZE(memif_ul1_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL1_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul1_ch2_mix,
			   ARRAY_SIZE(memif_ul1_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL2_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul2_ch1_mix,
			   ARRAY_SIZE(memif_ul2_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL2_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul2_ch2_mix,
			   ARRAY_SIZE(memif_ul2_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL3_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul3_ch1_mix,
			   ARRAY_SIZE(memif_ul3_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL3_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul3_ch2_mix,
			   ARRAY_SIZE(memif_ul3_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL_MONO_1_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul_mono_1_mix,
			   ARRAY_SIZE(memif_ul_mono_1_mix)),

	SND_SOC_DAPM_MIXER("UL_MONO_2_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul_mono_2_mix,
			   ARRAY_SIZE(memif_ul_mono_2_mix)),
};

static const struct snd_soc_dapm_route mt6735_memif_routes[] = {
	/* capture */
	{"UL1", NULL, "UL1_CH1"},
	{"UL1", NULL, "UL1_CH2"},
	{"UL1_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"UL1_CH2", "ADDA_UL_CH2", "ADDA Capture"},

	{"UL2", NULL, "UL2_CH1"},
	{"UL2", NULL, "UL2_CH2"},
	{"UL2_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"UL2_CH2", "ADDA_UL_CH2", "ADDA Capture"},

	{"UL3", NULL, "UL3_CH1"},
	{"UL3", NULL, "UL3_CH2"},
	{"UL3_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"UL3_CH2", "ADDA_UL_CH2", "ADDA Capture"},

	{"UL_MONO_1", NULL, "UL_MONO_1_CH1"},
	{"UL_MONO_1_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"UL_MONO_1_CH1", "ADDA_UL_CH2", "ADDA Capture"},

	{"UL_MONO_2", NULL, "UL_MONO_2_CH1"},
	{"UL_MONO_2_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"UL_MONO_2_CH1", "ADDA_UL_CH2", "ADDA Capture"},
};

static const struct snd_soc_component_driver mt6735_afe_pcm_dai_component = {
	.name = "mt6735-afe-pcm-dai",
};

static const struct mtk_base_memif_data memif_data[MT6735_MEMIF_NUM] = {
	[MT6735_MEMIF_DL1] = {
		.name = "DL1",
		.id = MT6735_MEMIF_DL1,
		.reg_ofs_base = AFE_DL1_BASE,
		.reg_ofs_cur = AFE_DL1_CUR,
		.fs_reg = AFE_DAC_CON1,
		.fs_shift = DL1_MODE_SFT,
		.fs_maskbit = DL1_MODE_MASK,
		.mono_reg = AFE_DAC_CON1,
		.mono_shift = DL1_DATA_SFT,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = DL1_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = DL1_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
	[MT6735_MEMIF_DL2] = {
		.name = "DL2",
		.id = MT6735_MEMIF_DL2,
		.reg_ofs_base = AFE_DL2_BASE,
		.reg_ofs_cur = AFE_DL2_CUR,
		.fs_reg = AFE_DAC_CON1,
		.fs_shift = DL2_MODE_SFT,
		.fs_maskbit = DL2_MODE_MASK,
		.mono_reg = AFE_DAC_CON1,
		.mono_shift = DL2_DATA_SFT,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = DL2_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = DL2_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
	[MT6735_MEMIF_AWB] = {
		.name = "AWB",
		.id = MT6735_MEMIF_AWB,
		.reg_ofs_base = AFE_AWB_BASE,
		.reg_ofs_cur = AFE_AWB_CUR,
		.fs_reg = AFE_DAC_CON1,
		.fs_shift = AWB_MODE_SFT,
		.fs_maskbit = AWB_MODE_MASK,
		.mono_reg = AFE_DAC_CON1,
		.mono_shift = AWB_DATA_SFT,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = AWB_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = AWB_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
	[MT6735_MEMIF_VUL] = {
		.name = "VUL",
		.id = MT6735_MEMIF_VUL,
		.reg_ofs_base = AFE_VUL_BASE,
		.reg_ofs_cur = AFE_VUL_CUR,
		.fs_reg = AFE_DAC_CON1,
		.fs_shift = VUL_MODE_SFT,
		.fs_maskbit = VUL_MODE_MASK,
		.mono_reg = AFE_DAC_CON1,
		.mono_shift = VUL_R_MONO_SFT,
		.mono_invert = 1,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = VUL_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = VUL_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
	[MT6735_MEMIF_VUL2] = {
		.name = "VUL2",
		.id = MT6735_MEMIF_VUL2,
		.reg_ofs_base = AFE_VUL_D2_BASE,
		.reg_ofs_cur = AFE_VUL_D2_CUR,
		.fs_reg = AFE_DAC_CON0,
		.fs_shift = VUL_DATA2_MODE_SFT,
		.fs_maskbit = VUL_DATA2_MODE_MASK,
		.mono_reg = AFE_DAC_CON0,
		.mono_shift = VUL_DATA2_R_MONO_SFT,
		.mono_invert = 1,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = VUL_DATA2_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = VUL_DATA2_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
	[MT6735_MEMIF_DAI] = {
		.name = "DAI",
		.id = MT6735_MEMIF_DAI,
		.reg_ofs_base = AFE_DAI_BASE,
		.reg_ofs_cur = AFE_DAI_CUR,
		.fs_reg = AFE_DAC_CON0,
		.fs_shift = DAI_MODE_SFT,
		.fs_maskbit = DAI_MODE_MASK,
		.mono_reg = -1,
		.mono_shift = 0,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = DAI_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = DAI_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
	[MT6735_MEMIF_MOD_DAI] = {
		.name = "MOD_DAI",
		.id = MT6735_MEMIF_MOD_DAI,
		.reg_ofs_base = AFE_MOD_DAI_BASE,
		.reg_ofs_cur = AFE_MOD_DAI_CUR,
		.fs_reg = AFE_DAC_CON1,
		.fs_shift = MOD_DAI_MODE_SFT,
		.fs_maskbit = MOD_DAI_MODE_MASK,
		.mono_reg = -1,
		.mono_shift = 0,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = MOD_DAI_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = MOD_DAI_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
};

static const struct mtk_base_irq_data irq_data[MT6735_IRQ_NUM] = {
	[MT6735_IRQ_1] = {
		.id = MT6735_IRQ_1,
		.irq_cnt_reg = AFE_IRQ_MCU_CNT1,
		.irq_cnt_shift = 0,
		.irq_cnt_maskbit = AFE_IRQ_MCU_CNT1_MASK,
		.irq_fs_reg = AFE_IRQ_MCU_CON,
		.irq_fs_shift = IRQ1_MCU_MODE_SFT,
		.irq_fs_maskbit = 0xf,
		.irq_en_reg = AFE_IRQ_MCU_CON,
		.irq_en_shift = IRQ1_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ_MCU_CLR,
		.irq_clr_shift = IRQ1_MCU_CLR_SFT,
	},
	[MT6735_IRQ_2] = {
		.id = MT6735_IRQ_2,
		.irq_cnt_reg = AFE_IRQ_MCU_CNT2,
		.irq_cnt_shift = 0,
		.irq_cnt_maskbit = AFE_IRQ_MCU_CNT2_MASK,
		.irq_fs_reg = AFE_IRQ_MCU_CON,
		.irq_fs_shift = IRQ2_MCU_MODE_SFT,
		.irq_fs_maskbit = 0xf,
		.irq_en_reg = AFE_IRQ_MCU_CON,
		.irq_en_shift = IRQ2_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ_MCU_CLR,
		.irq_clr_shift = IRQ2_MCU_CLR_SFT,
	},
};

static const struct regmap_config mt6735_afe_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = AFE_MAX_REGISTER,
};

static irqreturn_t mt6735_afe_irq_handler(int irq, void *dev_id)
{
	struct mtk_base_afe *afe = dev_id;
	unsigned int status, mcu_en;
	irqreturn_t irq_ret = IRQ_HANDLED;
	int ret, i;

	ret = regmap_read(afe->regmap, AFE_IRQ_MCU_EN, &mcu_en);
	if (ret)
		return IRQ_NONE;

	ret = regmap_read(afe->regmap, AFE_IRQ_MCU_STATUS, &status);
	if (ret || (status & mcu_en & AFE_IRQ_STATUS_BITS) == 0) {
		dev_err_ratelimited(afe->dev,
				    "irq status err, ret %d, status 0x%x, mcu_en 0x%x\n",
				    ret, status, mcu_en);
		status = mcu_en & AFE_IRQ_STATUS_BITS;
		irq_ret = IRQ_NONE;
	} else {
		for (i = 0; i < MT6735_MEMIF_NUM; i++) {
			struct mtk_base_afe_memif *memif = &afe->memif[i];
			struct mtk_base_afe_irq *irq_p;

			if (!memif->substream || memif->irq_usage < 0)
				continue;

			irq_p = &afe->irqs[memif->irq_usage];
			if (status & BIT(irq_p->irq_data->irq_en_shift))
				snd_pcm_period_elapsed(memif->substream);
		}
	}

	/* clear the IRQ sources that are routed to the MCU */
	regmap_write(afe->regmap, AFE_IRQ_MCU_CLR,
		     (status & AFE_IRQ_STATUS_BITS));

	return irq_ret;
}

static int mt6735_afe_runtime_suspend(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);
	unsigned int afe_on;
	int retry = 0;

	/* disable AFE */
	regmap_update_bits(afe->regmap, AFE_DAC_CON0,
			   BIT(AFE_ON_SFT), 0);
	do {
		regmap_read(afe->regmap, AFE_DAC_CON0, &afe_on);
		if (!(afe_on & BIT(AFE_ON_RETM_SFT)))
			break;
		udelay(10);
	} while (++retry < 100000);

	if (retry)
		dev_warn(afe->dev, "%s(), AFE power down retries: %d\n",
			 __func__, retry);

	/* make sure all irq status bits are cleared */
	regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, AFE_IRQ_STATUS_BITS);

	return mt6735_afe_disable_clock(afe);
}

static int mt6735_afe_runtime_resume(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);
	int ret;

	ret = mt6735_afe_enable_clock(afe);
	if (ret)
		return ret;

	/* route all AFE interrupts to the MCU */
	regmap_write(afe->regmap, AFE_IRQ_MCU_EN,
		     AFE_IRQ_MCU_EN_MASK_SFT);

	/* every memory interface uses normal (8_24) mode */
	regmap_update_bits(afe->regmap, AFE_MEMIF_HDALIGN, 0x3f << 20,
			   0x3f << 20);
	regmap_update_bits(afe->regmap, AFE_MEMIF_MSB,
			   BIT(CPU_COMPACT_MODE_SFT), 0);
	regmap_update_bits(afe->regmap, AFE_MEMIF_MSB,
			   BIT(CPU_HD_ALIGN_SFT), 0);

	/* set all output ports to 24 bit */
	regmap_write(afe->regmap, AFE_CONN_24BIT, GENMASK(29, 0));

	/* enable AFE */
	regmap_update_bits(afe->regmap, AFE_DAC_CON0,
			   BIT(AFE_ON_SFT), BIT(AFE_ON_SFT));

	return 0;
}

static int mt6735_dai_memif_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	list_add(&dai->list, &afe->sub_dais);

	dai->dai_drivers = mt6735_memif_dai_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mt6735_memif_dai_driver);

	dai->dapm_widgets = mt6735_memif_widgets;
	dai->num_dapm_widgets = ARRAY_SIZE(mt6735_memif_widgets);
	dai->dapm_routes = mt6735_memif_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mt6735_memif_routes);

	return 0;
}

typedef int (*mt6735_dai_register_cb)(struct mtk_base_afe *);

static const mt6735_dai_register_cb mt6735_dai_register_cbs[] = {
	mt6735_dai_adda_register,
	mt6735_dai_pcm_register,
	mt6735_dai_hostless_register,
	mt6735_dai_memif_register,
};

static int mt6735_afe_pcm_dev_probe(struct platform_device *pdev)
{
	struct mtk_base_afe *afe;
	struct mt6735_afe_private *afe_priv;
	struct device *dev = &pdev->dev;
	int i, irq_id, ret;

	afe = devm_kzalloc(dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;

	afe_priv = devm_kzalloc(dev, sizeof(*afe_priv), GFP_KERNEL);
	if (!afe_priv)
		return -ENOMEM;

	afe->platform_priv = afe_priv;
	afe->dev = dev;

	/* initialise the audio related clocks */
	ret = mt6735_init_clock(afe);
	if (ret) {
		dev_err(dev, "failed to get audio clocks\n");
		return ret;
	}

	/* regmap init */
	afe->base_addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(afe->base_addr))
		return PTR_ERR(afe->base_addr);

	afe->regmap = devm_regmap_init_mmio(dev, afe->base_addr,
					    &mt6735_afe_regmap_config);
	if (IS_ERR(afe->regmap))
		return PTR_ERR(afe->regmap);

	/* init memif */
	afe->memif_size = MT6735_MEMIF_NUM;
	afe->memif = devm_kcalloc(dev, afe->memif_size, sizeof(*afe->memif),
				  GFP_KERNEL);
	if (!afe->memif)
		return -ENOMEM;

	for (i = 0; i < afe->memif_size; i++) {
		afe->memif[i].data = &memif_data[i];
		afe->memif[i].irq_usage = -1;
	}

	mutex_init(&afe->irq_alloc_lock);

	/* irq initialize */
	afe->irqs_size = MT6735_IRQ_NUM;
	afe->irqs = devm_kcalloc(dev, afe->irqs_size, sizeof(*afe->irqs),
				 GFP_KERNEL);
	if (!afe->irqs)
		return -ENOMEM;

	for (i = 0; i < afe->irqs_size; i++)
		afe->irqs[i].irq_data = &irq_data[i];

	irq_id = platform_get_irq(pdev, 0);
	if (irq_id < 0)
		return irq_id;

	ret = devm_request_irq(dev, irq_id, mt6735_afe_irq_handler,
			       IRQF_TRIGGER_NONE, "mt6735-afe", afe);
	if (ret) {
		dev_err(dev, "could not request AFE irq %d: %d\n", irq_id, ret);
		return ret;
	}

	/* init sub_dais */
	INIT_LIST_HEAD(&afe->sub_dais);

	for (i = 0; i < ARRAY_SIZE(mt6735_dai_register_cbs); i++) {
		ret = mt6735_dai_register_cbs[i](afe);
		if (ret) {
			dev_err(dev, "DAI registration %d failed: %d\n", i, ret);
			return ret;
		}
	}

	ret = mtk_afe_combine_sub_dai(afe);
	if (ret) {
		dev_err(dev, "mtk_afe_combine_sub_dai failed: %d\n", ret);
		return ret;
	}

	afe->mtk_afe_hardware = &mt6735_afe_hardware;
	afe->memif_fs = mt6735_memif_fs;
	afe->irq_fs = mt6735_irq_fs;
	afe->runtime_resume = mt6735_afe_runtime_resume;
	afe->runtime_suspend = mt6735_afe_runtime_suspend;

	platform_set_drvdata(pdev, afe);

	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		goto err_pm_disable;

	/* register the PCM component first, then the DAI component */
	ret = devm_snd_soc_register_component(dev, &mtk_afe_pcm_platform,
					      NULL, 0);
	if (ret) {
		dev_err(dev, "failed to register PCM component: %d\n", ret);
		goto err_pm_put;
	}

	ret = devm_snd_soc_register_component(dev,
					      &mt6735_afe_pcm_dai_component,
					      afe->dai_drivers,
					      afe->num_dai_drivers);
	if (ret) {
		dev_err(dev, "failed to register DAI component: %d\n", ret);
		goto err_pm_put;
	}

	return 0;

err_pm_put:
	pm_runtime_put_sync(dev);
err_pm_disable:
	pm_runtime_disable(dev);

	return ret;
}

static void mt6735_afe_pcm_dev_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		mt6735_afe_runtime_suspend(dev);
	pm_runtime_put_sync(dev);
}

static const struct of_device_id mt6735_afe_pcm_dt_match[] = {
	{ .compatible = "mediatek,mt6735-audio", },
	{ .compatible = "mediatek,mt6753-audio", },
	{},
};
MODULE_DEVICE_TABLE(of, mt6735_afe_pcm_dt_match);

static const struct dev_pm_ops mt6735_afe_pm_ops = {
	RUNTIME_PM_OPS(mt6735_afe_runtime_suspend,
		       mt6735_afe_runtime_resume, NULL)
};

static struct platform_driver mt6735_afe_pcm_driver = {
	.driver = {
		.name = "mt6735-audio",
		.of_match_table = mt6735_afe_pcm_dt_match,
		.pm = pm_ptr(&mt6735_afe_pm_ops),
	},
	.probe = mt6735_afe_pcm_dev_probe,
	.remove = mt6735_afe_pcm_dev_remove,
};

module_platform_driver(mt6735_afe_pcm_driver);

MODULE_DESCRIPTION("MediaTek MT6735/MT6737/MT6753 ALSA SoC AFE platform driver");
MODULE_LICENSE("GPL");
