/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Meizu Linux mainline project
 *
 * Register level support for the MediaTek CONSYS (Wi-Fi/BT/GPS combo)
 * subsystem of the MT6735/MT6735M/MT6737M, MT6750/MT6755 and MT6797 SoCs.
 *
 * The CONSYS block is powered from the AP SPM controller, clocked through
 * topckgen and keeps its firmware in DRAM shared with the AP.  This file
 * implements the register sequences recovered from the vendor "conn_soc"
 * driver (mtk_wcn_consys_hw.c, wmt_plat_alps.c), translated to the modern
 * register-map, clock, interrupt and reserved-memory APIs.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#include "connsys.h"
#include "connsys_hw.h"

/* --- offsets inside the CONN MCU / HIF windows ------------------------ */
#define CONNSYS_CHIP_ID_OFF		0x0008
#define CONNSYS_MCU_CFG_ACR_OFF		0x0110
#define CONNSYS_ROM_RAM_DELSEL_OFF	0x0114
#define CONNSYS_CPUPCR_OFF		0x0160

/* --- offsets inside the AP reset-generator window --------------------- */
#define CONNSYS_CPU_SW_RST_OFF		0x0018

/* --- offsets inside the sleep-power-manager window -------------------- */
#define CONNSYS_SPM_CONN_PWR_CON	0x0000

#define CONNSYS_PWRON_CONFG_EN_VALUE	0x0b160001	/* power-on config on	*/
#define CONNSYS_PWRON_CONFG_DIS_VALUE	0x0b160000	/* power-on config off	*/
#define CONNSYS_SPM_PWR_ON_SET		BIT(30)
#define CONNSYS_SPM_PWR_ON_CLR		BIT(31)

#define CONNSYS_CPU_SW_RST_BIT		BIT(12)
#define CONSYS_CPU_SW_RST_CTRL_MASK	GENMASK(31, 24)
#define CONNSYS_CPU_SW_RST_CTRL_KEY	(0x88 << 24)
#define CONNSYS_CPU_SW_RST_CTRL_MASK	GENMASK(31, 24)

#define CONNSYS_MCU_CFG_ACR_MBIST_BIT	BIT(18)

/* Accepted values of the CONNSYS chip id register */
#define CONNSYS_CHIP_ID_MT6735		0x0321
#define CONNSYS_CHIP_ID_MT6735M		0x0335
#define CONNSYS_CHIP_ID_MT6735S		0x0337
#define CONNSYS_CHIP_ID_MT6755		0x0326
#define CONNSYS_CHIP_ID_MT6797		0x0279

/*
 * Register accessors.  They are called with cd->lock held (or from probe
 * before the device is published), as the windows are shared by the whole
 * suite.
 */
static inline void connsys_wr(struct connsys_dev *cd, enum connsys_reg_block blk,
			      u32 off, u32 val)
{
	writel(val, cd->regs[blk] + off);
}

static inline u32 connsys_rd(struct connsys_dev *cd, enum connsys_reg_block blk,
			     u32 off)
{
	return readl(cd->regs[blk] + off);
}

static void connsys_rmw(struct connsys_dev *cd, enum connsys_reg_block blk,
			u32 off, u32 clear, u32 set)
{
	u32 val = connsys_rd(cd, blk, off);

	val &= ~clear;
	val |= set;
	connsys_wr(cd, blk, off, val);
}

/* Poll @reg until (value & @mask) == @want, with a millisecond budget. */
static int connsys_poll(struct connsys_dev *cd, enum connsys_reg_block blk,
			u32 off, u32 mask, u32 want, u32 timeout_ms)
{
	unsigned int budget = timeout_ms ?: cd->timeout_ms;
	u32 val;

	while (budget--) {
		val = connsys_rd(cd, blk, off);
		if ((val & mask) == want)
			return 0;
		usleep_range(1000, 1500);
	}

	dev_err(cd->dev, "register 0x%08llx did not reach 0x%08x/0x%08x\n",
		(u64)(cd->reg_phys[blk] + off), want, mask);
	return -ETIMEDOUT;
}

/*
 * The BGF line is in fact the connection-free "BTIF bridge" interrupt: it
 * fires when the MCU raises a firmware problem that the AP has to drain.
 * Reading CPUPCR acknowledges the source and gives the dump a marker.
 */
