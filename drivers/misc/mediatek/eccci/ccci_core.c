// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MD1 cellular modem (ECCCI) - core.
 *
 * This file owns the modem life cycle:
 *
 *   probe        map the CLDMA/CCIF windows, get the clocks and the IRQs,
 *                reserve the modem DRAM, create the character ports and the
 *                ccmni interfaces and publish the device.
 *   power on     make sure the reserved DRAM really is reserved, request
 *                "modem_1_wg_n.img" and "dsp_1_wg_n.bin", program the MD
 *                boot vector, release the MD reset and start CLDMA.
 *   boot wait    the modem handshakes on the CCCI control channel; the
 *                legacy "md_init" monitor protocol used by the M5c, M3 Note
 *                and MX6 basebands is covered by the CCIF SRAM poller.
 *   assert       flush every port, reload the firmware and restart.
 *   power off    stop CLDMA, hold the MD in reset, drop the firmware.
 *
 * Every state change runs on the ordered workqueue @wq, so the state machine
 * is strictly serialised; the ports only ever read @md->state.
 */
#include <linux/cdev.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_reserved_mem.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "ccci_priv.h"
#include "ccci_cldma_reg.h"

#define CCCI_FW_NAME		"modem_1_wg_n.img"
#define CCCI_FW_DSP_NAME	"dsp_1_wg_n.bin"

/* Size of the shared memory window the SMEM ioctls report. */
#define CCCI_SMEM_MIN_SIZE	0x10000

/* Modem image header, identical to the vendor struct md_check_header_v3. */
#define MD_IMG_MAGIC		"CHECK_HEADER"
#define MD_IMG_HDR_SIZE		256

static LIST_HEAD(ccci_modem_list);
static DEFINE_MUTEX(ccci_modem_list_lock);

/* ---------------------------------------------------------------------- */
/* Modem lookup                                                           */
/* ---------------------------------------------------------------------- */

struct ccci_modem *ccci_modem_get(int md_id)
{
	struct ccci_modem *md;

	mutex_lock(&ccci_modem_list_lock);
	list_for_each_entry(md, &ccci_modem_list, modem_entry) {
		if (md->index == md_id) {
			mutex_unlock(&ccci_modem_list_lock);
			return md;
		}
	}
	mutex_unlock(&ccci_modem_list_lock);

	return NULL;
}
EXPORT_SYMBOL_GPL(ccci_modem_get);

const char *ccci_md_state_name(enum ccci_md_state state)
{
	static const char *const names[] = {
		[CCCI_STATE_INVALID] = "invalid",
		[CCCI_STATE_GATED] = "gated",
		[CCCI_STATE_BOOT_WAITING_FOR_HS1] = "waiting_hs1",
		[CCCI_STATE_BOOT_WAITING_FOR_HS2] = "waiting_hs2",
		[CCCI_STATE_READY] = "ready",
		[CCCI_STATE_EXCEPTION] = "exception",
		[CCCI_STATE_RESET] = "reset",
	};

	if (state > CCCI_STATE_RESET)
		return "unknown";
	return names[state];
}
EXPORT_SYMBOL_GPL(ccci_md_state_name);

enum ccci_md_state ccci_md_get_state(struct ccci_modem *md)
{
	return READ_ONCE(md->state);
}
EXPORT_SYMBOL_GPL(ccci_md_get_state);

static void ccci_set_state(struct ccci_modem *md, enum ccci_md_state state)
{
	enum ccci_md_state old = READ_ONCE(md->state);

	if (old == state)
		return;

	ccci_info(md, "state %s -> %s\n", ccci_md_state_name(old),
		  ccci_md_state_name(state));
	WRITE_ONCE(md->state, state);

	/* The network carriers follow the modem. */
	ccci_ccmni_set_carrier(md, state == CCCI_STATE_READY);
}

void ccci_broadcast_state(struct ccci_modem *md, enum ccci_md_state state)
{
	ccci_port_broadcast_state(md, state);
}

/* ---------------------------------------------------------------------- */
/* Reserved memory                                                        */
/* ---------------------------------------------------------------------- */

static struct reserved_mem *ccci_find_reserved(struct ccci_modem *md,
					       struct device_node *np,
					       const char *const *names,
					       const char *const *compat)
{
	struct device_node *child;
	struct reserved_mem *rmem;
	int i;

	for_each_child_of_node(np, child) {
		for (i = 0; names[i]; i++) {
			if (!strstarts(child->name, names[i]))
				continue;
			rmem = of_reserved_mem_lookup(child);
			if (!rmem) {
				dev_err(md->dev,
					"reserved memory '%s' has no reg/size\n",
					child->name);
				of_node_put(child);
				return NULL;
			}
			of_node_put(child);
			return rmem;
		}
		for (i = 0; compat[i]; i++) {
			if (!of_device_is_compatible(child, compat[i]))
				continue;
			rmem = of_reserved_mem_lookup(child);
			if (!rmem) {
				dev_err(md->dev,
					"reserved memory '%s' has no reg/size\n",
					child->name);
				of_node_put(child);
				return NULL;
			}
			of_node_put(child);
			return rmem;
		}
	}

	return NULL;
}

