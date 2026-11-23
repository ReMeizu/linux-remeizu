// SPDX-License-Identifier: GPL-2.0-only
/*
 * SMP bring-up for the MediaTek MT6735 family (MT6735/MT6735M/MT6753)
 * behind the stock BL31.
 *
 * The stock ATF (BL31, "tee1" partition) exposes the PSCI v0.2 CPU_ON
 * call that the vendor DT uses (0x84000003), but it does not power up
 * the CPU's MTCMOS domain: a scan of the two MT6735-family BL31 images
 * available to us (ctyon-mt6735/tee1.bin and the Meizu M5c dump) finds no
 * access to the SPM block at 0x10006000 at all.  No MT6753 BL31 dump
 * exists yet, so the same is assumed for MT6753.  This matches the 3.18
 * BSP, where the non-secure kernel powers the core on right after PSCI
 * CPU_ON (enable-method "mt-boot").
 *
 * This file implements that enable-method:
 *
 *   cpu_boot = PSCI CPU_ON (BL31 arms the entrypoint) + SPM MTCMOS ON
 *
 * Register sequences are taken 1:1 from the downstream MT6735 BSP; every
 * register and bit below carries the downstream file:line it comes from.
 * Unlike the BSP, every handshake poll is bounded so that a stuck MTCMOS
 * ACK turns into a failed CPU bring-up instead of a silent hang.
 *
 * Deliberately not implemented (yet): CPU hotplug power-off.  It needs
 * the same handshakes in reverse after PSCI CPU_OFF and has not been
 * validated on hardware; without ->cpu_die the generic arm64 code
 * simply refuses to offline CPUs instead of guessing.
 *
 * Known gap for cluster 1: the BSP additionally clears ACINACTM in
 * MP0_AXI_CONFIG through the proprietary MTK SIP 0x82000201
 * (mt-smp.c:354) and enables CCI-400 snoops at 0x10395000
 * (mt-smp.c:360-361).  That SIP does not exist in mainline and CCI-400
 * has no DT node for this SoC, so neither is done here; cluster 0
 * (CPU1-3) is intra-cluster coherent, cluster 1 (CPU4-7) coherency is
 * unproven.
 *
 * Downstream references (android_kernel_m2note_4ida):
 *   D1 drivers/misc/mediatek/mach/mt6735/mt_psci.c
 *   D2 drivers/misc/mediatek/mach/mt6735/mt_spm_mtcmos.c
 *   D3 drivers/misc/mediatek/mach/mt6735/include/mach/mt_spm.h
 *   D4 drivers/misc/mediatek/mach/mt6735/include/mach/mt_spm_mtcmos_internal.h
 */

#define pr_fmt(fmt) "mt6735-smp: " fmt

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/spinlock.h>

#include <asm/cpu_ops.h>
#include <asm/cputype.h>
#include <asm/smp_plat.h>

extern const struct cpu_operations cpu_psci_ops;

/*
 * SPM (sleep controller) registers, offsets from 0x10006000.
 * D3:71-107,162-163
 */
#define SPM_POWERON_CONFIG_SET		0x000	/* D3:71 */
#define SPM_REGWR_EN			BIT(0)	/* D3:231 */
#define SPM_PROJECT_CODE		0xb16	/* D3:229 */
#define SPM_REGWR_CFG_KEY		(SPM_PROJECT_CODE << 16) /* D3:232 */
#define SPM_CA7_CPU0_PWR_CON		0x200	/* D3:75 */
#define SPM_CA7_CPUTOP_PWR_CON		0x208	/* D3:77 */
#define SPM_CA7_CPU1_PWR_CON		0x218	/* D3:80 */
#define SPM_CA7_CPU2_PWR_CON		0x21c	/* D3:81 */
#define SPM_CA7_CPU3_PWR_CON		0x220	/* D3:82 */
#define SPM_CA7_CPU1_L1_PDN		0x264	/* D3:91 */
#define SPM_CA7_CPU2_L1_PDN		0x26c	/* D3:92 */
#define SPM_CA7_CPU3_L1_PDN		0x274	/* D3:93 */
#define SPM_CA15_CPU0_PWR_CON		0x2a0	/* D3:101 */
#define SPM_CA15_CPU1_PWR_CON		0x2a4	/* D3:102 */
#define SPM_CA15_CPU2_PWR_CON		0x2a8	/* D3:103 */
#define SPM_CA15_CPU3_PWR_CON		0x2ac	/* D3:104 */
#define SPM_CA15_CPUTOP_PWR_CON		0x2b0	/* D3:105 */
#define SPM_CA15_L1_PWR_CON		0x2b4	/* D3:106 */
#define SPM_CA15_L2_PWR_CON		0x2b8	/* D3:107 */
#define SPM_PWR_STATUS			0x60c	/* D3:162 */
#define SPM_PWR_STATUS_2ND		0x610	/* D3:163 */

