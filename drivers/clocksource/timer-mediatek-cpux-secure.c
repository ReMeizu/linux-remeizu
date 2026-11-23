// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6750 CPUXGPT system-counter initialization.
 * Register and secure-call ABI based on the MediaTek GPL MT6755 BSP.
 * Copyright (C) 2015-2016 MediaTek Inc.
 */

#define pr_fmt(fmt) "mtk-cpux-secure: " fmt

#include <linux/arm-smccc.h>
#include <linux/bits.h>
#include <linux/clocksource.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <asm/barrier.h>
#include <asm/processor.h>
#include <asm/sysreg.h>

#include "timer-of.h"

#define CPUX_DATA		0
#define CPUX_INDEX		4
#define CPUX_GLOBAL_CTRL	0
#define CPUX_ENABLE		BIT(0)
#define CPUX_DIV_MASK		GENMASK(10, 8)
#define CPUX_DIV2		BIT(9)
#define CPUX_INPUT_RATE		26000000

/* This legacy SiP service uses SMC32 even from a 64-bit kernel. */
#define MTK_MCUSYS_WRITE \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32, \
			   ARM_SMCCC_OWNER_SIP, 0x201)

static struct timer_of cpux_timer = {
	.flags = TIMER_OF_BASE | TIMER_OF_CLOCK,
};

static int __init mtk_cpux_secure_write(phys_addr_t address, u32 value)
{
	struct arm_smccc_res result;

	if (upper_32_bits(address))
		return -ERANGE;

	arm_smccc_smc(MTK_MCUSYS_WRITE, address, value, 0, 0, 0, 0, 0,
		      &result);
	switch ((s32)result.a0) {
	case 0:
		return 0;
	case -1:
		return -EOPNOTSUPP;
	case -2:
		return -EINVAL;
	case -3:
		return -ERANGE;
	case -4:
		return -EACCES;
	default:
		return -EIO;
	}
}

static int __init mtk_cpux_secure_init(struct device_node *node)
{
	struct resource resource;
	u32 old, desired, actual;
	u64 before;
	int ret, i;

	ret = of_address_to_resource(node, 0, &resource);
	if (ret)
		return ret;
	if (resource_size(&resource) != 8)
		return -EINVAL;

	ret = timer_of_init(node, &cpux_timer);
	if (ret)
		return ret;

	if (timer_of_rate(&cpux_timer) != CPUX_INPUT_RATE ||
	    read_sysreg(cntfrq_el0) != CPUX_INPUT_RATE / 2) {
		pr_err("clock/CNTFRQ mismatch: %lu/%llu\n",
		       timer_of_rate(&cpux_timer), read_sysreg(cntfrq_el0));
		ret = -EINVAL;
		goto failed;
	}

	ret = mtk_cpux_secure_write(resource.start + CPUX_INDEX,
				   CPUX_GLOBAL_CTRL);
	if (ret)
		goto failed;
	old = readl(timer_of_base(&cpux_timer) + CPUX_DATA);

	/* Never change the rate of a counter already used by secure firmware. */
	if ((old & CPUX_ENABLE) && (old & CPUX_DIV_MASK) != CPUX_DIV2) {
		ret = -EBUSY;
		goto failed;
	}

	desired = (old & ~CPUX_DIV_MASK) | CPUX_DIV2 | CPUX_ENABLE;
	if (desired != old) {
		ret = mtk_cpux_secure_write(resource.start + CPUX_DATA, desired);
		if (ret)
			goto failed;
	}

	ret = mtk_cpux_secure_write(resource.start + CPUX_INDEX,
				   CPUX_GLOBAL_CTRL);
	if (ret)
		goto failed;
	actual = readl(timer_of_base(&cpux_timer) + CPUX_DATA);
	if ((actual & (CPUX_DIV_MASK | CPUX_ENABLE)) !=
	    (CPUX_DIV2 | CPUX_ENABLE)) {
		ret = -EIO;
		goto failed;
	}

	/* A stopped counter also stops udelay(), so use a bounded instruction loop. */
	isb();
	before = read_sysreg(cntvct_el0);
	for (i = 0; i < 10000; i++) {
		isb();
		if (read_sysreg(cntvct_el0) != before) {
			pr_info("system counter running at 13000000 Hz\n");
			return 0;
		}
		cpu_relax();
	}
	ret = -ETIMEDOUT;
failed:
	pr_err("secure system-counter initialization failed: %d\n", ret);
	/* Keep the input clock enabled: secure firmware may already use the counter. */
	return ret;
}
TIMER_OF_DECLARE(mtk_mt6750_secure, "mediatek,mt6750-cpuxgpt",
		 mtk_cpux_secure_init);
