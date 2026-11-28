/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mt6735-interconnection.h  --  MT6735/MT6737/MT6753 AFE input indices
 *
 * Bit positions inside the AFE_CONNn registers, as used by the vendor 4.9
 * BSP (mt_soc_afe_connection.c) for the MT6735 family.
 */

#ifndef _MT6735_INTERCONNECTION_H_
#define _MT6735_INTERCONNECTION_H_

#define I_I2S0_CH1		0
#define I_I2S0_CH2		1
#define I_ADDA_UL_CH1		3
#define I_ADDA_UL_CH2		4
#define I_DL1_CH1		5
#define I_DL1_CH2		6
#define I_DL2_CH1		7
#define I_DL2_CH2		8
#define I_PCM_1_CAP_CH1		9
#define I_I2S2_CH1		10
#define I_I2S2_CH2		11
#define I_PCM_2_CAP_CH1		14
#define I_PCM_2_CAP_CH2		21
#define I_PCM_1_CAP_CH2		22
#define I_MOD_DAI_CH1		25

#endif