static irqreturn_t connsys_bgf_isr(int irq, void *data)
{
	struct connsys_dev *cd = data;

	dev_warn_ratelimited(cd->dev,
			     "BGF interrupt from CONNSYS, CPUPCR=0x%08x\n",
			     connsys_rd(cd, CONNSYS_REG_MCU, CONNSYS_CPUPCR_OFF));
	return IRQ_HANDLED;
}

static irqreturn_t connsys_wdt_isr(int irq, void *data)
{
	struct connsys_dev *cd = data;
	unsigned long flags;

	spin_lock_irqsave(&cd->launch_lock, flags);
	cd->wdt_events++;
	spin_unlock_irqrestore(&cd->launch_lock, flags);

	dev_warn_ratelimited(cd->dev, "CONNSYS watchdog bite (#%u)\n",
			     cd->wdt_events);
	return IRQ_HANDLED;
}

/*
 * CONN2AP: the MCU wants the AP attention (HIF data available, MCU log
 * flushed...).  The STP RX path is polled from the BTIF interrupt, so the
 * handler only wakes the power-management path.
 */
static irqreturn_t connsys_conn2ap_isr(int irq, void *data)
{
	struct connsys_dev *cd = data;

	wake_up_interruptible_all(&cd->launcher_wq);
	return IRQ_HANDLED;
}

void connsys_activate_irqs(struct connsys_dev *cd)
{
	static const char *const names[] = { "bgf", "wdt", "conn2ap" };
	irq_handler_t const handlers[] = {
		connsys_bgf_isr, connsys_wdt_isr, connsys_conn2ap_isr,
	};
	int *const irqs[] = { &cd->bgf_irq, &cd->wdt_irq, &cd->conn2ap_irq };
	int i, ret;

	if (cd->irq_ready)
		return;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (*irqs[i] < 0)
			continue;
		ret = devm_request_threaded_irq(cd->dev, *irqs[i], NULL,
						handlers[i], IRQF_ONESHOT,
						dev_name(cd->dev), cd);
		if (ret) {
			dev_err(cd->dev, "cannot request the %s interrupt: %d\n",
				names[i], ret);
			*irqs[i] = -ENXIO;
		}
	}

	/* The BTIF line is the STP data path: it must never be shared. */
	if (cd->btif_irq > 0) {
		ret = devm_request_irq(cd->dev, cd->btif_irq,
				       connsys_btif_isr, 0, "btif", cd);
		if (ret) {
			dev_err(cd->dev, "cannot request the BTIF interrupt: %d\n",
				ret);
			cd->btif_irq = -ENXIO;
		}
	}

	cd->irq_ready = true;
}

void connsys_deactivate_irqs(struct connsys_dev *cd)
{
	int *const irqs[] = { &cd->bgf_irq, &cd->wdt_irq, &cd->conn2ap_irq };
	int i;

	if (!cd->irq_ready)
		return;

	if (cd->btif_irq > 0)
		devm_free_irq(cd->dev, cd->btif_irq, cd);

	for (i = 0; i < ARRAY_SIZE(irqs); i++) {
		if (*irqs[i] < 0)
			continue;
		devm_free_irq(cd->dev, *irqs[i], cd);
	}
	cd->irq_ready = false;
}

/**
 * connsys_map_resources - map the register windows, BTIF and clocks
 *
 * Called once from probe.  REG 0 is the CONN MCU window, REG 1 the AP
 * reset generator, REG 2 topckgen, REG 3 the SPM controller and REG 4 the
 * CONN AXI protection block; the optional REG 5 is the shared DRAM window
 * and "btif" the STP transport.
 */