static int ccci_check_reserved(struct ccci_modem *md, struct reserved_mem *rmem,
			       const char *what, resource_size_t min_size)
{
	if (!rmem) {
		ccci_err(md, "reserved memory '%s' is missing from the device tree\n",
			 what);
		return -ENOENT;
	}
	if (rmem->size < min_size) {
		ccci_err(md, "reserved memory '%s' is 0x%llx bytes, need 0x%llx\n",
			 what, (u64)rmem->size, (u64)min_size);
		return -EINVAL;
	}
	return 0;
}

/*
 * The modem DRAM must never reach the page allocator: the MD core and the AP
 * both write into it, and the MD MPU configuration is derived from it.
 */
static int ccci_setup_memory(struct ccci_modem *md, struct device_node *np)
{
	static const char *const md_names[] = {
		"modem-md1", "reserve-memory-ccci_md1", NULL,
	};
	static const char *const md_compat[] = {
		"mediatek,reserve-memory-ccci_md1", NULL,
	};
	static const char *const smem_names[] = {
		"modem-share", "reserve-memory-ccci_share", NULL,
	};
	static const char *const smem_compat[] = {
		"mediatek,reserve-memory-ccci_share", NULL,
	};
	struct reserved_mem *smem_rmem, *md_rmem;
	int ret;

	md_rmem = ccci_find_reserved(md, np, md_names, md_compat);
	ret = ccci_check_reserved(md, md_rmem, "modem-md1", 0x100000);
	if (ret)
		return ret;

	/*
	 * The shared window is optional: a board that never had a modem may
	 * reclaim it, and the SMEM ioctls then report a zero length instead
	 * of handing out unmapped memory.
	 */
	smem_rmem = ccci_find_reserved(md, np, smem_names, smem_compat);
	if (smem_rmem) {
		ret = ccci_check_reserved(md, smem_rmem, "modem-share",
					  CCCI_SMEM_MIN_SIZE);
		if (ret)
			return ret;
		md->hw.smem_base = smem_rmem->base;
		md->hw.smem_len = smem_rmem->size;
	}

	md->hw.md_base = md_rmem->base;
	md->hw.md_size = md_rmem->size;

	ccci_info(md, "modem DRAM 0x%llx@0x%llx, shared window 0x%llx@0x%llx\n",
		  (u64)md->hw.md_size, (u64)md->hw.md_base,
		  (u64)md->hw.smem_len, (u64)md->hw.smem_base);

	return 0;
}

/* ---------------------------------------------------------------------- */
/* Device tree                                                            */
/* ---------------------------------------------------------------------- */

static int ccci_parse_dt(struct ccci_modem *md, struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct resource *res;
	u32 val;
	int i, nreg, nirq;

	nreg = of_property_count_elems_of_size(np, "reg", sizeof(u32) * 4);
	if (nreg < CCCI_REG_MAX) {
		ccci_err(md, "need %d register windows, the device tree has %d\n",
			 CCCI_REG_MAX, max(nreg, 0));
		return -EINVAL;
	}

	for (i = 0; i < CCCI_REG_MAX; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res) {
			ccci_err(md, "register window %d missing\n", i);
			return -EINVAL;
		}
		if (i <= CCCI_REG_CLDMA_AP_PDN)
			md->hw.cldma_len = resource_size(res);
		else if (i >= CCCI_REG_CCIF_AP)
			md->hw.ccif_len = resource_size(res);
	}

	md->hw.cldma_ap_ao = platform_get_resource(pdev, IORESOURCE_MEM,
						   CCCI_REG_CLDMA_AP_AO)->start;
	md->hw.cldma_md_ao = platform_get_resource(pdev, IORESOURCE_MEM,
						   CCCI_REG_CLDMA_MD_AO)->start;
	md->hw.cldma_ap_pdn = platform_get_resource(pdev, IORESOURCE_MEM,
						    CCCI_REG_CLDMA_AP_PDN)->start;
	md->hw.cldma_md_pdn = platform_get_resource(pdev, IORESOURCE_MEM,
						    CCCI_REG_CLDMA_MD_PDN)->start;
	md->hw.ccif_ap = platform_get_resource(pdev, IORESOURCE_MEM,
					       CCCI_REG_CCIF_AP)->start;
	md->hw.ccif_md = platform_get_resource(pdev, IORESOURCE_MEM,
					       CCCI_REG_CCIF_MD)->start;

	ccci_dbg(md, "CLDMA 0x%llx/0x%llx (PD 0x%llx/0x%llx), CCIF 0x%llx/0x%llx\n",
		 (u64)md->hw.cldma_ap_ao, (u64)md->hw.cldma_md_ao,
		 (u64)md->hw.cldma_ap_pdn, (u64)md->hw.cldma_md_pdn,
		 (u64)md->hw.ccif_ap, (u64)md->hw.ccif_md);

	nirq = platform_irq_count(pdev);
	if (nirq < CCCI_IRQ_CCIF + 1) {
		ccci_err(md, "need at least %d interrupts, the device tree has %d\n",
			 CCCI_IRQ_CCIF + 1, nirq);
		return -EINVAL;
	}
	for (i = 0; i < CCCI_IRQ_MAX; i++) {
		if (i >= nirq) {
			md->irq[i] = -1;
			continue;
		}
		md->irq[i] = platform_get_irq(pdev, i);
		if (md->irq[i] == -EPROBE_DEFER)
			return -EPROBE_DEFER;
	}

	if (of_property_read_u32(np, "mediatek,md_id", &val))
		val = 0;
	else
		val &= 0xff;
	if (val != 0) {
		ccci_err(md, "only MD1 is supported, the node asks for MD%d\n",
			 val + 1);
		return -EINVAL;
	}
	md->hw.md_id = val;

	if (of_property_read_u32(np, "mediatek,cldma_capability", &val))
		md->hw.capability = md->pdata->cldma.capability;
	else
		md->hw.capability = val;

	if (!of_property_read_u32(np, "mediatek,md_smem_size", &val))
		md->hw.smem_size = max_t(u32, val, CCCI_SMEM_MIN_SIZE);

	return ccci_setup_memory(md, np);
}

