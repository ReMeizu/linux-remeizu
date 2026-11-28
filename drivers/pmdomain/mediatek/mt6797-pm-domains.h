/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __PMDOMAIN_MEDIATEK_MT6797_PM_DOMAINS_H
#define __PMDOMAIN_MEDIATEK_MT6797_PM_DOMAINS_H

#include "mtk-pm-domains.h"
#include <dt-bindings/power/mt6797-power.h>

/*
 * MT6797 (Helio X20, Meizu MX6 / m95) power domain support.
 *
 * MT6797 uses the MT6797-style SPM register layout, NOT the MT8173 one the
 * generic SPM_* defines in mtk-pm-domains.h describe (those have
 * MFG_PWR_CON at 0x214 / PWR_STATUS at 0x60c), so raw offsets are used on
 * purpose below.  The values come from the in-tree legacy scpsys data
 * (drivers/pmdomain/mediatek/mtk-scpsys.c, scp_domain_data_mt6797 and
 * SPM_PWR_STATUS_MT6797), which is itself taken from the vendor SPM
 * headers (spm_v2/mt_spm_reg_mt6797.h).
 *
 * The defined domains are the ones the current boards need - VDEC, MJC
 * and the GPU chain.  The remaining MT6797 domains (VENC, ISP, MM, AUDIO)
 * stay on the legacy "mediatek,mt6797-scpsys" provider until they are
 * described here as well; both providers live in the same SPM register
 * block at 0x10006000 and only touch their own offsets.
 *
 * GPU chain: MFG_ASYNC (bus glue) -> MFG (T880 core) -> MFG_CORE0..3 (one
 * L2 shard each).  Mainline panfrost binds a single "mfg" power domain, so
 * the DT nests the four L2 shards under MFG and MFG under MFG_ASYNC:
 * genpd walks the whole chain on every panfrost power on/off.
 */

static enum scpsys_bus_prot_block scpsys_bus_prot_blocks_mt6797[] = {
	BUS_PROT_BLOCK_INFRA
};

static const struct scpsys_domain_data scpsys_domain_data_mt6797[] = {
	[MT6797_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = BIT(7),
		.ctl_offs = 0x0300,
		.pwr_sta_offs = 0x0180,
		.pwr_sta2nd_offs = 0x0184,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
	},
	[MT6797_POWER_DOMAIN_MFG_ASYNC] = {
		.name = "mfg_async",
		.sta_mask = BIT(13),
		.ctl_offs = 0x0334,
		.pwr_sta_offs = 0x0180,
		.pwr_sta2nd_offs = 0x0184,
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(MT8173_TOP_AXI_PROT_EN_MFG_S),
		},
	},
	[MT6797_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = BIT(4),
		.ctl_offs = 0x0338,
		.pwr_sta_offs = 0x0180,
		.pwr_sta2nd_offs = 0x0184,
	},
	[MT6797_POWER_DOMAIN_MFG_CORE0] = {
		.name = "mfg_core0",
		.sta_mask = BIT(12),
		.ctl_offs = 0x0340,
		.pwr_sta_offs = 0x0180,
		.pwr_sta2nd_offs = 0x0184,
	},
	[MT6797_POWER_DOMAIN_MFG_CORE1] = {
		.name = "mfg_core1",
		.sta_mask = BIT(22),
		.ctl_offs = 0x0344,
		.pwr_sta_offs = 0x0180,
		.pwr_sta2nd_offs = 0x0184,
	},
	[MT6797_POWER_DOMAIN_MFG_CORE2] = {
		.name = "mfg_core2",
		.sta_mask = BIT(23),
		.ctl_offs = 0x0348,
		.pwr_sta_offs = 0x0180,
		.pwr_sta2nd_offs = 0x0184,
	},
	[MT6797_POWER_DOMAIN_MFG_CORE3] = {
		.name = "mfg_core3",
		.sta_mask = BIT(24),
		.ctl_offs = 0x034c,
		.pwr_sta_offs = 0x0180,
		.pwr_sta2nd_offs = 0x0184,
	},
	[MT6797_POWER_DOMAIN_MJC] = {
		.name = "mjc",
		.sta_mask = BIT(20),
		.ctl_offs = 0x0310,
		.pwr_sta_offs = 0x0180,
		.pwr_sta2nd_offs = 0x0184,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
	},
};

static const struct scpsys_soc_data mt6797_scpsys_data = {
	.domains_data = scpsys_domain_data_mt6797,
	.num_domains = ARRAY_SIZE(scpsys_domain_data_mt6797),
	.bus_prot_blocks = scpsys_bus_prot_blocks_mt6797,
	.num_bus_prot_blocks = ARRAY_SIZE(scpsys_bus_prot_blocks_mt6797),
};

#endif /* __PMDOMAIN_MEDIATEK_MT6797_PM_DOMAINS_H */
