// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MD1 cellular modem (ECCCI) - CLDMA ring driver.
 *
 * CLDMA is the DMA engine that carries CCCI packets between the AP and the
 * MD core. It has two AXI masters sharing one ring pair:
 *
 *   UL (Ap -> md): descriptors built here, the engine reads our data buffer
 *                  and writes it into the modem side ring.
 *   SO (Md -> ap): the engine fills the descriptors and buffers we provide,
 *                  the IRQ tells us when a descriptor has been completed.
 *
 * Both directions use a circular array of 16 byte GPDs in coherent memory,
 * linked by physical address, with hardware ownership handed over through
 * the HWO bit (bit 0 of the flags byte). The checksum byte protects the
 * flags and mailbox fields; the engine silently discards a GPD whose
 * checksum does not match, which is a classic source of "the modem never
 * answers" bugs, so it is always computed here.
 *
 * Receive side: the completion IRQ only drains the queue indices and defers
 * routing to NAPI, which keeps the hard IRQ handler bounded.
 */
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/random.h>
#include <linux/skbuff.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "ccci_priv.h"
#include "ccci_cldma_reg.h"

#define CCCI_NET_IP_ALIGN	2

static inline u32 cldma_rd(struct ccci_modem *md, enum ccci_reg_index idx,
			   u32 off)
{
	return ioread32(md->base[idx] + off);
}

static inline void cldma_wr(struct ccci_modem *md, enum ccci_reg_index idx,
			    u32 off, u32 val)
{
	iowrite32(val, md->base[idx] + off);
}

/**
 * ccci_cldma_queue_of_ch() - map a logical channel to a CLDMA queue
 * @ch: logical channel
 * @queue: filled with the queue number
 *
 * Non network channels share queues 0..5, network channels use the two
 * dedicated queues so that a saturated control channel can never starve the
 * data path (and vice versa).
 */
bool ccci_cldma_queue_of_ch(int ch, int *queue)
{
	if (ch < 0 || ch > CCCI_MAX_CH_NUM)
		return false;

	if (ccci_ch_is_net(ch)) {
		/* RX, TX and DL-ACK channels of one netdev share a queue. */
		*queue = CCCI_CLDMA_NET_FIRST_Q +
			 ((ch - CCCI_CCMNI1_RX) / 4) % CCCI_CLDMA_NET_NUM_Q;
		return true;
	}

	switch (ch) {
	case CCCI_CONTROL_RX:
	case CCCI_CONTROL_TX:
		*queue = 0;
		return true;
	case CCCI_SYSTEM_RX:
	case CCCI_SYSTEM_TX:
	case CCCI_PMIC_RX:
	case CCCI_PMIC_TX:
	case CCCI_UEM_RX:
	case CCCI_UEM_TX:
	case CCCI_STATUS_RX:
	case CCCI_STATUS_TX:
		*queue = 1;
		return true;
	case CCCI_FS_RX:
	case CCCI_FS_TX:
		*queue = 2;
		return true;
	case CCCI_UART1_RX:
	case CCCI_UART1_RX_ACK:
	case CCCI_UART1_TX:
	case CCCI_UART1_TX_ACK:
	case CCCI_UART2_RX:
	case CCCI_UART2_RX_ACK:
	case CCCI_UART2_TX:
	case CCCI_UART2_TX_ACK:
		*queue = 3;
		return true;
	case CCCI_RPC_RX:
	case CCCI_RPC_TX:
	case CCCI_IPC_RX:
	case CCCI_IPC_RX_ACK:
	case CCCI_IPC_TX:
	case CCCI_IPC_TX_ACK:
	case CCCI_IPC_UART_RX:
	case CCCI_IPC_UART_RX_ACK:
	case CCCI_IPC_UART_TX:
	case CCCI_IPC_UART_TX_ACK:
		*queue = 4;
		return true;
	case CCCI_MD_LOG_RX:
	case CCCI_MD_LOG_TX:
		*queue = 5;
		return true;
	default:
		return false;
	}
}

static void ccci_cldma_gpd_init(struct cldma_gpd *gpd, dma_addr_t next)
{
	memset(gpd, 0, sizeof(*gpd));
	gpd->next = cpu_to_le32((u32)next);
	gpd->ext_len = 0;
	gpd->reserved = 0;
}