int connsys_map_resources(struct connsys_dev *cd)
{
	struct platform_device *pdev = to_platform_device(cd->dev);
	struct resource *res;
	int i, ret;

	for (i = 0; i < CONNSYS_REG_MAX; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res)
			continue;
		cd->regs[i] = devm_ioremap_resource(cd->dev, res);
		if (IS_ERR(cd->regs[i]))
			return PTR_ERR(cd->regs[i]);
		cd->reg_phys[i] = res->start;
	}

	if (!cd->regs[CONNSYS_REG_MCU] ||
	    !cd->regs[CONNSYS_REG_AP_RGU] ||
	    !cd->regs[CONNSYS_REG_TOPCKGEN] ||
	    !cd->regs[CONNSYS_REG_SPM]) {
		dev_err(cd->dev, "missing mandatory CONSYS register windows\n");
		return -EINVAL;
	}
	if (!cd->regs[CONNSYS_REG_AXI])
		dev_warn(cd->dev, "no CONN AXI window, protection not managed\n");

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "btif");
	if (res) {
		cd->btif = devm_ioremap_resource(cd->dev, res);
		if (IS_ERR(cd->btif))
			return PTR_ERR(cd->btif);
		cd->btif_phys = res->start;
		ret = platform_get_irq_byname_optional(pdev, "btif");
		cd->btif_irq = ret > 0 ? ret : -ENXIO;
	}

	cd->clk_cfg = devm_clk_get_optional_enabled(cd->dev, "connsys_cfg");
	if (IS_ERR(cd->clk_cfg))
		return PTR_ERR(cd->clk_cfg);
	cd->clk_emi = devm_clk_get_optional(cd->dev, "connsys_emi");
	if (IS_ERR(cd->clk_emi))
		return PTR_ERR(cd->clk_emi);
	cd->clk_btif = devm_clk_get_optional(cd->dev, "btif");
	if (IS_ERR(cd->clk_btif))
		return PTR_ERR(cd->clk_btif);
	cd->clk_btif_dma = devm_clk_get_optional(cd->dev, "btif_dma");
	if (IS_ERR(cd->clk_btif_dma))
		return PTR_ERR(cd->clk_btif_dma);

	if (cd->clk_emi) {
		ret = clk_prepare_enable(cd->clk_emi);
		if (ret)
			return ret;
	}
	if (cd->clk_btif) {
		ret = clk_prepare_enable(cd->clk_btif);
		if (ret)
			goto err_emi;
	}
	if (cd->clk_btif_dma) {
		ret = clk_prepare_enable(cd->clk_btif_dma);
		if (ret)
			goto err_btif;
	}

	cd->timeout_ms = 100;

	return 0;

err_btif:
	clk_disable_unprepare(cd->clk_btif);
err_emi:
	clk_disable_unprepare(cd->clk_emi);
	return ret;
}

void connsys_unmap_resources(struct connsys_dev *cd)
{
	if (cd->clk_btif_dma)
		clk_disable_unprepare(cd->clk_btif_dma);
	if (cd->clk_btif)
		clk_disable_unprepare(cd->clk_btif);
	if (cd->clk_emi)
		clk_disable_unprepare(cd->clk_emi);
}

/**
 * connsys_power_on - release CONNSYS from reset and enable its clocks
 *
 * Follows the vendor sequence: clock gating, out of CPU software reset,
 * power-on configuration, SPM power domain handshake and finally the AXI
 * protection of the CONN masters.
 */
int connsys_power_on(struct connsys_dev *cd)
{
	const struct connsys_soc_data *soc = cd->soc;
	int ret;

	lockdep_assert_held(&cd->lock);

	if (cd->state > CONNSYS_POWER_OFF)
		return 0;

	/* 1. ungate the CONN top clock and enable the AP2CONN oscillator */
	connsys_rmw(cd, CONNSYS_REG_TOPCKGEN, soc->topcg_off,
		    0, soc->topcg_bit);
	connsys_rmw(cd, CONNSYS_REG_TOPCKGEN, soc->ap2conn_off,
		    0, soc->ap2conn_bit);

	/* 2. drive the AP reset generator out of reset */
	connsys_rmw(cd, CONNSYS_REG_AP_RGU, CONNSYS_CPU_SW_RST_OFF,
		    CONNSYS_CPU_SW_RST_BIT | CONSYS_CPU_SW_RST_CTRL_MASK,
		    CONNSYS_CPU_SW_RST_CTRL_KEY);

	/* 3. power-on configuration */
	connsys_wr(cd, CONNSYS_REG_SPM, CONNSYS_SPM_CONN_PWR_CON,
		   CONNSYS_PWRON_CONFG_EN_VALUE);

	/* 4. let the MCU run: leave the MBIST free-run mode */
	connsys_rmw(cd, CONNSYS_REG_MCU, CONNSYS_MCU_CFG_ACR_OFF,
		    CONNSYS_MCU_CFG_ACR_MBIST_BIT, 0);

	/* 5. SPM power-on handshake */
	connsys_rmw(cd, CONNSYS_REG_SPM, CONNSYS_SPM_CONN_PWR_CON,
		    CONNSYS_SPM_PWR_ON_CLR, CONNSYS_SPM_PWR_ON_SET);
	ret = connsys_poll(cd, CONNSYS_REG_SPM, CONNSYS_SPM_CONN_PWR_CON,
			   CONNSYS_SPM_PWR_ON_SET, CONNSYS_SPM_PWR_ON_SET,
			   cd->timeout_ms);
	if (ret) {
		dev_err(cd->dev, "CONNSYS power-on ACK timeout\n");
		return ret;
	}

	/* 6. disable the AXI protection of the CONN masters */
	connsys_rmw(cd, CONNSYS_REG_AXI, soc->axi_prot_en_off,
		    soc->axi_prot_mask, 0);
	ret = connsys_poll(cd, CONNSYS_REG_AXI, soc->axi_prot_sta_off,
			   soc->axi_prot_mask, 0, cd->timeout_ms);
	if (ret) {
		dev_err(cd->dev, "AXI protection stuck\n");
		return ret;
	}

	cd->state = CONNSYS_POWER_ON;
	dev_dbg(cd->dev, "%s powered on\n", soc->name);
	return 0;
}