/* ---------------------------------------------------------------------- */
/* Firmware                                                               */
/* ---------------------------------------------------------------------- */

/*
 * The vendor image starts with a 256 byte "CHECK_HEADER" header. A blob
 * without the magic is still accepted (some boards pre-flash the image) but
 * it is reported, since loading garbage into the MD is a guaranteed assert.
 */
static bool ccci_validate_image(struct ccci_modem *md,
				const struct firmware *fw, const char *name)
{
	if (!fw || !fw->size)
		return false;

	if (fw->size < MD_IMG_HDR_SIZE) {
		ccci_err(md, "%s is too small (%zu bytes)\n", name, fw->size);
		return false;
	}

	if (memcmp(fw->data, MD_IMG_MAGIC, sizeof(MD_IMG_MAGIC) - 1)) {
		ccci_warn(md, "%s has no CHECK_HEADER magic, using it as is\n",
			  name);
		return true;
	}

	ccci_dbg(md, "%s: header %02x%02x%02x%02x, %zu bytes\n", name,
		 fw->data[12], fw->data[13], fw->data[14], fw->data[15],
		 fw->size);
	return true;
}

static int ccci_request_firmware(struct ccci_modem *md)
{
	const struct firmware *fw;
	int ret;

	ret = firmware_request_nowarn(&fw, CCCI_FW_NAME, md->dev);
	if (ret) {
		ccci_err(md, "cannot load %s (%d)\n", CCCI_FW_NAME, ret);
		return ret;
	}
	if (!ccci_validate_image(md, fw, CCCI_FW_NAME)) {
		release_firmware(fw);
		return -EINVAL;
	}
	md->fw = fw;

	ret = firmware_request_nowarn(&fw, CCCI_FW_DSP_NAME, md->dev);
	if (ret) {
		/*
		 * The DSP image is only used by basebands that run the audio
		 * DSP inside MD1; its absence is not fatal, the modem image
		 * header says whether one is embedded in it.
		 */
		ccci_info(md, "%s not available (%d), continuing without it\n",
			  CCCI_FW_DSP_NAME, ret);
		return 0;
	}
	if (!ccci_validate_image(md, fw, CCCI_FW_DSP_NAME)) {
		release_firmware(fw);
		return 0;
	}
	md->fw_dsp = fw;

	return 0;
}

static void ccci_release_firmware(struct ccci_modem *md)
{
	release_firmware(md->fw);
	release_firmware(md->fw_dsp);
	md->fw = NULL;
	md->fw_dsp = NULL;
}

/* ---------------------------------------------------------------------- */
/* Hardware helpers                                                       */
/* ---------------------------------------------------------------------- */

static void ccci_wr(struct ccci_modem *md, enum ccci_reg_index idx, u32 off,
		    u32 val)
{
	iowrite32(val, md->base[idx] + off);
}

static void ccci_md_let_go(struct ccci_modem *md)
{
	const struct ccci_platform_data *pdata = md->pdata;

	/*
	 * The MD core powers up paused on its boot vector. The key register
	 * must be seeded first, then the vector ("start of the MD address
	 * window") and finally the enable register: the ROM samples all
	 * three when it leaves reset.
	 */
	if (!pdata->slave_boot)
		return;

	ccci_dbg(md, "MD boot vector handshake\n");
	ccci_wr(md, CCCI_REG_CLDMA_AP_AO, pdata->boot_vector_key,
		pdata->boot_vector_key_value);
	ccci_wr(md, CCCI_REG_CLDMA_AP_AO, pdata->boot_vector,
		pdata->boot_vector_value);
	ccci_wr(md, CCCI_REG_CLDMA_AP_AO, pdata->boot_vector_en,
		pdata->boot_vector_en_value);
	udelay(10);
}

static void ccci_cldma_hw_release(struct ccci_modem *md)
{
	const struct ccci_platform_data *pdata = md->pdata;
	u32 val;

	/*
	 * CLDMA is reset by the INFRA reset controller whose CLDMA bits are
	 * reachable from the CLDMA control register: the reset field is
	 * write-one-to-clear, so writing 0b10 to the field releases it.
	 */
	if (!pdata->cldma_ao_rst_mask && !pdata->cldma_pd_rst_mask)
		return;

	if (pdata->cldma_ao_rst_mask) {
		val = pdata->cldma_ao_rst_mask << 1;
		ccci_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_MD_CTRL, val);
		ccci_wr(md, CCCI_REG_CLDMA_MD_AO, CLDMA_MD_CTRL, val);
	}
	if (pdata->cldma_pd_rst_mask) {
		val = pdata->cldma_pd_rst_mask << 1;
		ccci_wr(md, CCCI_REG_CLDMA_AP_PDN, CLDMA_MD_CTRL, val);
		ccci_wr(md, CCCI_REG_CLDMA_MD_PDN, CLDMA_MD_CTRL, val);
	}

	/* Two IP cycles are enough for the release to reach the flops. */
	udelay(2);
}

