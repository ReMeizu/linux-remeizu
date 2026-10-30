/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek MT6353 PMIC register offsets (storage-only subset).
 *
 * All values are taken from the MediaTek 3.18 BSP:
 *   drivers/misc/mediatek/include/mt-plat/mt6755/include/mach/mt6353_hw.h
 * (the MT6353 is the PMIC of the MT6750/MT6755 Meizu boards). The register
 * base in that header is 0x0000 (mt6353_hw.h:18), so the offsets below are
 * directly usable as pwrap/regmap register numbers, exactly like the
 * MT6351/MT6358 mainline bindings.
 */
#ifndef __MFD_MT6353_REGISTERS_H__
#define __MFD_MT6353_REGISTERS_H__

/* Chip ID, mt6353_hw.h:77-78 */
#define MT6353_HWCID		0x0200
#define MT6353_SWCID		0x0202

/*
 * LDO control registers. Bits used here:
 *   bit 1 = LDO enable (all three rails), mt6353_hw.h:5883-5884,
 *           5937-5938 and 6020-6021.
 */
#define MT6353_LDO_VMC_CON0	0x0a7a	/* mt6353_hw.h:440 */
#define MT6353_LDO_VMCH_CON0	0x0a80	/* mt6353_hw.h:443 */
#define MT6353_LDO_VEMC33_CON0	0x0a8a	/* mt6353_hw.h:448 */

/*
 * Voltage-select registers. The VOSEL fields live in the DLDO analog
 * block, not in the LDO control register:
 *   VMC     DLDO_ANA_CON14, bits [10:8], mt6353_hw.h:476, 6311-6313
 *   VMCH    DLDO_ANA_CON6,  bits [9:8],  mt6353_hw.h:468, 6230-6232
 *   VEMC33  DLDO_ANA_CON8,  bits [9:8],  mt6353_hw.h:470, 6254-6256
 */
#define MT6353_VMC_ANA_CON0	0x0ac2
#define MT6353_VMCH_ANA_CON0	0x0ab2
#define MT6353_VEMC33_ANA_CON0	0x0ab6

#endif /* __MFD_MT6353_REGISTERS_H__ */