/**
 * connsys_power_off - put CONNSYS back into reset and gate its clocks
 */
void connsys_power_off(struct connsys_dev *cd)
{
	const struct connsys_soc_data *soc = cd->soc;

	lockdep_assert_held(&cd->lock);

	if (cd->state == CONNSYS_POWER_OFF)
		return;

	/* keep the MCU in reset and freeze its bus */
	connsys_rmw(cd, CONNSYS_REG_AP_RGU, CONNSYS_CPU_SW_RST_OFF,
		    CONNSYS_CPU_SW_RST_BIT | CONNSYS_CPU_SW_RST_CTRL_MASK,
		    CONNSYS_CPU_SW_RST_BIT | CONNSYS_CPU_SW_RST_CTRL_KEY);
	connsys_rmw(cd, CONNSYS_REG_MCU, CONNSYS_MCU_CFG_ACR_OFF,
		    0, CONNSYS_MCU_CFG_ACR_MBIST_BIT);
	connsys_rmw(cd, CONNSYS_REG_AXI, soc->axi_prot_en_off, 0,
		    soc->axi_prot_mask);

	connsys_wr(cd, CONNSYS_REG_SPM, CONNSYS_SPM_CONN_PWR_CON,
		   CONNSYS_PWRON_CONFG_DIS_VALUE);
	connsys_rmw(cd, CONNSYS_REG_SPM, CONNSYS_SPM_CONN_PWR_CON,
		    CONNSYS_SPM_PWR_ON_SET, CONNSYS_SPM_PWR_ON_CLR);

	connsys_rmw(cd, CONNSYS_REG_TOPCKGEN, soc->topcg_off,
		    soc->topcg_bit, 0);
	connsys_rmw(cd, CONNSYS_REG_TOPCKGEN, soc->ap2conn_off,
		    soc->ap2conn_bit, 0);

	cd->state = CONNSYS_POWER_OFF;
	dev_dbg(cd->dev, "%s powered off\n", soc->name);
}

/**
 * connsys_read_chip_id - read (and sanity check) the CONNSYS chip id
 *
 * The MCU configuration block exposes a silicon id at +0x08.  With the
 * domain collapsed the bus floats, so zeros and all-ones are rejected.
 * The silicon id is *not* the same as the SoC id reported to userspace;
 * the DT tells which SoC the kernel runs on.
 */
int connsys_read_chip_id(struct connsys_dev *cd, u32 *chip_id)
{
	u32 val;

	lockdep_assert_held(&cd->lock);

	val = connsys_rd(cd, CONNSYS_REG_MCU, CONNSYS_CHIP_ID_OFF);
	if (!val || val == 0xffffffffU) {
		dev_err(cd->dev, "invalid CONNSYS chip id 0x%08x\n", val);
		return -ENODEV;
	}

	*chip_id = val;
	return 0;
}

/**
 * connsys_emi_setup - wire the reserved DRAM window to CONNSYS
 *
 * CONNSYS and the AP share a DRAM window: firmware, HIF rings and the MCU
 * log buffer live there.  An optional "memory-region" phandle selects the
 * reserved region published by the boot loader; the base is programmed into
 * both the MCU "ROM/RAM delete select" register and the topckgen EMI remap
 * register.
 */