static void ccci_cldma_gpd_own(struct cldma_gpd *gpd, dma_addr_t buf,
			       unsigned int len, bool ioc, u8 extra_flags,
			       u16 mailbox)
{
	u8 flags = CLDMA_GPD_HWO | extra_flags;

	if (ioc)
		flags |= CLDMA_GPD_IOC;

	gpd->mailbox = cpu_to_le16(mailbox);
	gpd->buffer = cpu_to_le32((u32)buf);
	gpd->length = cpu_to_le16(len);
	gpd->checksum = cldma_gpd_checksum(flags, gpd->mailbox);
	/*
	 * The flags byte carries the ownership bit: it must be written last,
	 * after the descriptor body and the checksum are visible to the DMA
	 * engine.
	 */
	dma_wmb();
	WRITE_ONCE(gpd->flags, flags);
}

static void ccci_cldma_gpd_release(struct cldma_gpd *gpd)
{
	WRITE_ONCE(gpd->flags, READ_ONCE(gpd->flags) & ~CLDMA_GPD_HWO);
}

/* ---------------------------------------------------------------------- */
/* Rings                                                                  */
/* ---------------------------------------------------------------------- */

static int ccci_cldma_ring_alloc(struct ccci_modem *md,
				 struct cldma_ring *ring, const char *name)
{
	struct device *dev = md->dev;
	int i;

	ring->rings = dma_alloc_coherent(dev,
					 sizeof(struct cldma_gpd) *
					 CCCI_CLDMA_QUEUE_GPD,
					 &ring->rings_dma, GFP_KERNEL);
	if (!ring->rings)
		return -ENOMEM;

	for (i = 0; i < CCCI_CLDMA_QUEUE_GPD; i++) {
		dma_addr_t next = ring->rings_dma +
				  (sizeof(struct cldma_gpd) *
				   ((i + 1) % CCCI_CLDMA_QUEUE_GPD));

		ring->dsc[i].gpd = &ring->rings[i];
		ring->dsc[i].gpd_dma = ring->rings_dma +
				       sizeof(struct cldma_gpd) * i;
		ring->dsc[i].skb = NULL;
		ccci_cldma_gpd_init(ring->dsc[i].gpd, next);
	}

	ring->read = 0;
	ring->write = 0;
	ccci_dbg(md, "%s ring at 0x%pad\n", name, &ring->rings_dma);
	return 0;
}

static void ccci_cldma_ring_free(struct ccci_modem *md,
				 struct cldma_ring *ring)
{
	struct device *dev = md->dev;
	int i;

	for (i = 0; i < CCCI_CLDMA_QUEUE_GPD; i++) {
		struct cldma_desc *dsc = &ring->dsc[i];

		if (!dsc->skb)
			continue;
		if (dsc->dma)
			dma_unmap_single(dev, dsc->dma, dsc->dma_len,
					 DMA_TO_DEVICE);
		dev_kfree_skb_any(dsc->skb);
		dsc->skb = NULL;
		dsc->dma = 0;
	}

	if (ring->rings) {
		dma_free_coherent(dev,
				  sizeof(struct cldma_gpd) *
				  CCCI_CLDMA_QUEUE_GPD,
				  ring->rings, ring->rings_dma);
		ring->rings = NULL;
	}
}

/* Size class of a received packet: 16, 1.5K or 4K buffers. */
/**
 * ccci_cldma_rx_refill() - give a receive descriptor back to the hardware
 * @q: queue
 * @ring: receive ring
 * @dsc: descriptor slot to refill
 *
 * The receiver always hands over full size buffers: CLDMA has no "packet"
 * concept, it writes up to the allowed length and reports how much arrived.
 */
static int ccci_cldma_rx_refill_one(struct cldma_queue *q,
				    struct cldma_ring *ring,
				    struct cldma_desc *dsc)
{
	struct ccci_modem *md = q->md;
	struct sk_buff *skb;
	unsigned int size = 4096;
	dma_addr_t dma;

	skb = alloc_skb(size, GFP_ATOMIC | __GFP_NOWARN);
	if (!skb)
		return -ENOMEM;

	dma = dma_map_single(md->dev, skb->data, size, DMA_FROM_DEVICE);
	if (dma_mapping_error(md->dev, dma)) {
		kfree_skb(skb);
		return -ENOMEM;
	}

	dsc->skb = skb;
	dsc->dma = dma;
	dsc->dma_len = size;
	dsc->skb_len = 0;

	/*
	 * Allow length: the whole buffer, minus one byte so that the engine
	 * never reports a full buffer (which would be ambiguous with "no
	 * space left").  IOC on the last descriptor of the ring is implied by
	 * the hardware; setting it on all of them keeps partial rings working.
	 */
	ccci_cldma_gpd_own(dsc->gpd, dma, size - 1, true, 0, size - 1);
	return 0;
}

