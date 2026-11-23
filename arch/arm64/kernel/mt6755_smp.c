// SPDX-License-Identifier: GPL-2.0-only
/*
 * SMP bring-up for MediaTek MT6755 (Helio P10) behind the stock BL31.
 *
 * The stock ATF (BL31, "tee1" partition) implements PSCI v0.2, but its
 * plat_affinst_on() only records the warm-boot entrypoint and programs the
 * per-CPU reset vector in MCUCFG (0x1020xxxx); it does NOT power up the
 * CPU's MTCMOS power domain (verified by disassembly of the device's
 * dev_tee1.bin: no SPM 0x10006xxx access anywhere in BL31).
 *
 * The 3.18 BSP therefore performed the SPM MTCMOS power-on from the
 * non-secure kernel right after PSCI CPU_ON (enable-method "mt-boot",
 * drivers/misc/mediatek/base/power/mt6755/mt_psci.c).  This file
 * replicates that as an arm64 enable-method "mediatek,mt6755-smp":
 *
 *   cpu_boot = PSCI CPU_ON (BL31 arms the entrypoint) + SPM MTCMOS ON
 *   cpu_kill = PSCI AFFINITY_INFO poll + SPM MTCMOS OFF
 *
 * Register sequences are 1:1 from the BSP
 * (drivers/misc/mediatek/base/power/mt6755/mt_spm_mtcmos.c) but with
 * bounded polling so a stuck handshake degrades to a failed hotplug
 * instead of a hard hang.
 */

#define pr_fmt(fmt) "mt6755-smp: " fmt

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/smp.h>
#include <linux/spinlock.h>

#include <asm/cpu_ops.h>
#include <asm/cputype.h>
#include <asm/smp_plat.h>

extern const struct cpu_operations cpu_psci_ops;

#define MT6755_SPM_PHYS			0x10006000
#define MT6755_INFRACFG_PHYS		0x10001000
#define MT6755_APMIXED_PHYS		0x1000c000
#define MT6755_IOMAP_SIZE		0x1000

/* SPM (sleep controller) registers, offsets from 0x10006000 */
#define SPM_POWERON_CONFIG_EN		0x000
#define  SPM_REGWR_CFG_KEY		(0xb16 << 16)
#define  SPM_BCLK_CG_EN			BIT(0)
#define SPM_CPU_IDLE_STA		0x174
#define SPM_PWR_STATUS			0x180
#define SPM_PWR_STATUS_2ND		0x184
#define SPM_MP0_CPU_PWR_CON(n)		(0x208 + 4 * (n))	/* n = 0..3 */
#define SPM_MP1_CPUTOP_PWR_CON		0x218
#define SPM_MP1_CPU_PWR_CON(n)		(0x21c + 4 * (n))	/* n = 0..3 */
#define SPM_MP0_CPU_L1_PDN(n)		(0x248 + 4 * (n))	/* n = 0..3 */
#define SPM_MP1_CPUTOP_L2_PDN		0x258
#define SPM_MP1_CPU_L1_PDN(n)		(0x260 + 4 * (n))	/* n = 0..3 */

/* *_PWR_CON bits */
#define PWR_RST_B			BIT(0)
#define PWR_ISO				BIT(1)
#define PWR_ON				BIT(2)
#define PWR_ON_2ND			BIT(3)
#define PWR_CLK_DIS			BIT(4)
#define SRAM_CKISO			BIT(5)
#define SRAM_ISOINT_B			BIT(6)

/* *_L1_PDN / *_L2_PDN bits */
#define SRAM_PDN			BIT(0)
#define SRAM_PDN_ACK			BIT(8)

/* PWR_STATUS / PWR_STATUS_2ND bits */
#define MP0_CPUTOP_STA			BIT(8)
#define MP0_CPU_STA(n)			BIT(9 + (n))
#define MP1_CPUTOP_STA			BIT(15)
#define MP1_CPU_STA(n)			BIT(16 + (n))

/* CPU_IDLE_STA bits (standbywfi) */
#define MP0_CPU_WFI(n)			BIT(10 + (n))
#define MP1_CPU_WFI(n)			BIT(14 + (n))

/* infracfg_ao registers, offsets from 0x10001000 */
#define INFRA_TOPAXI_PROTECTEN		0x220
#define INFRA_TOPAXI_PROTECTSTA1	0x228
#define  MP1_CPUTOP_PROT_MASK		BIT(30)