int connsys_emi_setup(struct connsys_dev *cd)
{
	const struct connsys_soc_data *soc = cd->soc;
	struct device_node *rmem_np;
	struct reserved_mem *rmem;
	u32 base = 0, val;
	int ret;

	lockdep_assert_held(&cd->lock);

	rmem_np = of_parse_phandle(cd->dev->of_node, "memory-region", 0);
	if (rmem_np) {
		rmem = of_reserved_mem_lookup(rmem_np);
		of_node_put(rmem_np);
		if (!rmem)
			return -EPROBE_DEFER;
		base = lower_32_bits(rmem->base);
	}

	ret = of_reserved_mem_device_init(cd->dev);
	if (ret && ret != -ENODEV)
		return ret;

	connsys_wr(cd, CONNSYS_REG_MCU, CONNSYS_ROM_RAM_DELSEL_OFF, base);
	val = (base >> soc->emi_map_shift) & soc->emi_map_mask;
	connsys_rmw(cd, CONNSYS_REG_TOPCKGEN, soc->emi_map_off,
		    soc->emi_map_mask << soc->emi_map_shift,
		    val << soc->emi_map_shift);

	/* Mask the CONN2AP wakeup source where the SoC provides one. */
	if (soc->conn2ap_mask_off)
		connsys_rmw(cd, CONNSYS_REG_MCU, soc->conn2ap_mask_off,
			    0, soc->conn2ap_mask_bit);

	dev_dbg(cd->dev, "CONNSYS EMI window at 0x%08x\n", base);
	return 0;
}

int connsys_request_firmware(struct connsys_dev *cd, const char *name,
			     const struct firmware **fw)
{
	int ret;

	ret = request_firmware(fw, name, cd->dev);
	if (ret) {
		dev_err(cd->dev, "firmware %s not available: %d\n", name, ret);
		return ret;
	}
	dev_dbg(cd->dev, "firmware %s loaded (%zu bytes)\n", name, (*fw)->size);
	return 0;
}

void connsys_release_firmware(const struct firmware *fw)
{
	if (fw)
		release_firmware(fw);
}

/* ------------------------------------------------------------------ */
/* BTIF: the UART-shaped AP<->CONNSYS transport used for STP           */
/* ------------------------------------------------------------------ */

#define CONNSYS_BTIF_BASE_MASK		GENMASK_ULL(39, 12)
#define CONNSYS_BTIF_DMA_LEN		0x10
#define CONNSYS_BTIF_REG_LEN		0x100

#define BTIF_RBR_OFF			0x00	/* rx		*/
#define BTIF_THR_OFF			0x00	/* tx		*/
#define BTIF_IER_OFF			0x04
#define BTIF_IIR_OFF			0x08	/* read		*/
#define BTIF_FIFOCTRL_OFF		0x08	/* write	*/
#define BTIF_FAKELCR_OFF		0x0c
#define BTIF_LSR_OFF			0x14
#define BTIF_SLEEP_EN_OFF		0x48
#define BTIF_DMA_EN_OFF			0x4c
#define BTIF_RTOCNT_OFF			0x54
#define BTIF_TRI_LVL_OFF		0x60
#define BTIF_WAK_OFF			0x64
#define BTIF_WAT_TIME_OFF		0x68
#define BTIF_HANDSHAKE_OFF		0x6c

#define BTIF_IER_TXEEN			BIT(1)
#define BTIF_IER_RXFEN			BIT(0)

#define BTIF_FIFOCTRL_CLR_RX		BIT(1)
#define BTIF_FIFOCTRL_CLR_TX		BIT(2)

#define BTIF_LSR_DR			BIT(0)
#define BTIF_LSR_THRE			BIT(5)

#define BTIF_TRI_LVL_TX(x)		(((x) & 0xf) << 0)
#define BTIF_TRI_LVL_RX(x)		(((x) & 0x7) << 4)

#define BTIF_WAK_WAKE			BIT(0)

#define BTIF_RX_TIMEOUT_COUNT		0x10	/* chars before a rx timeout irq */

static inline u32 btif_rd(struct connsys_dev *cd, u32 off)
{
	return readl(cd->btif + off);
}

static inline void btif_wr(struct connsys_dev *cd, u32 off, u32 val)
{
	writel(val, cd->btif + off);
}

/**
 * connsys_btif_chip_reset - keep the CONNSYS side of BTIF in reset
 *
 * On the integrated parts the BTIF/STP engine belongs to CONNSYS: it is
 * placed in the "fake LCR" reset mode and its FIFOs are cleared.  The MCU
 * comes out of reset later when the launcher runs the boot script.
 */