static int ccci_cldma_rx_refill(struct cldma_queue *q, struct cldma_ring *ring)
{
	unsigned long flags;
	unsigned int i;
	int ret = 0;

	spin_lock_irqsave(&q->md->cldma->irq_lock, flags);
	for (i = 0; i < CCCI_CLDMA_QUEUE_GPD; i++) {
		if (ring->dsc[i].skb)
			continue;
		if (ccci_cldma_rx_refill_one(q, ring, &ring->dsc[i])) {
			ret = -ENOMEM;
			break;
		}
	}
	spin_unlock_irqrestore(&q->md->cldma->irq_lock, flags);

	return ret;
}

/* ---------------------------------------------------------------------- */
/* NAPI receive                                                           */
/* ---------------------------------------------------------------------- */

/**
 * ccci_cldma_rx_collect() - hand completed receive descriptors upwards
 * @q: queue
 * @budget: maximum number of packets
 */
static int ccci_cldma_rx_collect(struct cldma_queue *q, int budget)
{
	struct ccci_modem *md = q->md;
	struct cldma_ring *ring = &q->rx;
	struct device *dev = md->dev;
	int done = 0;

	while (done < budget && cldma_ring_busy(ring->read, ring->write)) {
		struct cldma_desc *dsc = &ring->dsc[ring->read];
		struct cldma_gpd *gpd = dsc->gpd;
		unsigned int len;
		struct sk_buff *skb;

		/* The peer still owns it: nothing more to do right now. */
		if (READ_ONCE(gpd->flags) & CLDMA_GPD_HWO)
			break;

		len = le16_to_cpu(gpd->length);
		skb = dsc->skb;
		dsc->skb = NULL;

		ring->read = (ring->read + 1) & (CCCI_CLDMA_QUEUE_GPD - 1);
		done++;

		if (!skb) {
			q->rx_full++;
			continue;
		}

		/* Revalidate after the unlock: the queue may be torn down. */
		if (!q->rxq)
			continue;

		dma_unmap_single(dev, dsc->dma, dsc->dma_len,
				 DMA_FROM_DEVICE);
		dsc->dma = 0;

		if (len < CCCI_HDR_SIZE || len > dsc->dma_len) {
			dev_warn_ratelimited(dev,
					     "queue %u: bogus GPD length %u\n",
					     q->index, len);
			dev_kfree_skb_any(skb);
			continue;
		}

		/* Trim the fixed size buffer down to the received length. */
		skb_put(skb, len);
		q->rx_pkts++;
		q->rx_bytes += len;

		ccci_recv_packet(md, skb);
	}

	return done;
}

static bool ccci_cldma_rx_work_pending(struct cldma_queue *q)
{
	struct cldma_ring *ring = &q->rx;
	struct cldma_gpd *gpd;

	if (!cldma_ring_busy(ring->read, ring->write))
		return false;

	gpd = ring->dsc[ring->read].gpd;
	return !(READ_ONCE(gpd->flags) & CLDMA_GPD_HWO);
}

static int ccci_cldma_napi(struct napi_struct *napi, int budget)
{
	struct ccci_cldma *cldma = container_of(napi, struct ccci_cldma, napi);
	int i, done = 0, total = 0;

	for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
		struct cldma_queue *q = &cldma->q[i];

		if (!q->rxq)
			continue;
		done = ccci_cldma_rx_collect(q, budget - total);
		total += done;
		if (total >= budget)
			break;
	}

	if (total < budget && napi_complete_done(napi, total)) {
		bool pending = false;

		for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
			if (cldma->q[i].rxq && ccci_cldma_rx_work_pending(&cldma->q[i]))
				pending = true;
		}
		if (pending)
			napi_schedule(napi);
	}

	return total;
}

/**
 * ccci_cldma_napi_poll() - drain the receive rings
 * @md: modem
 * @napi: NAPI context passed by the caller, ignored
 * @budget: maximum number of descriptors to consume
 *
 * The receive path is shared by every channel: the dispatcher routes the
 * packets by logical channel, so a single NAPI instance is enough.
 */
int ccci_cldma_napi_poll(struct ccci_modem *md, struct napi_struct *napi,
			 int budget)
{
	struct ccci_cldma *cldma = md->cldma;
	int i, done, total = 0;

	if (!cldma || !cldma->started)
		return 0;

	for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
		struct cldma_queue *q = &cldma->q[i];

		if (!q->rxq)
			continue;
		done = ccci_cldma_rx_collect(q, budget - total);
		total += done;
		if (total >= budget)
			break;
	}

	return total;
}

