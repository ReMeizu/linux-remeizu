/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __PMDOMAIN_MEDIATEK_MT6755_PM_DOMAINS_H
#define __PMDOMAIN_MEDIATEK_MT6755_PM_DOMAINS_H

#include "mtk-pm-domains.h"
#include <dt-bindings/power/mediatek,mt6755-power.h>

/*
 * MT6755 power domain support — DISP (DIS) MTCMOS only for now.
 *
 * VERIFIED-FROM-STOCK (kernel-m681-m6base-3.18.140/kernel-3.18/
 * drivers/clk/mediatek/clk-mt6755-pg.c):
 *   DIS_PWR_CON      = SPM + 0x030c            (line 140)
 *   PWR_STATUS       = SPM + 0x0180            (line 134)
 *   PWR_STATUS_2ND   = SPM + 0x0184            (line 135)
 *   DIS_PWR_STA_MASK = BIT(3)                  (line 180)
 *   DIS_SRAM_PDN     = BIT(8)                  (line 216)
 *   DIS_SRAM_PDN_ACK = BIT(12)                 (line 217)
 *   DIS_PROT_MASK    = BIT(1)                  (line 203; comment: "bit 1,
 *      6, 16; if bit6 set, MMSYS PDN, access reg will hang")
 *   INFRA_TOPAXI_PROTECTEN  = infracfg + 0x220 (line 155)
 *   INFRA_TOPAXI_PROTECTSTA1= infracfg + 0x228 (line 156)
 *   full on/off sequence: lines 755-845 — identical ordering to mainline
 *   scpsys_power_on/off (PWR_ON -> PWR_ON_2ND -> ack -> CLK_DIS/ISO/RST ->
 *   SRAM up -> release bus protection).
 *
 * NOTE: the generic mtk-pm-domains SPM_* offset defines (SPM_DIS_PWR_CON
 * 0x23c, SPM_PWR_STATUS 0x60c) are the MT8173/MT6795 layout and are WRONG
 * for MT6755 — MT6755 uses the MT6797-style layout, so raw offsets are used
 * here on purpose.
 *
 * NOTE: the stock sequence starts by unlocking SPM register control
 * (POWERON_CONFIG_EN = (0xb16<<16)|1, clk-mt6755-pg.c:758).  Mainline
 * mtk-pm-domains does not do this; on m681 the unlock is already performed
 * by the preloader/LK and (again) by arch/arm64/kernel/mt6755_smp.c
 * (SPM_POWERON_CONFIG_EN write, lines 197/270), so it holds by the time
 * scpsys probes.  If DIS_PWR_CON writes ever read back unchanged, this is
 * the first thing to check.
 */
static enum scpsys_bus_prot_block scpsys_bus_prot_blocks_mt6755[] = {
	BUS_PROT_BLOCK_INFRA
};

static const struct scpsys_domain_data scpsys_domain_data_mt6755[] = {
	[MT6755_POWER_DOMAIN_DISP] = {
		.name = "disp",
		.sta_mask = BIT(3),
		.ctl_offs = 0x030c,
		.pwr_sta_offs = 0x0180,
		.pwr_sta2nd_offs = 0x0184,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(BIT(1)),
		},
	},
	/*
	 * GPU (Mali-T860MP2) MTCMOS pair, VERIFIED-FROM-STOCK — from
	 * clk-mt6755-pg.c, the SAME file the working DIS recipe came from
	 * (the mt_spm.h 0x214/0x2c4 values are a different vintage — 0x2c4 is
	 * even tagged "mt6797" — and pointing scpsys at them WEDGED the r48
	 * boot at the mfgcfg probe):
	 *   MFG_ASYNC_PWR_CON = SPM + 0x0334   (pg.c:149, "/ * correct * /")
	 *   MFG_PWR_CON       = SPM + 0x0338   (pg.c:141)
	 *   MFG_ASYNC PWR_STA bit 23, no SRAM PDN        (pg.c:186)
	 *   MFG PWR_STA bit 4, SRAM_PDN bit 8 / ACK 16   (pg.c:181,218,219)
	 *   MFG_PROT_MASK = TOPAXI bit 21                (pg.c:204)
	 * On/off ordering (pg.c spm_mtcmos_ctrl_mfg_async/mfg2) matches the
	 * generic mainline sequence. MFG_ASYNC is the parent (bus glue), MFG
	 * the child (GPU core) — nested power-domain@ nodes in the DT.
	 */
	[MT6755_POWER_DOMAIN_MFG_ASYNC] = {
		.name = "mfg_async",
		.sta_mask = BIT(23),
		.ctl_offs = 0x0334,
		.pwr_sta_offs = 0x0180,
		.pwr_sta2nd_offs = 0x0184,
	},
	[MT6755_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = BIT(4),
		.ctl_offs = 0x0338,
		.pwr_sta_offs = 0x0180,
		.pwr_sta2nd_offs = 0x0184,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(16, 16),
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(BIT(21)),
		},
	},
};

static const struct scpsys_soc_data mt6755_scpsys_data = {
	.domains_data = scpsys_domain_data_mt6755,
	.num_domains = ARRAY_SIZE(scpsys_domain_data_mt6755),
	.bus_prot_blocks = scpsys_bus_prot_blocks_mt6755,
	.num_bus_prot_blocks = ARRAY_SIZE(scpsys_bus_prot_blocks_mt6755),
};

#endif /* __PMDOMAIN_MEDIATEK_MT6755_PM_DOMAINS_H */