/* *_PWR_CON bits, D4:22-28 */
#define PWR_RST_B			BIT(0)
#define PWR_ISO				BIT(1)
#define PWR_ON				BIT(2)
#define PWR_ON_2ND			BIT(3)
#define PWR_CLK_DIS			BIT(4)
#define SRAM_CKISO			BIT(5)
#define SRAM_ISOINT_B			BIT(6)

/* CA7 per-CPU L1_PDN bits, D4:34-35 */
#define L1_PDN				BIT(0)
#define L1_PDN_ACK			BIT(8)

/* CA15 CPUTOP L2_PDN bits, D4:58-62 */
#define CA15_L2_PDN			BIT(0)
#define CA15_L2_PDN_ACK			BIT(0)
#define CA15_L2_PDN_ISO			BIT(2)

/* CA15 shared L1_PWR_CON bits, D4:44-55 */
#define CA15_L1_PDN_ACK(n)		BIT(16 + (n))
#define CA15_L1_PDN_ISO(n)		BIT(4 + (n))
#define CA15_L1_PDN(n)			BIT(n)

/* PWR_STATUS / PWR_STATUS_2ND bits, D4:66-75 */
#define CA15_CPUTOP			BIT(16)
#define CA15_CPU_STA(n)			BIT(20 - (n))
#define CA7_CPU_STA(n)			BIT(13 - (n))	/* n = 0..3 */

#define MT6735_NO_L1_ISO		0xff

/* Per handshake budget; the BSP polled these unbounded and they ACK in <1us */
#define MT6735_POLL_LOOPS		20000	/* x 1us = 20 ms */

struct mt6735_cpu_bank {
	u16 pwr_con;		/* SPM_CA7/CA15_CPUx_PWR_CON */
	u16 l1_reg;		/* per-CPU L1_PDN or the shared CA15 one */
	u8 l1_pdn_bit;
	u8 l1_ack_bit;
	u8 l1_iso_bit;		/* MT6735_NO_L1_ISO if the bank has none */
	u8 status_bit;		/* PWR_STATUS / PWR_STATUS_2ND bit */
};

/*
 * Indexed by [MPIDR affinity level 1][affinity level 0].  Cluster 0 is the
 * "CA7" bank (D2:156-345), cluster 1 the "CA15" bank (D2:348-644).
 */
static const struct mt6735_cpu_bank mt6735_banks[2][4] = {
	[0] = {
		/* D3:75,90; D4:75,35 */
		{ SPM_CA7_CPU0_PWR_CON, 0x25c, 0, 8, MT6735_NO_L1_ISO, 13 },
		/* D3:80,91 */
		{ SPM_CA7_CPU1_PWR_CON, SPM_CA7_CPU1_L1_PDN, 0, 8,
		  MT6735_NO_L1_ISO, 12 },
		/* D3:81,92 */
		{ SPM_CA7_CPU2_PWR_CON, SPM_CA7_CPU2_L1_PDN, 0, 8,
		  MT6735_NO_L1_ISO, 11 },
		/* D3:82,93 */
		{ SPM_CA7_CPU3_PWR_CON, SPM_CA7_CPU3_L1_PDN, 0, 8,
		  MT6735_NO_L1_ISO, 10 },
	},
	[1] = {
		/* D3:101; D4:47,50-55,69 */
		{ SPM_CA15_CPU0_PWR_CON, SPM_CA15_L1_PWR_CON, 0, 16, 4, 20 },
		/* D3:102; D4:46,54,68 */
		{ SPM_CA15_CPU1_PWR_CON, SPM_CA15_L1_PWR_CON, 1, 17, 5, 19 },
		/* D3:103; D4:45,53,67 */
		{ SPM_CA15_CPU2_PWR_CON, SPM_CA15_L1_PWR_CON, 2, 18, 6, 18 },
		/* D3:104; D4:44,52,66 */
		{ SPM_CA15_CPU3_PWR_CON, SPM_CA15_L1_PWR_CON, 3, 19, 7, 17 },
	},
};