/* ---------------------------------------------------------------------- */
/* Transmit                                                               */
/* ---------------------------------------------------------------------- */

static void ccci_cldma_tx_reclaim(struct cldma_queue *q)
{
	struct ccci_modem *md = q->md;
	struct cldma_ring *ring = &q->tx;
	struct device *dev = md->dev;

	while (cldma_ring_busy(ring->read, ring->write)) {
		struct cldma_desc *dsc = &ring->dsc[ring->read];
		struct cldma_gpd *gpd = dsc->gpd;

		if (READ_ONCE(gpd->flags) & CLDMA_GPD_HWO)
			break;

		if (dsc->skb) {
			dma_unmap_single(dev, dsc->dma, dsc->dma_len,
					 DMA_TO_DEVICE);
			dev_kfree_skb_any(dsc->skb);
			dsc->skb = NULL;
			dsc->dma = 0;
		}
		ccci_cldma_gpd_release(gpd);
		dsc->gpd->buffer = 0;
		dsc->gpd->length = 0;
		ring->read = (ring->read + 1) & (CCCI_CLDMA_QUEUE_GPD - 1);
	}
}

static unsigned int ccci_cldma_tx_free(struct cldma_queue *q)
{
	struct cldma_ring *ring = &q->tx;

	/*
	 * The ring keeps one slot empty so that "full" and "empty" are
	 * distinguishable from the indices alone.
	 */
	return cldma_ring_free(ring->read, ring->write);
}

static void ccci_cldma_tx_kick(struct cldma_queue *q)
{
	struct ccci_modem *md = q->md;

	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_UL_START_CMD,
		 BIT(q->index));
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_UL_RESUME_CMD,
		 BIT(q->index));
}

/**
 * ccci_cldma_send_packet() - queue one CCCI packet for transmission
 * @md: modem
 * @ch: logical channel, used to pick the queue
 * @skb: packet, ownership is transferred even on failure
 *
 * Returns 0 when the packet was handed to the engine, -EBUSY when the ring
 * is full (the caller may retry) and a negative errno otherwise.
 */
int ccci_cldma_send_packet(struct ccci_modem *md, int ch, struct sk_buff *skb)
{
	struct ccci_cldma *cldma = md->cldma;
	struct cldma_queue *q;
	struct cldma_ring *ring;
	struct cldma_desc *dsc;
	dma_addr_t dma;
	unsigned int len, queue_idx;
	unsigned long flags;
	int ret = 0;

	if (!cldma || !cldma->started) {
		kfree_skb(skb);
		return -CCCI_ERR_HIF_NOT_POWER_ON;
	}

	if (!ccci_cldma_queue_of_ch(ch, &queue_idx)) {
		kfree_skb(skb);
		return -CCCI_ERR_CHANNEL_NUM_MIS_MATCH;
	}

	if (queue_idx >= CCCI_CLDMA_QUEUE_NUM ||
	    !(cldma->hw->queue_mask & BIT(queue_idx))) {
		kfree_skb(skb);
		return -CCCI_ERR_INVALID_PARAM;
	}

	q = &cldma->q[queue_idx];
	if (!q->txq || !q->tx.rings) {
		kfree_skb(skb);
		return -CCCI_ERR_INVALID_PARAM;
	}

	len = skb->len;
	if (len > CCCI_PKT_SIZE_MAX) {
		kfree_skb(skb);
		return -CCCI_ERR_INVALID_PARAM;
	}

	dma = dma_map_single(md->dev, skb->data, len, DMA_TO_DEVICE);
	if (dma_mapping_error(md->dev, dma)) {
		kfree_skb(skb);
		return -ENOMEM;
	}

	ring = &q->tx;

	spin_lock_irqsave(&cldma->irq_lock, flags);

	ccci_cldma_tx_reclaim(q);

	if (!ccci_cldma_tx_free(q)) {
		/* Try to make room by kicking the engine once more. */
		ccci_cldma_tx_kick(q);
		spin_unlock_irqrestore(&cldma->irq_lock, flags);
		dma_unmap_single(md->dev, dma, len, DMA_TO_DEVICE);
		kfree_skb(skb);
		return -EBUSY;
	}

	dsc = &ring->dsc[ring->write];
	dsc->skb = skb;
	dsc->dma = dma;
	dsc->dma_len = len;
	dsc->skb_len = len;

	/*
	 * The completion interrupt is requested on the last descriptor we own
	 * so that a burst of packets produces a single IRQ.
	 */
	ccci_cldma_gpd_own(dsc->gpd, dma, len, true, 0,
			   (u16)(len & 0xffff));

	ring->write = (ring->write + 1) & (CCCI_CLDMA_QUEUE_GPD - 1);
	q->tx_pkts++;
	q->tx_bytes += len;

	ccci_cldma_tx_kick(q);
	spin_unlock_irqrestore(&cldma->irq_lock, flags);

	if (ret)
		kfree_skb(skb);
	return ret;
}

