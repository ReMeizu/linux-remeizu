/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Meizu Linux mainline project
 *
 * SoC descriptions for the MediaTek CONSYS controller.
 *
 * The offsets are the ones used by the vendor conn_soc driver:
 *   MT6735 family : mt6735/include/mtk_wcn_consys_hw.h
 *   MT6797        : mt6797/include/mtk_wcn_consys_hw.h
 * MT6750/MT6755 share the topckgen, SPM and top-AXI layout of MT6735; their
 * CONN2AP mask register lives at the MT6735 offset as well.
 */

#include <linux/sizes.h>

#include "connsys.h"
#include "connsys_hw.h"

/*
 * MT6735 / MT6735M / MT6737M (M5c, M5s, M2 Note)
 *
 *   topckgen +0x084:1<<26   CONN clock gate clear
 *   topckgen +0x700:1<<10   AP2CONN_OSC_EN
 *   topckgen +0x320        EMI base remap, <<16, 12 bits
 *   topckgen +0x220/0x228  top AXI protection (bits 2 and 8)
 */
const struct connsys_soc_data connsys_soc_mt6735 = {
	.name		= "MT6735",
	.chip_id	= CONNSYS_CHIP_MT6735,
	.family		= CONNSYS_FW_FAMILY_MT6735,
	.topcg_off	= 0x0084,
	.topcg_bit	= BIT(26),
	.ap2conn_off	= 0x0700,
	.ap2conn_bit	= BIT(10),
	.emi_map_off	= 0x0320,
	.emi_map_shift	= 16,
	.emi_map_mask	= GENMASK(11, 0),
	.axi_prot_en_off = 0x0220,
	.axi_prot_sta_off = 0x0228,
	.axi_prot_mask	= BIT(2) | BIT(8),
	.wd_reset_bit	= BIT(9),
	.conn2ap_mask_off = 0x0000,
	.conn2ap_mask_bit = BIT(8),
	.rom_patch	= "ROMv2_lm_patch_001.bin",
	.firmware	= "WIFI_RAM_CODE_6735",
};

/*
 * MT6750 / MT6755 / MT6755M (U10, M3 Note, M3s, M6, L681, M681)
 */
const struct connsys_soc_data connsys_soc_mt6755 = {
	.name		= "MT6755",
	.chip_id	= CONNSYS_CHIP_MT6755,
	.family		= CONNSYS_FW_FAMILY_MT6755,
	.topcg_off	= 0x0084,
	.topcg_bit	= BIT(26),
	.ap2conn_off	= 0x0700,
	.ap2conn_bit	= BIT(10),
	.emi_map_off	= 0x0320,
	.emi_map_shift	= 16,
	.emi_map_mask	= GENMASK(11, 0),
	.axi_prot_en_off = 0x0220,
	.axi_prot_sta_off = 0x0228,
	.axi_prot_mask	= BIT(2) | BIT(8),
	.wd_reset_bit	= BIT(9),
	.conn2ap_mask_off = 0x0000,
	.conn2ap_mask_bit = BIT(8),
	.rom_patch	= "ROMv2_lm_patch_001.bin",
	.firmware	= "WIFI_RAM_CODE_6755",
};

/*
 * MT6797 / MT6797X (MX6)
 *
 *   topckgen +0x084:1<<26   CONN clock gate clear
 *   topckgen +0x1f00:1<<10  AP2CONN_OSC_EN
 *   topckgen +0x1340        EMI base remap, <<16, 12 bits
 *   topckgen +0x1220/0x1228 top AXI protection (bits 18 and 19)
 *   MCU    +0x1350:1<<8     CONN2AP sleep mask
 */
const struct connsys_soc_data connsys_soc_mt6797 = {
	.name		= "MT6797",
	.chip_id	= CONNSYS_CHIP_MT6797,
	.family		= CONNSYS_FW_FAMILY_MT6797,
	.topcg_off	= 0x0084,
	.topcg_bit	= BIT(26),
	.ap2conn_off	= 0x1f00,
	.ap2conn_bit	= BIT(10),
	.emi_map_off	= 0x1340,
	.emi_map_shift	= 16,
	.emi_map_mask	= GENMASK(11, 0),
	.axi_prot_en_off = 0x1220,
	.axi_prot_sta_off = 0x1228,
	.axi_prot_mask	= BIT(18) | BIT(19),
	.wd_reset_bit	= BIT(9),
	.conn2ap_mask_off = 0x1350,
	.conn2ap_mask_bit = BIT(8),
	.rom_patch	= "ROMv3_patch_0000_0001.bin",
	.firmware	= "WIFI_RAM_CODE_6797",
};

const struct connsys_soc_data *connsys_soc_by_chip_id(u32 chip_id)
{
	switch (chip_id) {
	case CONNSYS_CHIP_MT6735:
	case CONNSYS_CHIP_MT6735M:
	case CONNSYS_CHIP_MT6735S:
		return &connsys_soc_mt6735;
	case CONNSYS_CHIP_MT6755:
	case CONNSYS_CHIP_MT6755M:
	case CONNSYS_CHIP_MT6750:
		return &connsys_soc_mt6755;
	case CONNSYS_CHIP_MT6797:
		return &connsys_soc_mt6797;
	default:
		return NULL;
	}
}