/* ---------------------------------------------------------------------- */
/* Power on / off                                                         */
/* ---------------------------------------------------------------------- */

/*
 * ccci_power_on() - boot the modem
 * @md: modem
 * @cold: drop the firmware to force a reload (reset path)
 *
 * The caller must hold @md->state_lock.
 */
static int ccci_power_on(struct ccci_modem *md, bool cold)
{
	int ret;

	if (md->powered)
		return 0;

	if (cold || !md->fw) {
		ccci_release_firmware(md);
		ret = ccci_request_firmware(md);
		if (ret)
			return ret;
	}

	ret = clk_bulk_prepare_enable(md->nr_clks, md->clks);
	if (ret) {
		ccci_err(md, "cannot enable the CCIF/CLDMA clocks (%d)\n", ret);
		goto err_fw;
	}

	ccci_cldma_hw_release(md);

	ret = ccci_cldma_start(md);
	if (ret) {
		ccci_err(md, "CLDMA start failed (%d)\n", ret);
		goto err_clk;
	}

	ccci_md_let_go(md);

	md->hw.ccif_sram = md->base[CCCI_REG_CCIF_AP] + CCIF_CHDATA;

	/*
	 * A fresh boot restarts the per channel sequence numbering and the
	 * boot handshake.
	 */
	memset(md->seq_num, 0, sizeof(md->seq_num));
	reinit_completion(&md->handshake_wait);

	/* Serialise against a stray timer callback that is still running. */
	timer_delete_sync(&md->mdtimeout_timer);
	md->boot_stage = CCCI_MD_BOOT_STAGE_1;
	md->powered = true;

	mod_timer(&md->mdtimeout_timer,
		  jiffies + msecs_to_jiffies(CCCI_MD_BOOT_TIMEOUT_MS));
	ccci_set_state(md, CCCI_STATE_BOOT_WAITING_FOR_HS1);

	ccci_info(md, "powered on, %zu byte modem image\n",
		  md->fw ? md->fw->size : 0);
	return 0;

err_clk:
	clk_bulk_disable_unprepare(md->nr_clks, md->clks);
err_fw:
	ccci_release_firmware(md);
	return ret;
}

/*
 * ccci_power_off_locked() - stop the modem
 * @md: modem
 * @reason: human readable cause, for the log
 *
 * The caller must hold @md->state_lock.
 */
static void ccci_power_off_locked(struct ccci_modem *md, const char *reason)
{
	timer_delete_sync(&md->mdtimeout_timer);

	if (!md->powered) {
		ccci_set_state(md, CCCI_STATE_GATED);
		return;
	}

	ccci_set_state(md, CCCI_STATE_RESET);
	ccci_cldma_stop(md);
	clk_bulk_disable_unprepare(md->nr_clks, md->clks);
	ccci_release_firmware(md);

	md->powered = false;
	md->hw.ccif_sram = NULL;
	md->boot_stage = CCCI_MD_BOOT_STAGE_0;
	ccci_set_state(md, CCCI_STATE_GATED);
	ccci_info(md, "powered off (%s)\n", reason);
}

/* ---------------------------------------------------------------------- */
/* Receive path                                                           */
/* ---------------------------------------------------------------------- */

static void ccci_handle_assert(struct ccci_modem *md, struct sk_buff *skb)
{
	struct ccci_msg *hdr = (struct ccci_msg *)skb->data;

	ccci_err(md, "modem assert on channel %d, data %08x %08x\n",
		 CCCI_HDR_CHANNEL(hdr), le32_to_cpu(hdr->data[0]),
		 le32_to_cpu(hdr->data[1]));
	ccci_cldma_dump(md, "modem assert");

	md->ex_stage = CCCI_MD_BOOT_STAGE_2;
	if (!work_pending(&md->boot_work))
		queue_work(md->wq, &md->boot_work);
}

static void ccci_boot_complete(struct ccci_modem *md, const char *how)
{
	timer_delete_sync(&md->mdtimeout_timer);

	mutex_lock(&md->state_lock);
	if (READ_ONCE(md->state) == CCCI_STATE_READY) {
		mutex_unlock(&md->state_lock);
		return;
	}
	if (!md->powered) {
		mutex_unlock(&md->state_lock);
		return;
	}
	md->boot_stage = CCCI_MD_BOOT_STAGE_3;
	md->ex_stage = 0;
	md->boot_count++;
	ccci_set_state(md, CCCI_STATE_READY);
	mutex_unlock(&md->state_lock);

	ccci_broadcast_state(md, CCCI_STATE_READY);
	ccci_info(md, "modem booted (%s)\n", how);
}