/* apmixedsys: ARMPLL_L ("ARMCA15PLL"), clocks cluster 1 (CPU4-7) */
#define ARMCA15PLL_CON0			0x200
#define ARMCA15PLL_CON1			0x204
#define ARMCA15PLL_PWR_CON0		0x20c

static void __iomem *mt6755_spm;
static void __iomem *mt6755_infracfg;
static void __iomem *mt6755_apmixed;
static DEFINE_RAW_SPINLOCK(mt6755_spm_lock);

/* per-step handshake budget; BSP polled these unbounded (they ack in <1us) */
#define MT6755_POLL_LOOPS		20000	/* x 1us = 20 ms */

static int mt6755_poll(void __iomem *reg, u32 mask, bool set)
{
	int i;

	for (i = 0; i < MT6755_POLL_LOOPS; i++) {
		u32 val = readl(reg);

		if (set ? ((val & mask) == mask) : !(val & mask))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int mt6755_poll_pwr_sta(u32 mask, bool set)
{
	int ret;

	ret = mt6755_poll(mt6755_spm + SPM_PWR_STATUS, mask, set);
	if (ret)
		return ret;
	return mt6755_poll(mt6755_spm + SPM_PWR_STATUS_2ND, mask, set);
}

static void mt6755_setbits(void __iomem *reg, u32 bits)
{
	writel(readl(reg) | bits, reg);
}

static void mt6755_clrbits(void __iomem *reg, u32 bits)
{
	writel(readl(reg) & ~bits, reg);
}

/*
 * Power up cluster 1 (MP1 CPUTOP): ARMPLL_L on, CPUTOP MTCMOS on, release
 * TOPAXI bus protection.  BSP: spm_mtcmos_ctrl_cpusys1(STA_POWER_ON) +
 * enable_armpll_l().  Freqhopping / SPM-hwmode steps of the BSP are
 * intentionally skipped (no freqhopping support in mainline; hwmode only
 * matters for SPM idle power control).
 */
static int mt6755_cluster1_power_on(void)
{
	void __iomem *pwr_con = mt6755_spm + SPM_MP1_CPUTOP_PWR_CON;
	void __iomem *l2_pdn = mt6755_spm + SPM_MP1_CPUTOP_L2_PDN;
	int ret;

	if ((readl(mt6755_spm + SPM_PWR_STATUS) & MP1_CPUTOP_STA) &&
	    (readl(mt6755_spm + SPM_PWR_STATUS_2ND) & MP1_CPUTOP_STA))
		return 0;

	/* ARMPLL_L on (BSP enable_armpll_l()) */
	mt6755_setbits(mt6755_apmixed + ARMCA15PLL_PWR_CON0, BIT(0));
	udelay(2);
	mt6755_clrbits(mt6755_apmixed + ARMCA15PLL_PWR_CON0, BIT(1));
	mt6755_setbits(mt6755_apmixed + ARMCA15PLL_CON1, BIT(31));
	mt6755_setbits(mt6755_apmixed + ARMCA15PLL_CON0, BIT(0));
	udelay(100);

	/* MP1 CPUTOP MTCMOS on */
	mt6755_setbits(pwr_con, PWR_ON);
	mt6755_setbits(pwr_con, PWR_ON_2ND);
	ret = mt6755_poll_pwr_sta(MP1_CPUTOP_STA, true);
	if (ret)
		return ret;
	mt6755_clrbits(pwr_con, PWR_ISO);
	mt6755_clrbits(l2_pdn, SRAM_PDN);
	ret = mt6755_poll(l2_pdn, SRAM_PDN_ACK, false);
	if (ret)
		return ret;
	udelay(1);
	mt6755_setbits(pwr_con, SRAM_ISOINT_B);
	udelay(1);
	mt6755_clrbits(pwr_con, SRAM_CKISO);
	mt6755_clrbits(pwr_con, PWR_CLK_DIS);
	mt6755_setbits(pwr_con, PWR_RST_B);

	/* release bus protection */
	mt6755_clrbits(mt6755_infracfg + INFRA_TOPAXI_PROTECTEN,
		       MP1_CPUTOP_PROT_MASK);
	return mt6755_poll(mt6755_infracfg + INFRA_TOPAXI_PROTECTSTA1,
			   MP1_CPUTOP_PROT_MASK, false);
}

/* BSP spm_mtcmos_ctrl_cpuN(STA_POWER_ON), N = 1..7 */
static int mt6755_cpu_power_on(unsigned int cluster, unsigned int core)
{
	void __iomem *pwr_con, *l1_pdn;
	unsigned long flags;
	u32 sta_mask;
	int ret;

	raw_spin_lock_irqsave(&mt6755_spm_lock, flags);

	/* unlock SPM register writes */
	writel(SPM_REGWR_CFG_KEY | SPM_BCLK_CG_EN,
	       mt6755_spm + SPM_POWERON_CONFIG_EN);

	if (cluster == 0) {
		/* cluster 0 is alive (we are running on it) */
		pwr_con = mt6755_spm + SPM_MP0_CPU_PWR_CON(core);
		l1_pdn = mt6755_spm + SPM_MP0_CPU_L1_PDN(core);
		sta_mask = MP0_CPU_STA(core);
	} else {
		ret = mt6755_cluster1_power_on();
		if (ret)
			goto out;
		pwr_con = mt6755_spm + SPM_MP1_CPU_PWR_CON(core);
		l1_pdn = mt6755_spm + SPM_MP1_CPU_L1_PDN(core);
		sta_mask = MP1_CPU_STA(core);
	}

	mt6755_setbits(pwr_con, PWR_ON);
	mt6755_setbits(pwr_con, PWR_ON_2ND);
	ret = mt6755_poll_pwr_sta(sta_mask, true);
	if (ret)
		goto out;
	mt6755_clrbits(pwr_con, PWR_ISO);
	mt6755_clrbits(l1_pdn, SRAM_PDN);
	ret = mt6755_poll(l1_pdn, SRAM_PDN_ACK, false);
	if (ret)
		goto out;
	udelay(1);
	mt6755_setbits(pwr_con, SRAM_ISOINT_B);
	mt6755_clrbits(pwr_con, SRAM_CKISO);
	mt6755_clrbits(pwr_con, PWR_CLK_DIS);
	/* release reset: core enters BL31 warm entry via MCUCFG rv_addr */
	mt6755_setbits(pwr_con, PWR_RST_B);
	ret = 0;
out:
	raw_spin_unlock_irqrestore(&mt6755_spm_lock, flags);
	return ret;
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * BSP spm_mtcmos_ctrl_cpuN(STA_POWER_DOWN, chkWfiBeforePdn=1).
 * The dying CPU has executed PSCI CPU_OFF and sits in wfi inside BL31
 * with its MTCMOS domain still powered; cut the power here so that a
 * subsequent CPU_ON + power-up resets it cleanly.
 * Cluster 1 CPUTOP is deliberately left powered once on (BSP powers it
 * down with the last MP1 core; skipped for simplicity/safety).
 */
static int mt6755_cpu_power_off(unsigned int cluster, unsigned int core)
{
	void __iomem *pwr_con, *l1_pdn;
	unsigned long flags;
	u32 sta_mask, wfi_mask;
	int ret;

	if (cluster == 0) {
		pwr_con = mt6755_spm + SPM_MP0_CPU_PWR_CON(core);
		l1_pdn = mt6755_spm + SPM_MP0_CPU_L1_PDN(core);
		sta_mask = MP0_CPU_STA(core);
		wfi_mask = MP0_CPU_WFI(core);
	} else {
		pwr_con = mt6755_spm + SPM_MP1_CPU_PWR_CON(core);
		l1_pdn = mt6755_spm + SPM_MP1_CPU_L1_PDN(core);
		sta_mask = MP1_CPU_STA(core);
		wfi_mask = MP1_CPU_WFI(core);
	}

	/* wait for the core to reach wfi in BL31 */
	ret = mt6755_poll(mt6755_spm + SPM_CPU_IDLE_STA, wfi_mask, true);
	if (ret)
		return ret;

	raw_spin_lock_irqsave(&mt6755_spm_lock, flags);
	writel(SPM_REGWR_CFG_KEY | SPM_BCLK_CG_EN,
	       mt6755_spm + SPM_POWERON_CONFIG_EN);
	mt6755_setbits(pwr_con, PWR_ISO);
	mt6755_setbits(pwr_con, SRAM_CKISO);
	mt6755_clrbits(pwr_con, SRAM_ISOINT_B);
	mt6755_setbits(l1_pdn, SRAM_PDN);
	ret = mt6755_poll(l1_pdn, SRAM_PDN_ACK, true);
	if (ret)
		goto out;
	mt6755_clrbits(pwr_con, PWR_RST_B);
	mt6755_setbits(pwr_con, PWR_CLK_DIS);
	mt6755_clrbits(pwr_con, PWR_ON);
	mt6755_clrbits(pwr_con, PWR_ON_2ND);
	ret = mt6755_poll_pwr_sta(sta_mask, false);
out:
	raw_spin_unlock_irqrestore(&mt6755_spm_lock, flags);
	return ret;
}
#endif /* CONFIG_HOTPLUG_CPU */

static int __init mt6755_smp_map(void)
{
	if (mt6755_spm)
		return 0;

	mt6755_spm = ioremap(MT6755_SPM_PHYS, MT6755_IOMAP_SIZE);
	mt6755_infracfg = ioremap(MT6755_INFRACFG_PHYS, MT6755_IOMAP_SIZE);
	mt6755_apmixed = ioremap(MT6755_APMIXED_PHYS, MT6755_IOMAP_SIZE);
	if (!mt6755_spm || !mt6755_infracfg || !mt6755_apmixed) {
		pr_err("failed to map SPM/infracfg/apmixed\n");
		if (mt6755_spm)
			iounmap(mt6755_spm);
		if (mt6755_infracfg)
			iounmap(mt6755_infracfg);
		if (mt6755_apmixed)
			iounmap(mt6755_apmixed);
		mt6755_spm = NULL;
		mt6755_infracfg = NULL;
		mt6755_apmixed = NULL;
		return -ENOMEM;
	}
	return 0;
}

static int __init mt6755_cpu_init(unsigned int cpu)
{
	return cpu_psci_ops.cpu_init(cpu);
}

static int __init mt6755_cpu_prepare(unsigned int cpu)
{
	int ret;

	ret = mt6755_smp_map();
	if (ret)
		return ret;

	return cpu_psci_ops.cpu_prepare(cpu);
}

static int mt6755_cpu_boot(unsigned int cpu)
{
	u64 mpidr = cpu_logical_map(cpu);
	unsigned int cluster = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	unsigned int core = MPIDR_AFFINITY_LEVEL(mpidr, 0);
	int ret;

	if (cluster > 1 || core > 3)
		return -EINVAL;

	/* 1. PSCI CPU_ON: BL31 validates + arms entrypoint and rv_addr */
	ret = cpu_psci_ops.cpu_boot(cpu);
	if (ret)
		return ret;

	/* 2. actually power the core; it resets into BL31 warm entry */
	ret = mt6755_cpu_power_on(cluster, core);
	if (ret)
		pr_err("CPU%u (mpidr %u/%u): MTCMOS power-on failed (%d)\n",
		       cpu, cluster, core, ret);
	else
		pr_info("CPU%u (mpidr %u/%u): powered on\n",
			cpu, cluster, core);
	return ret;
}

#ifdef CONFIG_HOTPLUG_CPU
static bool mt6755_cpu_can_disable(unsigned int cpu)
{
	return cpu_psci_ops.cpu_can_disable(cpu);
}

static int mt6755_cpu_disable(unsigned int cpu)
{
	return cpu_psci_ops.cpu_disable(cpu);
}

static void mt6755_cpu_die(unsigned int cpu)
{
	cpu_psci_ops.cpu_die(cpu);
}

static int mt6755_cpu_kill(unsigned int cpu)
{
	u64 mpidr = cpu_logical_map(cpu);
	unsigned int cluster = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	unsigned int core = MPIDR_AFFINITY_LEVEL(mpidr, 0);
	int ret;

	ret = cpu_psci_ops.cpu_kill(cpu);
	if (ret)
		return ret;

	ret = mt6755_cpu_power_off(cluster, core);
	if (ret)
		pr_warn("CPU%u: MTCMOS power-off failed (%d)\n", cpu, ret);
	return ret;
}
#endif

const struct cpu_operations mt6755_cpu_ops = {
	.name		= "mediatek,mt6755-smp",
	.cpu_init	= mt6755_cpu_init,
	.cpu_prepare	= mt6755_cpu_prepare,
	.cpu_boot	= mt6755_cpu_boot,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_can_disable = mt6755_cpu_can_disable,
	.cpu_disable	= mt6755_cpu_disable,
	.cpu_die	= mt6755_cpu_die,
	.cpu_kill	= mt6755_cpu_kill,
#endif
};