/* ---------------------------------------------------------------------- */
/* Interrupts                                                             */
/* ---------------------------------------------------------------------- */

/*
 * ccci_cldma_report_error() - decode and report a CLDMA queue error
 * @md: modem
 * @dir: "TX" or "RX", for the log
 * @l2: level 2 status register value
 * @l3: level 3 status register value
 *
 * Level 2 has one bit per queue in the error nibble, level 3 carries the
 * error class in four byte lanes, each with one bit per queue. A descriptor
 * or checksum error is recoverable (the descriptor is dropped); an internal
 * engine error wedges the DMA and needs a modem reset.
 */
static void ccci_cldma_dec_error(struct ccci_modem *md, const char *dir,
				 u32 l2, u32 l3)
{
	u32 qmask = 0;
	bool fatal = false;
	int i;

	for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
		if (l2 & BIT(16 + i))
			qmask |= BIT(i);
		if (l3 & BIT(i))
			fatal = true;
	}
	if (!qmask)
		qmask = l3 & 0xff;

	dev_err_ratelimited(md->dev,
			    "%s error on queue(s) 0x%02x, L2=0x%08x L3=0x%08x, class%s\n",
			    dir, qmask, l2, l3,
			    fatal ? " internal, resetting the modem" : " recoverable");

	if (!fatal)
		return;

	WRITE_ONCE(md->cldma->bus_error, true);
	if (!work_pending(&md->boot_work))
		queue_work(md->wq, &md->boot_work);
}

static irqreturn_t ccci_cldma_irq(int irq, void *data)
{
	struct ccci_modem *md = data;
	struct ccci_cldma *cldma = md->cldma;
	u32 t0, t1, r0, r1;
	bool wake = false;
	int i;

	t0 = cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2TISAR0);
	t1 = cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2TISAR1);
	r0 = cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2RISAR0);
	r1 = cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2RISAR1);

	if (!t0 && !t1 && !r0 && !r1)
		return IRQ_NONE;

	/* Acknowledge (write one to clear) everything we saw. */
	if (t0)
		cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2TISAR0, t0);
	if (t1)
		cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2TISAR1, t1);
	if (r0)
		cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2RISAR0, r0);
	if (r1)
		cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2RISAR1, r1);

	if (unlikely((t0 | t1) & CLDMA_INT_ERROR)) {
		u32 l3t0 = cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3TISAR0);
		u32 l3t1 = cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3TISAR1);

		cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3TISAR0, l3t0);
		cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3TISAR1, l3t1);
		if (l3t0 | l3t1)
			ccci_cldma_dec_error(md, "TX", t0 | t1, l3t0 | l3t1);
	}

	if (unlikely((r0 | r1) & CLDMA_INT_ERROR)) {
		u32 l3r0 = cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3RISAR0);
		u32 l3r1 = cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3RISAR1);

		cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3RISAR0, l3r0);
		cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3RISAR1, l3r1);
		if (l3r0 | l3r1)
			ccci_cldma_dec_error(md, "RX", r0 | r1, l3r0 | l3r1);
	}

	/* TX completion: the engine finished the GPDs it was given. */
	if ((t0 | t1) & (CLDMA_INT_DONE | CLDMA_INT_QUEUE_EMPTY)) {
		for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
			struct cldma_queue *q = &cldma->q[i];
			unsigned long flags;

			if (!q->txq)
				continue;
			spin_lock_irqsave(&cldma->irq_lock, flags);
			ccci_cldma_tx_reclaim(q);
			q->tx_busy = false;
			spin_unlock_irqrestore(&cldma->irq_lock, flags);
			wake = true;
		}
	}

	/* RX: a descriptor has been filled, let NAPI do the heavy lifting. */
	if ((r0 | r1) & (CLDMA_INT_DONE | CLDMA_INT_QUEUE_EMPTY))
		napi_schedule(&cldma->napi);

	if (wake) {
		for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
			if (cldma->q[i].txq)
				wake_up(&cldma->q[i].wq);
		}
	}

	return IRQ_HANDLED;
}

