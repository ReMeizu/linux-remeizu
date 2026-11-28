/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mt6735-reg.h  --  MediaTek MT6735/MT6737/MT6753 audio front-end registers
 *
 * Register layout, bit positions and reset values taken from the vendor
 * 4.9 BSP AFE header (sound/soc/mediatek/mt6735/AudDrv_Afe.h,
 * "AFE_BASE + <offset>"). The MT6735 AFE register file is the same IP block
 * as the one described by mt6797-reg.h upstream, so the register offsets and
 * memif/IRQ bit fields below are identical to that of MT6797. Only the
 * MT6735-specific bits that the drivers need are spelled out.
 */

#ifndef _MT6735_REG_H_
#define _MT6735_REG_H_

/* AUDIO_TOP */
#define AUDIO_TOP_CON0			0x0000
#define PDN_DAC_PREDIS_SFT		26
#define PDN_DAC_SFT			25
#define PDN_ADC_SFT			24
#define PDN_AFE_SFT			2

#define AUDIO_TOP_CON1			0x0004
#define AUDIO_TOP_CON2			0x0008
#define AUDIO_TOP_CON3			0x000c

/* Memory interfaces */
#define AFE_DAC_CON0			0x0010
#define AFE_ON_RETM_SFT			27
#define DAI_MODE_SFT			24
#define DAI_MODE_MASK			0x3
#define DL3_MODE_SFT			12
#define DL3_MODE_MASK			0xf
#define MOD_DAI_ON_SFT			7
#define AWB_ON_SFT			6
#define DL3_ON_SFT			5
#define DAI_ON_SFT			4
#define VUL_ON_SFT			3
#define DL2_ON_SFT			2
#define DL1_ON_SFT			1
#define AFE_ON_SFT			0

#define AFE_DAC_CON1			0x0014
#define MOD_DAI_MODE_SFT		30
#define MOD_DAI_MODE_MASK		0x3
#define DAI_DUP_WR_SFT			29
#define VUL_R_MONO_SFT			28
#define VUL_DATA_SFT			27
#define AWB_R_MONO_SFT			25
#define AWB_DATA_SFT			24
#define DL2_DATA_SFT			22
#define DL1_DATA_SFT			21
#define VUL_MODE_SFT			16
#define VUL_MODE_MASK			0xf
#define AWB_MODE_SFT			12
#define AWB_MODE_MASK			0xf
#define I2S_MODE_SFT			8
#define I2S_MODE_MASK			0xf
#define DL2_MODE_SFT			4
#define DL2_MODE_MASK			0xf
#define DL1_MODE_SFT			0
#define DL1_MODE_MASK			0xf

#define AFE_I2S_CON			0x0018
#define AFE_DAIBT_CON0			0x001c

/* AFE interconnections */
#define AFE_CONN0			0x0020
#define AFE_CONN1			0x0024
#define AFE_CONN2			0x0028
#define AFE_CONN3			0x002c
#define AFE_CONN4			0x0030
#define AFE_I2S_CON1			0x0034
#define AFE_I2S_CON2			0x0038
#define AFE_MRGIF_CON			0x003c
#define AFE_I2S_CON3			0x004c
#define AFE_CONN5			0x005c
#define AFE_CONN6			0x00bc
#define AFE_CONN7			0x0460
#define AFE_CONN8			0x0464
#define AFE_CONN9			0x0468
#define AFE_CONN10			0x046c

#define AFE_CONN_24BIT			0x006c

#define AFE_MEMIF_MSB			0x00cc
#define CPU_COMPACT_MODE_SFT		23
#define CPU_HD_ALIGN_SFT		22

#define AFE_MEMIF_HD_MODE		0x03f8
#define MOD_DAI_HD_SFT			18
#define DAI_HD_SFT			16
#define VUL_DATA2_HD_SFT		12
#define VUL_HD_SFT			10
#define AWB_HD_SFT			8
#define DL2_HD_SFT			4
#define DL1_HD_SFT			0

#define AFE_MEMIF_HDALIGN		0x03fc
#define MOD_DAI_NORMAL_MODE_SFT		25
#define VUL_NORMAL_MODE_SFT		21
#define AWB_NORMAL_MODE_SFT		20

/* DL1 */
#define AFE_DL1_BASE			0x0040
#define AFE_DL1_CUR			0x0044
#define AFE_DL1_END			0x0048
/* DL2 */
#define AFE_DL2_BASE			0x0050
#define AFE_DL2_CUR			0x0054
#define AFE_DL2_END			0x0058
/* AWB */
#define AFE_AWB_BASE			0x0070
#define AFE_AWB_END			0x0078
#define AFE_AWB_CUR			0x007c
/* VUL */
#define AFE_VUL_BASE			0x0080
#define AFE_VUL_END			0x0088
#define AFE_VUL_CUR			0x008c
/* DAI (modem downlink) */
#define AFE_DAI_BASE			0x0090
#define AFE_DAI_END			0x0098
#define AFE_DAI_CUR			0x009c
/* MOD_DAI, split from DAI on MT6735 */
#define AFE_MOD_DAI_BASE		0x0330
#define AFE_MOD_DAI_END			0x0338
#define AFE_MOD_DAI_CUR			0x033c
/* VUL second capture stream */
#define AFE_VUL_D2_BASE			0x0350
#define AFE_VUL_D2_END			0x0358
#define AFE_VUL_D2_CUR			0x035c
#define VUL_DATA2_MODE_SFT		20
#define VUL_DATA2_MODE_MASK		0xf
#define VUL_DATA2_R_MONO_SFT		11
#define VUL_DATA2_DATA_SFT		10
#define VUL_DATA2_ON_SFT		9