/*
 * ccci_handle_handshake() - the modem reported a valid runtime packet
 * @md: modem
 * @skb: first packet received on a control or service channel
 */
static void ccci_handle_handshake(struct ccci_modem *md, struct sk_buff *skb)
{
	struct ccci_msg *hdr = (struct ccci_msg *)skb->data;

	ccci_dbg(md, "handshake on channel %d, data %08x\n",
		 CCCI_HDR_CHANNEL(hdr), le32_to_cpu(hdr->data[0]));

	if (!READ_ONCE(md->powered) ||
	    READ_ONCE(md->boot_stage) == CCCI_MD_BOOT_STAGE_0)
		return;

	if (md->boot_stage == CCCI_MD_BOOT_STAGE_1) {
		md->boot_stage = CCCI_MD_BOOT_STAGE_2;
		if (READ_ONCE(md->state) == CCCI_STATE_BOOT_WAITING_FOR_HS1)
			ccci_set_state(md, CCCI_STATE_BOOT_WAITING_FOR_HS2);
		complete(&md->handshake_wait);
		/*
		 * The legacy basebands (M5c, M3 Note, MX6 class) send a single
		 * boot stage message and then start serving channels; the
		 * newer ones follow with the runtime data exchange. Either way
		 * a valid packet on the control or system channel means the
		 * image is running and the channels can be used.
		 */
		ccci_boot_complete(md, "handshake");
	}
}

/**
 * ccci_recv_packet() - route one packet received from the modem
 * @md: modem
 * @skb: packet starting with a full struct ccci_msg
 *
 * The layout is validated here once so that the ports can trust the channel
 * and length fields. The caller transfers ownership of @skb.
 */
void ccci_recv_packet(struct ccci_modem *md, struct sk_buff *skb)
{
	struct ccci_msg *hdr = (struct ccci_msg *)skb->data;
	unsigned int ch, len;
	struct ccci_port *port;

	if (skb->len < CCCI_HDR_SIZE) {
		ccci_err(md, "short packet from the modem (%u bytes)\n",
			 skb->len);
		goto drop;
	}

	ch = CCCI_HDR_CHANNEL(hdr);
	len = CCCI_HDR_LEN(hdr);

	if (CCCI_HDR_ASSERT(hdr)) {
		ccci_handle_assert(md, skb);
		goto drop;
	}

	if (ch >= CCCI_MAX_CH_NUM) {
		ccci_warn(md, "packet on unknown channel %u\n", ch);
		goto drop;
	}

	/*
	 * CLDMA strips the padding of its fixed size buffers, so the length
	 * in the header must match the descriptor. A mismatch means a stale
	 * header: trust the descriptor and repair the copy.
	 */
	if (len != skb->len) {
		ccci_dbg(md, "channel %u length mismatch (%u vs %u)\n", ch, len,
			 skb->len);
		CCCI_HDR_SET_LEN(hdr, skb->len);
	}

	atomic_inc(&md->traffic_cnt);

	/*
	 * The control and system channels are how the modem announces that
	 * the firmware came up: the first valid packet completes the boot.
	 */
	if ((ch == CCCI_CONTROL_RX || ch == CCCI_SYSTEM_RX) &&
	    READ_ONCE(md->state) != CCCI_STATE_READY)
		ccci_handle_handshake(md, skb);

	if (ccci_ch_is_net(ch) && ccci_ccmni_rx(md, skb))
		return;

	mutex_lock(&md->port_lock);
	port = md->ports[ch];
	mutex_unlock(&md->port_lock);

	if (!port) {
		ccci_dbg(md, "no port for channel %u, dropping %u bytes\n", ch,
			 len);
		goto drop;
	}

	ccci_port_recv(port, skb);
	return;

drop:
	kfree_skb(skb);
}

/**
 * ccci_send_packet() - transmit one CCCI packet
 * @md: modem
 * @ch: logical channel, selects the CLDMA queue
 * @skb: packet, ownership is transferred even on failure
 */
int ccci_send_packet(struct ccci_modem *md, int ch, struct sk_buff *skb)
{
	struct ccci_msg *hdr;
	u16 seq;
	int ret;

	if (ch < 0 || ch >= CCCI_MAX_CH_NUM) {
		kfree_skb(skb);
		return -CCCI_ERR_CHANNEL_NUM_MIS_MATCH;
	}

	if (READ_ONCE(md->state) != CCCI_STATE_READY &&
	    READ_ONCE(md->state) != CCCI_STATE_BOOT_WAITING_FOR_HS2) {
		kfree_skb(skb);
		if (!READ_ONCE(md->powered))
			return -CCCI_ERR_HIF_NOT_POWER_ON;
		return -CCCI_ERR_MD_IN_RESET;
	}

	hdr = (struct ccci_msg *)skb->data;
	if (skb->len != CCCI_HDR_LEN(hdr)) {
		kfree_skb(skb);
		return -CCCI_ERR_INVALID_PARAM;
	}

	seq = READ_ONCE(md->seq_num[ch]);
	md->seq_num[ch] = (seq + 1) & 0x7fff;
	hdr->seq_num = cpu_to_le16(seq);
	/* data[0] mirrors what the modem puts there: its own channel id. */
	hdr->data[0] = cpu_to_le32(CCCI_MONITOR_CH_ID);

	ret = ccci_cldma_send_packet(md, ch, skb);
	if (ret)
		return ret;

	md->ch_pkt_cnt[ch]++;
	return 0;
}