static irqreturn_t ccci_ccif_irq(int irq, void *data)
{
	struct ccci_modem *md = data;
	u32 con;

	/*
	 * The CCIF interrupt carries the small numbered control messages the
	 * baseband uses for "boot stage 2 ready", watchdog pings and the
	 * flight mode acknowledgements. The payload lives in the shared SRAM
	 * and is consumed by the poller / control port, so the handler only
	 * acknowledges the interrupt here.
	 */
	con = cldma_rd(md, CCCI_REG_CCIF_AP, CCIF_CON);
	cldma_wr(md, CCCI_REG_CCIF_AP, CCIF_CON,
		 con & ~(CCIF_CON_TX_IRQ | CCIF_CON_RX_IRQ));
	cldma_wr(md, CCCI_REG_CCIF_AP, CCIF_CON,
		 con | CCIF_CON_TX_ACK | CCIF_CON_RX_ACK);

	return IRQ_HANDLED;
}

static irqreturn_t ccci_mdwdt_irq(int irq, void *data)
{
	struct ccci_modem *md = data;

	dev_err_ratelimited(md->dev, "modem watchdog expired\n");
	md->ex_stage = CCCI_MD_BOOT_STAGE_2;
	if (!work_pending(&md->boot_work))
		queue_work(md->wq, &md->boot_work);

	return IRQ_HANDLED;
}

/* ---------------------------------------------------------------------- */
/* Engine start/stop                                                      */
/* ---------------------------------------------------------------------- */

static void ccci_cldma_hw_reset(struct ccci_modem *md)
{
	struct ccci_cldma *cldma = md->cldma;
	u32 mask = BIT(0);
	int i;

	/*
	 * Hold both engines in reset, wait until they report idle, then
	 * program the rings. The stop command drains the descriptors the
	 * engine still owns; a stalled engine is released by the reset.
	 */
	for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
		if (!(cldma->hw->queue_mask & BIT(i)))
			continue;
		cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_UL_TCR(i),
			 CLDMA_TC_DISABLE);
		cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_UL_STOP_CMD, BIT(i));
	}

	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_SO_STOP_CMD, 0xff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2TISAR0, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2TISAR1, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2RISAR0, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2RISAR1, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3TISAR0, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3TISAR1, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3RISAR0, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3RISAR1, 0xffffffff);

	/* Mask everything while the rings are being reprogrammed. */
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2TIMSR0, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2TIMSR1, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3TIMSR0, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3TIMSR1, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3RIMSR0, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3RIMSR1, 0xffffffff);

	/*
	 * The MD side mirrors the AP ring addresses: MD UL reads our TX ring
	 * and MD SO writes our RX ring. These registers live in the PD window
	 * and are shared between the two halves.
	 */
	for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
		struct cldma_queue *q = &cldma->q[i];

		if (!(cldma->hw->queue_mask & BIT(i)))
			continue;
		if (q->tx.rings)
			cldma_wr(md, CCCI_REG_CLDMA_MD_PDN,
				 CLDMA_MD_UL_START_ADDR(i),
				 (u32)q->tx.rings_dma);
		if (q->rx.rings)
			cldma_wr(md, CCCI_REG_CLDMA_MD_PDN,
				 CLDMA_MD_SO_START_ADDR(i),
				 (u32)q->rx.rings_dma);
	}

	(void)mask;
}

static void ccci_cldma_hw_start(struct ccci_modem *md)
{
	struct ccci_cldma *cldma = md->cldma;
	int i;

	/* Unmask the normal traffic interrupts, keep error classes off. */
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2TIMCR0,
		 CLDMA_INT_DONE | CLDMA_INT_QUEUE_EMPTY | CLDMA_INT_ERROR);
	/*
	 * Traffic control is enabled per queue with the "TC enable" marker
	 * in the high byte; the engine only serves queues that carry it.
	 */
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2TIMCR1, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3TIMCR0, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3TIMCR1, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3RIMCR0, 0xffffffff);
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3RIMCR1, 0xffffffff);

	for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
		if (!(cldma->hw->queue_mask & BIT(i)))
			continue;
		cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_UL_TCR(i),
			 CLDMA_TC_ENABLE);
		cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_UL_TCR(i),
			 CLDMA_TC_ENABLE | CLDMA_TC_ENABLE_MARK);
	}

	/* Start the receive engine: it immediately begins filling the ring. */
	cldma_wr(md, CCCI_REG_CLDMA_AP_AO, CLDMA_SO_START_CMD, 0xff);
}