int connsys_btif_chip_reset(struct connsys_dev *cd)
{
	lockdep_assert_held(&cd->lock);

	if (!cd->btif)
		return 0;

	btif_wr(cd, BTIF_FAKELCR_OFF, 0);
	btif_wr(cd, BTIF_FIFOCTRL_OFF,
		BTIF_FIFOCTRL_CLR_RX | BTIF_FIFOCTRL_CLR_TX);
	btif_wr(cd, BTIF_IER_OFF, 0);
	btif_wr(cd, BTIF_SLEEP_EN_OFF, 0);

	return 0;
}

/**
 * connsys_btif_open - program BTIF for 8-bit STP transfers
 *
 * RX interrupts are enabled with a one-character FIFO threshold; TX is
 * polled so that a full-size STP fragment never needs a TX interrupt.
 */
int connsys_btif_open(struct connsys_dev *cd)
{
	lockdep_assert_held(&cd->lock);

	if (!cd->btif)
		return -ENODEV;

	btif_wr(cd, BTIF_FAKELCR_OFF, 0);
	btif_wr(cd, BTIF_FIFOCTRL_OFF,
		BTIF_FIFOCTRL_CLR_RX | BTIF_FIFOCTRL_CLR_TX);
	btif_wr(cd, BTIF_RTOCNT_OFF, BTIF_RX_TIMEOUT_COUNT);
	btif_wr(cd, BTIF_TRI_LVL_OFF,
		BTIF_TRI_LVL_TX(CONNSYS_BTIF_TX_FIFO / 2) | BTIF_TRI_LVL_RX(1));
	btif_wr(cd, BTIF_WAT_TIME_OFF, 0);
	btif_wr(cd, BTIF_HANDSHAKE_OFF, 0);
	btif_wr(cd, BTIF_SLEEP_EN_OFF, 0);
	btif_wr(cd, BTIF_IER_OFF, BTIF_IER_RXFEN);

	return 0;
}

void connsys_btif_close(struct connsys_dev *cd)
{
	lockdep_assert_held(&cd->lock);

	if (!cd->btif)
		return;

	btif_wr(cd, BTIF_IER_OFF, 0);
	btif_wr(cd, BTIF_SLEEP_EN_OFF, 0);
}

/**
 * connsys_btif_tx - push a packet into the BTIF TX FIFO
 *
 * Called with cd->tx_lock held.  The THRE bit tells when the holding
 * register has room; a short spin is used while the MCU drains the FIFO.
 */
int connsys_btif_tx(struct connsys_dev *cd, const u8 *data, size_t len)
{
	size_t i;
	u32 val;

	if (!cd->btif)
		return -ENODEV;

	for (i = 0; i < len; i++) {
		int spin = 1000;

		/* wake the MCU side out of deep sleep before the burst */
		if (!i)
			btif_wr(cd, BTIF_WAK_OFF, BTIF_WAK_WAKE);

		while (spin--) {
			val = btif_rd(cd, BTIF_LSR_OFF);
			if (val & BTIF_LSR_THRE)
				break;
			udelay(1);
		}
		if (spin < 0) {
			dev_err_ratelimited(cd->dev, "BTIF TX FIFO stuck\n");
			return -EAGAIN;
		}
		btif_wr(cd, BTIF_THR_OFF, data[i]);
	}

	return 0;
}

irqreturn_t connsys_btif_isr(int irq, void *data)
{
	struct connsys_dev *cd = data;

	/* mask the RX interrupt until the work item re-arms it */
	if (cd->btif)
		btif_wr(cd, BTIF_IER_OFF, 0);
	schedule_work(&cd->rx_work);

	return IRQ_HANDLED;
}

/**
 * connsys_btif_rx - drain the BTIF RX FIFO
 */
size_t connsys_btif_rx(struct connsys_dev *cd, u8 *buf, size_t len)
{
	size_t n = 0;
	u32 val;

	if (!cd->btif)
		return 0;

	while (n < len && (val = btif_rd(cd, BTIF_LSR_OFF)) &
			  BTIF_LSR_DR)
		buf[n++] = btif_rd(cd, BTIF_RBR_OFF);

	/* reading LSR once clears the timeout condition on the FIFO */
	if (!n)
		btif_rd(cd, BTIF_IIR_OFF);

	if (n < len)
		btif_wr(cd, BTIF_IER_OFF, BTIF_IER_RXFEN);

	return n;
}