/**
 * ccci_send_control_msg() - build and send a single packet message
 * @md: modem
 * @ch: destination channel
 * @data: payload
 * @len: payload length
 */
int ccci_send_control_msg(struct ccci_modem *md, int ch, const void *data,
			  size_t len)
{
	struct sk_buff *skb;
	struct ccci_msg *hdr;

	if (!md || len > CCCI_MTU)
		return -EINVAL;

	skb = ccci_alloc_skb(CCCI_HDR_SIZE + len, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	hdr = (struct ccci_msg *)skb->data;
	ccci_header_init(hdr, ch, CCCI_HDR_SIZE + len, len, 0);
	if (len)
		memcpy(skb->data + CCCI_HDR_SIZE, data, len);

	return ccci_send_packet(md, ch, skb);
}

/* ---------------------------------------------------------------------- */
/* Boot / assert state machine                                            */
/* ---------------------------------------------------------------------- */

static void ccci_boot_work(struct work_struct *work)
{
	struct ccci_modem *md = container_of(work, struct ccci_modem,
					     boot_work);
	unsigned long timeout;
	enum ccci_md_state state;
	int ret;

	mutex_lock(&md->state_lock);
	state = READ_ONCE(md->state);

	switch (state) {
	case CCCI_STATE_GATED:
		ret = ccci_power_on(md, true);
		if (ret)
			ccci_set_state(md, CCCI_STATE_GATED);
		break;
	case CCCI_STATE_EXCEPTION:
		/* The modem or the DMA engine died: reload and restart. */
		ccci_power_off_locked(md, "modem exception");
		ret = ccci_power_on(md, true);
		if (ret)
			ccci_set_state(md, CCCI_STATE_GATED);
		break;
	default:
		/* already booting or ready: nothing to do */
		break;
	}
	state = READ_ONCE(md->state);
	mutex_unlock(&md->state_lock);

	if (state != CCCI_STATE_BOOT_WAITING_FOR_HS1 &&
	    state != CCCI_STATE_BOOT_WAITING_FOR_HS2)
		return;

	/*
	 * Wait for the handshake without holding the state lock: the receive
	 * path takes it when it completes the handshake.
	 */
	timeout = wait_for_completion_timeout(&md->handshake_wait,
					      msecs_to_jiffies
					      (CCCI_MD_BOOT_TIMEOUT_MS));
	if (timeout)
		return;

	ccci_err(md, "modem did not handshake within %d ms\n",
		 CCCI_MD_BOOT_TIMEOUT_MS);

	mutex_lock(&md->state_lock);
	if (READ_ONCE(md->state) == CCCI_STATE_BOOT_WAITING_FOR_HS1 ||
	    READ_ONCE(md->state) == CCCI_STATE_BOOT_WAITING_FOR_HS2)
		ccci_power_off_locked(md, "boot timeout");
	mutex_unlock(&md->state_lock);
}

/*
 * ccci_poll_work() - watch the boot progress and the modem watchdog
 *
 * Basebands that do not send a handshake packet still fill the CCIF shared
 * SRAM once the image runs; a non zero signature there is enough to declare
 * the modem booted, which is the legacy "md_init" monitor protocol.
 */
static void ccci_poll_work(struct work_struct *work)
{
	struct ccci_modem *md = container_of(to_delayed_work(work),
					     struct ccci_modem, poll_work);
	enum ccci_md_state state = READ_ONCE(md->state);
	void __iomem *sram = md->hw.ccif_sram;

	if (sram && (state == CCCI_STATE_BOOT_WAITING_FOR_HS1 ||
		     state == CCCI_STATE_BOOT_WAITING_FOR_HS2)) {
		u32 sig = ioread32(sram);

		if (sig) {
			ccci_info(md, "boot signature %08x in the CCIF SRAM\n",
				  sig);
			ccci_boot_complete(md, "CCIF SRAM signature");
		}
	}

	mod_delayed_work(system_wq, &md->poll_work,
			 msecs_to_jiffies(CCCI_MD_WDT_POLL_MS));
}

static void ccci_mdtimeout_timer(struct timer_list *t)
{
	struct ccci_modem *md = container_of(t, struct ccci_modem,
					     mdtimeout_timer);

	ccci_err(md, "modem boot timer expired\n");
	queue_work(md->wq, &md->boot_work);
}

/* ---------------------------------------------------------------------- */
/* Public life cycle API                                                  */
/* ---------------------------------------------------------------------- */

int ccci_md_power_on(struct ccci_modem *md)
{
	if (READ_ONCE(md->state) == CCCI_STATE_READY)
		return 0;

	queue_work(md->wq, &md->boot_work);
	return 0;
}
EXPORT_SYMBOL_GPL(ccci_md_power_on);

int ccci_md_power_off(struct ccci_modem *md, const char *reason)
{
	cancel_work_sync(&md->boot_work);

	mutex_lock(&md->state_lock);
	ccci_power_off_locked(md, reason);
	mutex_unlock(&md->state_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(ccci_md_power_off);

int ccci_md_reset(struct ccci_modem *md, const char *reason)
{
	ccci_info(md, "reset requested (%s)\n", reason ?: "unspecified");

	if (READ_ONCE(md->powered))
		WRITE_ONCE(md->state, CCCI_STATE_EXCEPTION);
	queue_work(md->wq, &md->boot_work);
	return 0;
}
EXPORT_SYMBOL_GPL(ccci_md_reset);

/* ---------------------------------------------------------------------- */
/* debugfs                                                                */
/* ---------------------------------------------------------------------- */

static ssize_t ccci_debug_state_show(struct file *file, char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	struct ccci_modem *md = file->private_data;
	char buf[512];
	int len;

	len = scnprintf(buf, sizeof(buf),
			"state: %s\nboot_stage: %u\nex_stage: %u\nboot_count: %u\n"
			"powered: %d\nflight: %d\ncapability: 0x%08x\n"
			"firmware: %zu bytes\nshared window: 0x%llx\n"
			"traffic: %d\n",
			ccci_md_state_name(READ_ONCE(md->state)),
			md->boot_stage, md->ex_stage, md->boot_count,
			md->powered, md->flown, md->capability,
			md->fw ? md->fw->size : 0,
			(u64)md->hw.smem_len, atomic_read(&md->traffic_cnt));

	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static ssize_t ccci_debug_state_write(struct file *file,
				      const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	struct ccci_modem *md = file->private_data;
	char cmd[16];

	if (!count || count >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;
	cmd[count] = '\0';
	strim(cmd);

	if (!strcmp(cmd, "reset"))
		ccci_md_reset(md, "debugfs");
	else if (!strcmp(cmd, "off"))
		ccci_md_power_off(md, "debugfs");
	else if (!strcmp(cmd, "on"))
		ccci_md_power_on(md);
	else
		return -EINVAL;

	return count;
}

static const struct file_operations ccci_debug_state_fops = {
	.open = simple_open,
	.read = ccci_debug_state_show,
	.write = ccci_debug_state_write,
	.llseek = generic_file_llseek,
};

static void ccci_debugfs_init(struct ccci_modem *md)
{
	md->debug_root = debugfs_create_dir("md1", NULL);
	if (IS_ERR_OR_NULL(md->debug_root)) {
		md->debug_root = NULL;
		return;
	}
	debugfs_create_file("state", 0600, md->debug_root, md,
			    &ccci_debug_state_fops);
}

/* ---------------------------------------------------------------------- */
/* Probe / remove                                                        */
/* ---------------------------------------------------------------------- */

static const struct ccci_platform_data *ccci_match_pdata(struct device *dev)
{
	if (of_device_is_compatible(dev->of_node, "mediatek,mt6797-mdcldma"))
		return &ccci_mt6797_data;
	if (of_device_is_compatible(dev->of_node, "mediatek,mt6755-mdcldma"))
		return &ccci_mt6755_data;
	if (of_device_is_compatible(dev->of_node, "mediatek,mt6735-mdcldma"))
		return &ccci_mt6735_data;

	/* Boards that still use the vendor compatible string. */
	if (of_device_is_compatible(dev->of_node, "mediatek,mdcldma")) {
		if (of_device_is_compatible(dev->of_node, "mediatek,mt6735") ||
		    of_device_is_compatible(dev->of_node, "mediatek,mt6737m"))
			return &ccci_mt6735_data;
		if (of_device_is_compatible(dev->of_node, "mediatek,mt6797"))
			return &ccci_mt6797_data;
		return &ccci_mt6755_data;
	}

	return NULL;
}

static const struct of_device_id ccci_match_table[] = {
	{ .compatible = "mediatek,mt6735-mdcldma" },
	{ .compatible = "mediatek,mt6755-mdcldma" },
	{ .compatible = "mediatek,mt6797-mdcldma" },
	{ .compatible = "mediatek,mdcldma" },
	{ }
};
MODULE_DEVICE_TABLE(of, ccci_match_table);

static void ccci_destroy_wq(void *data)
{
	struct ccci_modem *md = data;

	destroy_workqueue(md->wq);
}

static void ccci_cldma_deinit_action(void *data)
{
	ccci_cldma_deinit(data);
}

static void ccci_char_deinit_action(void *data)
{
	ccci_char_deinit(data);
}

static void ccci_ccmni_deinit_action(void *data)
{
	ccci_ccmni_deinit(data);
}

static void ccci_unregister_modem(void *data)
{
	struct ccci_modem *md = data;

	mutex_lock(&ccci_modem_list_lock);
	if (!list_empty(&md->modem_entry))
		list_del_init(&md->modem_entry);
	mutex_unlock(&ccci_modem_list_lock);
}

static void ccci_pm_disable(void *data)
{
	pm_runtime_disable(data);
}

static int ccci_probe(struct platform_device *pdev)
{
	static const char *const clk_names[] = {
		"ccif-ap", "ccif-md", "cldma",
	};
	struct device *dev = &pdev->dev;
	struct ccci_modem *md;
	int i, ret;

	md = devm_kzalloc(dev, sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;

	md->dev = dev;
	md->index = 0;
	md->state = CCCI_STATE_GATED;
	mutex_init(&md->port_lock);
	mutex_init(&md->state_lock);
	init_completion(&md->handshake_wait);
	atomic_set(&md->traffic_cnt, 0);
	for (i = 0; i < CCCI_CRITICAL_USERS; i++)
		atomic_set(&md->critical_user_active[i], 0);
	INIT_LIST_HEAD(&md->modem_entry);
	timer_setup(&md->mdtimeout_timer, ccci_mdtimeout_timer, 0);

	md->pdata = ccci_match_pdata(dev);
	if (!md->pdata) {
		dev_err(dev, "no SoC description for %pOF\n", dev->of_node);
		return -ENODEV;
	}

	ret = ccci_parse_dt(md, pdev);
	if (ret)
		return ret;

	md->capability = md->hw.capability;

	/* The CLDMA rings are coherent structures on a 32 bit DMA bus. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "cannot set the 32 bit DMA mask (%d)\n", ret);
		return ret;
	}

	for (i = 0; i < CCCI_REG_MAX; i++) {
		md->base[i] = devm_ioremap_resource(dev,
				platform_get_resource(pdev, IORESOURCE_MEM, i));
		if (IS_ERR(md->base[i])) {
			dev_err(dev, "cannot map register window %d\n", i);
			return PTR_ERR(md->base[i]);
		}
	}

	md->clks = devm_kcalloc(dev, ARRAY_SIZE(clk_names),
				sizeof(*md->clks), GFP_KERNEL);
	if (!md->clks)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(clk_names); i++)
		md->clks[i].id = clk_names[i];
	ret = devm_clk_bulk_get_optional(dev, ARRAY_SIZE(clk_names),
					 md->clks);
	if (ret) {
		dev_err(dev, "cannot get the transport clocks (%d)\n", ret);
		return ret;
	}
	md->nr_clks = ARRAY_SIZE(clk_names);

	md->wq = alloc_ordered_workqueue("ccci-md%d", WQ_MEM_RECLAIM,
					 md->index + 1);
	if (!md->wq)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, ccci_destroy_wq, md);
	if (ret)
		return ret;

	INIT_WORK(&md->boot_work, ccci_boot_work);
	INIT_DELAYED_WORK(&md->poll_work, ccci_poll_work);

	md->cldma = devm_kzalloc(dev, sizeof(*md->cldma), GFP_KERNEL);
	if (!md->cldma)
		return -ENOMEM;
	ret = ccci_cldma_init(md);
	if (ret) {
		dev_err(dev, "transport init failed (%d)\n", ret);
		return ret;
	}
	ret = devm_add_action_or_reset(dev, ccci_cldma_deinit_action, md);
	if (ret)
		return ret;


	ret = ccci_char_init(md);
	if (ret) {
		dev_err(dev, "character device init failed (%d)\n", ret);
		return ret;
	}
	ret = devm_add_action_or_reset(dev, ccci_char_deinit_action, md);
	if (ret)
		return ret;

	ret = ccci_ccmni_init(md);
	if (ret) {
		dev_err(dev, "network interface init failed (%d)\n", ret);
		return ret;
	}
	ret = devm_add_action_or_reset(dev, ccci_ccmni_deinit_action, md);
	if (ret)
		return ret;

	mutex_lock(&ccci_modem_list_lock);
	if (!list_empty(&ccci_modem_list)) {
		mutex_unlock(&ccci_modem_list_lock);
		dev_err(dev, "only one modem is supported\n");
		return -EBUSY;
	}
	list_add_tail(&md->modem_entry, &ccci_modem_list);
	mutex_unlock(&ccci_modem_list_lock);
	devm_add_action_or_reset(dev, ccci_unregister_modem, md);

	platform_set_drvdata(pdev, md);
	ccci_debugfs_init(md);

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	devm_add_action_or_reset(dev, ccci_pm_disable, dev);

	dev_info(dev, "%s modem, CLDMA queues 0x%02x, network queues 0x%02x\n",
		 md->pdata->name, md->pdata->cldma.queue_mask,
		 md->pdata->cldma.net_queue_mask);

	/*
	 * Do not power the modem on behind the userspace's back: RILD and
	 * md_init drive CCCI_IOC_POWER_ON_MD once the SIM and the firmware
	 * are ready. The poller only watches a boot in progress.
	 */
	mod_delayed_work(system_wq, &md->poll_work,
			 msecs_to_jiffies(CCCI_MD_WDT_POLL_MS));

	return 0;
}

static void ccci_remove(struct platform_device *pdev)
{
	struct ccci_modem *md = platform_get_drvdata(pdev);

	if (!md)
		return;

	ccci_md_power_off(md, "driver removal");
	cancel_delayed_work_sync(&md->poll_work);
	debugfs_remove_recursive(md->debug_root);
	md->debug_root = NULL;
	WRITE_ONCE(md->state, CCCI_STATE_INVALID);
}

static struct platform_driver ccci_driver = {
	.probe = ccci_probe,
	.remove = ccci_remove,
	.driver = {
		.name = "mtk-ccci",
		.of_match_table = ccci_match_table,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ccci_driver);

MODULE_AUTHOR("MediaTek Inc.");
MODULE_DESCRIPTION("MediaTek MD1 cellular modem (ECCCI)");
MODULE_LICENSE("GPL");