int ccci_cldma_start(struct ccci_modem *md)
{
	struct ccci_cldma *cldma = md->cldma;
	int i, ret = 0;

	if (!cldma)
		return -ENODEV;
	if (cldma->started)
		return 0;

	mutex_lock(&cldma->tx_lock);

	cldma->bus_error = false;
	for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
		cldma->q[i].tx.read = 0;
		cldma->q[i].tx.write = 0;
		cldma->q[i].rx.read = 0;
		cldma->q[i].rx.write = CCCI_CLDMA_QUEUE_GPD - 1;
		cldma->q[i].tx_busy = false;
	}

	for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
		struct cldma_queue *q = &cldma->q[i];

		if (!q->rxq)
			continue;
		ret = ccci_cldma_rx_refill(q, &q->rx);
		if (ret) {
			dev_err(md->dev, "queue %d: no memory for RX buffers\n",
				i);
			goto err;
		}
	}

	ccci_cldma_hw_reset(md);
	ccci_cldma_hw_start(md);

	if (!cldma->napi_enabled) {
		netif_napi_add_weight(NULL, &cldma->napi, ccci_cldma_napi,
				      CCCI_CLDMA_BUDGET);
		napi_enable(&cldma->napi);
		cldma->napi_enabled = true;
	}
	cldma->started = true;
	mutex_unlock(&cldma->tx_lock);

	ccci_info(md, "CLDMA started, %s\n", cldma->hw->name);
	return 0;

err:
	cldma->started = false;
	mutex_unlock(&cldma->tx_lock);
	return ret;
}

void ccci_cldma_stop(struct ccci_modem *md)
{
	struct ccci_cldma *cldma = md->cldma;
	int i;

	if (!cldma || !cldma->started)
		return;

	mutex_lock(&cldma->tx_lock);
	cldma->started = false;

	ccci_cldma_hw_reset(md);

	if (cldma->napi_enabled) {
		napi_disable(&cldma->napi);
		netif_napi_del(&cldma->napi);
		cldma->napi_enabled = false;
	}

	for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
		struct cldma_queue *q = &cldma->q[i];

		ccci_cldma_tx_reclaim(q);
		/* Drop anything the modem left in the ring. */
		ccci_cldma_rx_collect(q, CCCI_CLDMA_QUEUE_GPD);
		ccci_cldma_ring_free(md, &q->rx);
		ccci_cldma_ring_free(md, &q->tx);
		q->tx_busy = false;
		wake_up_all(&q->wq);
	}
	mutex_unlock(&cldma->tx_lock);
}

/* ---------------------------------------------------------------------- */
/* MD watchdog poller                                                     */
/* ---------------------------------------------------------------------- */

static void ccci_cldma_wdt_check(struct work_struct *work)
{
	struct ccci_cldma *cldma =
		container_of(to_delayed_work(work), struct ccci_cldma, wdt_work);
	struct ccci_modem *md = cldma->md;

	if (READ_ONCE(md->state) == CCCI_STATE_READY && cldma->started) {
		u32 idle = cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_CHNL_IDLE);
		u32 busy = cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_IP_BUSY);

		/* A stuck bus master wedges the whole AP: report it early. */
		if (busy & cldma->hw->queue_mask && !(idle & cldma->hw->queue_mask))
			dev_warn_ratelimited(md->dev,
					     "CLDMA busy with no idle queue (0x%08x)\n",
					     busy);
	}

	schedule_delayed_work(&cldma->wdt_work,
			      msecs_to_jiffies(CCCI_MD_WDT_POLL_MS));
}

void ccci_cldma_dump(struct ccci_modem *md, const char *reason)
{
	struct ccci_cldma *cldma = md->cldma;
	int i;

	if (!cldma)
		return;

	dev_err(md->dev, "CLDMA dump (%s): L2T=0x%08x L2R=0x%08x L3T=0x%08x L3R=0x%08x idle=0x%08x\n",
		reason,
		cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2TISAR0),
		cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L2RISAR0),
		cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3TISAR0),
		cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_L3RISAR0),
		cldma_rd(md, CCCI_REG_CLDMA_AP_AO, CLDMA_CHNL_IDLE));

	for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
		struct cldma_queue *q = &cldma->q[i];

		if (!q->txq || !q->rxq)
			continue;
		dev_err(md->dev,
			"queue %d: tx %u/%u rx %u/%u pkts %llu/%llu\n",
			i, q->tx.read, q->tx.write, q->rx.read, q->rx.write,
			q->tx_pkts, q->rx_pkts);
	}
}