/* ADDA (PMIC voice/audio interface, "MTKAIF") */
#define AFE_ADDA_DL_SRC2_CON0		0x0108
#define DL_2_INPUT_MODE_CTL_SFT		28
#define DL_2_INPUT_MODE_CTL_MASK	0xf
#define DL_2_OUTPUT_SEL_CTL_SFT		24
#define DL_2_OUTPUT_SEL_CTL_MASK	0x3
#define DL_2_MUTE_CH1_OFF_CTL_PRE_SFT	12
#define DL_2_MUTE_CH2_OFF_CTL_PRE_SFT	11
#define DL_2_GAIN_ON_CTL_PRE_SFT	1
#define DL_2_SRC_ON_TMP_CTL_PRE_SFT	0

#define AFE_ADDA_DL_SRC2_CON1		0x010c
#define AFE_ADDA_UL_SRC_CON0		0x0114
#define UL_MODE_3P25M_CH2_CTL_SFT	22
#define UL_MODE_3P25M_CH1_CTL_SFT	21
#define UL_SRC_USE_CIC_OUT_CTL_SFT	20
#define UL_VOICE_MODE_CH1_CH2_SFT	17
#define UL_VOICE_MODE_CH1_CH2_MASK	0x7
#define UL_SRC_ON_TMP_CTL_SFT		0

#define AFE_ADDA_UL_SRC_CON1		0x0118
#define AFE_ADDA_TOP_CON0		0x0120
#define AFE_ADDA_UL_DL_CON0		0x0124
#define ADDA_AFE_ON_SFT			0

#define AFE_ADDA_SRC_DEBUG		0x012c
#define AFE_ADDA_SRC_DEBUG_MON0		0x0130
#define AFE_ADDA_NEWIF_CFG0		0x0138
#define AFE_ADDA_NEWIF_CFG2		0x0140
#define AFE_ADDA_PREDIS_CON0		0x0260
#define AFE_ADDA_PREDIS_CON1		0x0264

/* PCM (modem/Bluetooth) interface */
#define PCM_INTF_CON1			0x0530
#define PCM_INTF_CON1_SFT_FORMAT	1
#define PCM_INTF_CON1_MASK_FORMAT	(0x3 << 1)
#define PCM_INTF_CON1_SFT_MODE		3
#define PCM_INTF_CON1_MASK_MODE		(0x3 << 3)
#define PCM_INTF_CON1_SFT_SYNC_LEN	9
#define PCM_INTF_CON1_MASK_SYNC_LEN	(0x1f << 9)
#define PCM_INTF_CON1_SFT_LCH_RPT	13
#define PCM_INTF_CON1_MASK_LCH_RPT	(0x1 << 13)
#define PCM_INTF_CON1_SFT_BT_MODE	15
#define PCM_INTF_CON1_MASK_BT_MODE	(0x1 << 15)
#define PCM_INTF_CON1_SFT_BYP_ASRC	16
#define PCM_INTF_CON1_MASK_BYP_ASRC	(0x1 << 16)
#define PCM_INTF_CON1_SFT_NEG_EDGE	17
#define PCM_INTF_CON1_MASK_NEG_EDGE	(0x1 << 17)
#define PCM_INTF_CON1_SFT_SLAVE		18
#define PCM_INTF_CON1_MASK_SLAVE	(0x1 << 18)
#define PCM_INTF_CON1_SFT_BCLK_OUT_INV	19
#define PCM_INTF_CON1_MASK_BCLK_OUT_INV (0x1 << 19)
#define PCM_EN_SFT			0

/* IRQs */
#define AFE_IRQ_MCU_CON			0x03a0
#define IRQ2_MCU_MODE_SFT		8
#define IRQ1_MCU_MODE_SFT		4
#define IRQ2_MCU_ON_SFT			1
#define IRQ1_MCU_ON_SFT			0
#define AFE_IRQ_MCU_STATUS		0x03a4
#define AFE_IRQ_MCU_CLR			0x03a8
#define IRQ2_MCU_CLR_SFT		1
#define IRQ1_MCU_CLR_SFT		0
#define AFE_IRQ_MCU_CNT1		0x03ac
#define AFE_IRQ_MCU_CNT1_MASK		0x3ffff
#define AFE_IRQ_MCU_CNT2		0x03b0
#define AFE_IRQ_MCU_CNT2_MASK		0x3ffff
#define AFE_IRQ_MCU_EN			0x03b4
#define AFE_IRQ_MCU_EN_MASK_SFT		(0x7f << 0)
#define AFE_IRQ_STATUS_BITS		(0x7f << 0)

/* Highest register the driver touches */
#define AFE_MAX_REGISTER		PCM_INTF_CON1

#endif