static void __iomem *mt6735_spm;
static DEFINE_RAW_SPINLOCK(mt6735_spm_lock);

static int mt6735_poll(void __iomem *reg, u32 mask, bool set)
{
	int i;

	for (i = 0; i < MT6735_POLL_LOOPS; i++) {
		u32 val = readl(reg);

		if (set ? ((val & mask) == mask) : !(val & mask))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/* D2:121,134,185,198: both PWR_STATUS registers must agree */
static int mt6735_poll_status(u32 mask, bool set)
{
	int i;

	for (i = 0; i < MT6735_POLL_LOOPS; i++) {
		u32 cur = readl(mt6735_spm + SPM_PWR_STATUS);
		u32 sec = readl(mt6735_spm + SPM_PWR_STATUS_2ND);
		bool ok = set ? (((cur & mask) == mask) && ((sec & mask) == mask))
			      : (!(cur & mask) && !(sec & mask));

		if (ok)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static void mt6735_setbits(void __iomem *reg, u32 bits)
{
	writel(readl(reg) | bits, reg);
}

static void mt6735_clrbits(void __iomem *reg, u32 bits)
{
	writel(readl(reg) & ~bits, reg);
}

/*
 * Cluster 1 CPUTOP (CA15_CPUTOP + CA15 L2), D2:835-925 (STA_POWER_ON
 * branch).  Called with mt6735_spm_lock held.  The BSP powers CPUTOP down
 * with the last cluster-1 core; we only ever power it on.
 */
static int mt6735_cluster1_power_on(void)
{
	void __iomem *pwr_con = mt6735_spm + SPM_CA15_CPUTOP_PWR_CON;
	void __iomem *l2_pdn = mt6735_spm + SPM_CA15_L2_PWR_CON;
	int ret;

	/* D2:390-392: only power CPUTOP if both status registers say it is off */
	if ((readl(mt6735_spm + SPM_PWR_STATUS) & CA15_CPUTOP) &&
	    (readl(mt6735_spm + SPM_PWR_STATUS_2ND) & CA15_CPUTOP))
		return 0;

	/* D2:888-892 */
	mt6735_setbits(pwr_con, PWR_ON);
	udelay(1);
	mt6735_setbits(pwr_con, PWR_ON_2ND);
	ret = mt6735_poll_status(CA15_CPUTOP, true);
	if (ret)
		return ret;

	/* D2:895-901 */
	mt6735_clrbits(pwr_con, PWR_ISO);
	mt6735_clrbits(l2_pdn, CA15_L2_PDN_ISO);
	mt6735_clrbits(l2_pdn, CA15_L2_PDN);
	ret = mt6735_poll(l2_pdn, CA15_L2_PDN_ACK, false);
	if (ret)
		return ret;

	/* D2:910-916 */
	ndelay(900);
	mt6735_setbits(pwr_con, SRAM_ISOINT_B);
	ndelay(100);
	mt6735_clrbits(pwr_con, SRAM_CKISO);
	mt6735_clrbits(pwr_con, PWR_CLK_DIS);
	mt6735_setbits(pwr_con, PWR_RST_B);
	return 0;
}

/*
 * D2:156-218 (cpu1), 220-282 (cpu2), 284-346 (cpu3),
 * 348-421 (cpu4), 423-495 (cpu5), 497-569 (cpu6), 571-643 (cpu7),
 * all STA_POWER_ON branches, chkWfiBeforePdn is only used on power-down.
 */
static int mt6735_cpu_power_on(unsigned int cluster, unsigned int core)
{
	const struct mt6735_cpu_bank *bank = &mt6735_banks[cluster][core];
	void __iomem *pwr_con, *l1;
	unsigned long flags;
	int ret = 0;

	pwr_con = mt6735_spm + bank->pwr_con;
	l1 = mt6735_spm + bank->l1_reg;

	raw_spin_lock_irqsave(&mt6735_spm_lock, flags);

	/* D2:93-94: unlock SPM register writes */
	writel(SPM_REGWR_CFG_KEY | SPM_REGWR_EN,
	       mt6735_spm + SPM_POWERON_CONFIG_SET);

	if (cluster == 1) {
		ret = mt6735_cluster1_power_on();
		if (ret)
			goto out;
	}

	/* D2:194-198 (cpu1), D2:396-400 (cpu4) */
	mt6735_setbits(pwr_con, PWR_ON);
	udelay(1);
	mt6735_setbits(pwr_con, PWR_ON_2ND);
	ret = mt6735_poll_status(BIT(bank->status_bit), true);
	if (ret)
		goto out;

	/* D2:201-206 (cpu1), D2:403-409 (cpu4) */
	mt6735_clrbits(pwr_con, PWR_ISO);
	if (bank->l1_iso_bit != MT6735_NO_L1_ISO)
		mt6735_clrbits(l1, BIT(bank->l1_iso_bit));
	mt6735_clrbits(l1, BIT(bank->l1_pdn_bit));
	ret = mt6735_poll(l1, BIT(bank->l1_ack_bit), false);
	if (ret)
		goto out;

	/* D2:207-212 (cpu1), D2:410-415 (cpu4) */
	udelay(1);
	mt6735_setbits(pwr_con, SRAM_ISOINT_B);
	mt6735_clrbits(pwr_con, SRAM_CKISO);
	mt6735_clrbits(pwr_con, PWR_CLK_DIS);
	mt6735_setbits(pwr_con, PWR_RST_B);

out:
	raw_spin_unlock_irqrestore(&mt6735_spm_lock, flags);
	return ret;
}

/*
 * D2:36-55: SPM base comes from the DT node compatible with the downstream
 * "mediatek,SLEEP" (reg 0x10006000 0x1000, no clocks in the vendor DTS).
 */
static int __init mt6735_spm_map(void)
{
	struct device_node *np;

	if (mt6735_spm)
		return 0;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6753-spm");
	if (!np)
		return -ENODEV;

	mt6735_spm = of_iomap(np, 0);
	of_node_put(np);
	if (!mt6735_spm)
		return -ENOMEM;

	return 0;
}

static int __init mt6735_cpu_init(unsigned int cpu)
{
	return 0;			/* D1:14-18 */
}

static int __init mt6735_cpu_prepare(unsigned int cpu)
{
	int ret;

	/* D1:20-26: map SPM once, then defer to the PSCI ops */
	ret = mt6735_spm_map();
	if (ret) {
		pr_err("SPM mapping failed (%d), CPU%d cannot be prepared\n",
		       ret, cpu);
		return ret;
	}

	return cpu_psci_ops.cpu_prepare(cpu);
}

static int mt6735_cpu_boot(unsigned int cpu)
{
	u64 mpidr = cpu_logical_map(cpu);
	unsigned int cluster = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	unsigned int core = MPIDR_AFFINITY_LEVEL(mpidr, 0);
	int ret;

	if (cluster > 1 || core > 3)
		return -EINVAL;

	/* D1:31: PSCI CPU_ON first; BL31 only arms the entrypoint */
	ret = cpu_psci_ops.cpu_boot(cpu);
	if (ret)
		return ret;

	/* D1:35: then bring the core's MTCMOS domain up */
	ret = mt6735_cpu_power_on(cluster, core);
	if (ret)
		pr_err("CPU%u (cluster %u core %u): MTCMOS power-on failed (%d)\n",
		       cpu, cluster, core, ret);

	return ret;
}

const struct cpu_operations mt6735_cpu_ops = {
	.name		= "mt-boot",	/* D1:89 */
	.cpu_init	= mt6735_cpu_init,
	.cpu_prepare	= mt6735_cpu_prepare,
	.cpu_boot	= mt6735_cpu_boot,
};