/* ---------------------------------------------------------------------- */
/* Init / deinit                                                          */
/* ---------------------------------------------------------------------- */

int ccci_cldma_init(struct ccci_modem *md)
{
	struct ccci_cldma *cldma = md->cldma;
	int i, ret;

	cldma->md = md;
	cldma->hw = &md->pdata->cldma;
	spin_lock_init(&cldma->irq_lock);
	mutex_init(&cldma->tx_lock);
	INIT_DELAYED_WORK(&cldma->wdt_work, ccci_cldma_wdt_check);

	cldma->wq = alloc_ordered_workqueue("ccci-cldma", WQ_MEM_RECLAIM);
	if (!cldma->wq)
		return -ENOMEM;

	for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
		struct cldma_queue *q = &cldma->q[i];

		q->md = md;
		q->index = i;
		q->txq = !!(cldma->hw->queue_mask & BIT(i));
		q->rxq = q->txq;
		init_waitqueue_head(&q->wq);

		if (!q->txq)
			continue;
		/*
		 * Network queues always exist even on SoCs that only expose
		 * six hardware queues: queue 6/7 are then reused with normal
		 * priority instead of being dropped.
		 */
		ret = ccci_cldma_ring_alloc(md, &q->rx, "RX");
		if (ret)
			goto err_rings;
		ret = ccci_cldma_ring_alloc(md, &q->tx, "TX");
		if (ret)
			goto err_rings;
	}

	/* Register the interrupts after the rings exist. */
	for (i = 0; i < CCCI_IRQ_MAX; i++) {
		irq_handler_t handler;
		unsigned long flags = 0;
		const char *name;

		if (md->irq[i] <= 0)
			continue;

		switch (i) {
		case CCCI_IRQ_CLDMA:
			handler = ccci_cldma_irq;
			name = "cldma";
			flags = IRQF_ONESHOT;
			break;
		case CCCI_IRQ_CCIF:
			handler = ccci_ccif_irq;
			name = "ccif";
			break;
		default:
			handler = ccci_mdwdt_irq;
			name = "mdwdt";
			break;
		}

		snprintf(cldma->irq_name[i], sizeof(cldma->irq_name[i]),
			 "ccci-md%d-%s", md->index + 1, name);
		ret = devm_request_irq(md->dev, md->irq[i], handler, flags,
				       cldma->irq_name[i], md);
		cldma->irq_owned[i] = !ret;
		if (ret) {
			dev_err(md->dev, "cannot request IRQ %d (%s): %d\n",
				md->irq[i], name, ret);
			goto err_irq;
		}
	}

	schedule_delayed_work(&cldma->wdt_work,
			      msecs_to_jiffies(CCCI_MD_WDT_POLL_MS));
	return 0;

err_irq:
	for (i = 0; i < CCCI_IRQ_MAX; i++) {
		if (cldma->irq_owned[i]) {
			devm_free_irq(md->dev, md->irq[i], md);
			cldma->irq_owned[i] = false;
		}
	}
err_rings:
	for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
		ccci_cldma_ring_free(md, &cldma->q[i].rx);
		ccci_cldma_ring_free(md, &cldma->q[i].tx);
	}
	destroy_workqueue(cldma->wq);
	cldma->wq = NULL;
	return ret;
}

void ccci_cldma_deinit(struct ccci_modem *md)
{
	struct ccci_cldma *cldma = md->cldma;
	int i;

	if (!cldma)
		return;

	cancel_delayed_work_sync(&cldma->wdt_work);
	ccci_cldma_stop(md);

	for (i = 0; i < CCCI_IRQ_MAX; i++) {
		if (cldma->irq_owned[i]) {
			devm_free_irq(md->dev, md->irq[i], md);
			cldma->irq_owned[i] = false;
			cldma->irq_name[i][0] = '\0';
		}
	}

	for (i = 0; i < CCCI_CLDMA_QUEUE_NUM; i++) {
		ccci_cldma_ring_free(md, &cldma->q[i].rx);
		ccci_cldma_ring_free(md, &cldma->q[i].tx);
	}

	if (cldma->wq) {
		destroy_workqueue(cldma->wq);
		cldma->wq = NULL;
	}
}
