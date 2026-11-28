// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MD1 cellular modem - per SoC description.
 *
 * The three SoC families in scope share the same CLDMA/CCIF IP. What differs
 * is the set of queues the AP owns, the clock and reset wiring, and how the
 * MD core is released from reset:
 *
 *   MT6797 (X20)      six AP queues, SCP MD1 domain powered by the AP and
 *                     explicit CLDMA reset bits in the INFRA registers.
 *   MT6755/MT6750     six AP queues, MD1 domain clock named "scp-sys-md1-main".
 *   MT6735/MT6737M    two AP queues (CLDMA capability 2), the MD core is
 *                     released through the MD boot vector registers only.
 */
#include <linux/kernel.h>
#include <linux/stddef.h>

#include "ccci_priv.h"


const struct ccci_platform_data ccci_mt6735_data = {
	.name = "MT6735",
	.cldma = {
		.name = "mt6735-cldma",
		.queue_mask = 0x3f,
		.net_queue_mask = 0xc0,
		.capability = MODEM_CAP_NAPI | MODEM_CAP_TXBUSY_STOP,
	},
	.slave_boot = true,
	/*
	 * MD boot vector registers, offsets inside the CCIF AP window. The
	 * key must be written before the vector and the enable last, the ROM
	 * samples all three when it leaves reset (see ccci_md_let_go()).
	 */
	.boot_vector_key = 0x1bc0,
	.boot_vector = 0x1bc4,
	.boot_vector_en = 0x1bc8,
	.boot_vector_key_value = 0x3567c766,
	.boot_vector_value = 0x00000000,
	.boot_vector_en_value = 0xa3b66175,
	/* INFRA reset bits: CLDMA AO[4], CLDMA PD[13] (vendor cldma_reg.h) */
	.cldma_ao_rst_mask = BIT(4),
	.cldma_pd_rst_mask = BIT(13),
};

const struct ccci_platform_data ccci_mt6755_data = {
	.name = "MT6755",
	.cldma = {
		.name = "mt6755-cldma",
		.queue_mask = 0x3f,
		.net_queue_mask = 0xc0,
		.capability = MODEM_CAP_NAPI | MODEM_CAP_TXBUSY_STOP,
	},
	.slave_boot = true,
	.boot_vector_key = 0x1bc0,
	.boot_vector = 0x1bc4,
	.boot_vector_en = 0x1bc8,
	.boot_vector_key_value = 0x3567c766,
	.boot_vector_value = 0x00000000,
	.boot_vector_en_value = 0xa3b66175,
	.cldma_ao_rst_mask = BIT(4),
	.cldma_pd_rst_mask = BIT(13),
};

const struct ccci_platform_data ccci_mt6797_data = {
	.name = "MT6797",
	.cldma = {
		.name = "mt6797-cldma",
		.queue_mask = 0x3f,
		.net_queue_mask = 0xc0,
		.capability = MODEM_CAP_NAPI | MODEM_CAP_TXBUSY_STOP,
	},
	.slave_boot = true,
	.boot_vector_key = 0x1bc0,
	.boot_vector = 0x1bc4,
	.boot_vector_en = 0x1bc8,
	.boot_vector_key_value = 0x3567c766,
	.boot_vector_value = 0x00000000,
	.boot_vector_en_value = 0xa3b66175,
	.cldma_ao_rst_mask = BIT(4),
	.cldma_pd_rst_mask = BIT(13),
};
