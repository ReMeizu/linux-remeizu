/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Meizu Linux mainline project
 *
 * STP (Serial Transport Protocol) engine for MediaTek CONSYS.
 *
 * STP is the framing layer that multiplexes the Bluetooth, GPS, Wi-Fi and
 * WMT control channels over the single BTIF serial link.  The wire format
 * (recovered from the vendor stp_core.c) is:
 *
 *	+------+------------------+----------+---------------------+
 *	| hdr0 | hdr1             | hdr2     | crc0 crc1 (little)  |
 *	+------+------------------+----------+---------------------+
 *
 *	hdr0[7]   = 1        (frame marker)
 *	hdr0[6]   = 0        (no NAK)
 *	hdr0[5:3] = TX sequence number
 *	hdr0[2:0] = RX acknowledge number
 *	hdr1[7]   = 0
 *	hdr1[6:4] = channel
 *	hdr1[3:0] = length[11:8]
 *	hdr2[7:0] = length[7:0]
 *	hdr3      = (hdr0 + hdr1 + hdr2) & 0xff
 *	crc       = CRC-16/IBM over the payload, little endian
 *
 * Only full STP mode (channels, sequence numbers, CRC) is implemented: it is
 * the mode the MCU uses on all integrated CONNSYS SoCs.  Pure acknowledgements
 * are four-byte frames with a zero length, exactly like the vendor driver.
 */

#include <linux/crc-itu-t.h>
#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/container_of.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#include "connsys.h"
#include "stp_core.h"

#define STP_SYNC		0x80
#define STP_DELIMITER		0x55
#define STP_HDR_LEN		4
#define STP_CRC_LEN		2
#define STP_FRAME_MAX		(STP_HDR_LEN + CONNSYS_BTIF_MAX_LEN + STP_CRC_LEN)
#define STP_WIN_SIZE		4
#define STP_TX_TIMEOUT		(200 * HZ / 1000)
#define STP_BUF_SIZE		SZ_16K
#define STP_RETRY_MAX		32

static inline bool stp_type_valid(u8 type)
{
	return type < CONNSYS_STP_MAX;
}

static int stp_flush_locked(struct connsys_dev *cd, struct stp_engine *stp)
{
	size_t off;
	int ret;

	if (!stp->tx_len || stp->barrier)
		return 0;

	for (off = 0; off < stp->tx_len;) {
		size_t chunk = min_t(size_t, stp->tx_len - off, 64);

		ret = connsys_btif_tx(cd, stp->tx_buf + off, chunk);
		if (ret)
			return ret;
		off += chunk;
	}
	stp->tx_len = 0;

	return 0;
}

static void stp_send_ack(struct connsys_dev *cd, struct stp_engine *stp,
			 u8 ack)
{
	u8 frame[STP_HDR_LEN];

	if (stp->barrier || !cd->btif)
		return;

	frame[0] = STP_SYNC | ack;
	frame[1] = CONNSYS_STP_STP << 4;
	frame[2] = 0;
	frame[3] = frame[0] + frame[1] + frame[2];
	connsys_btif_tx(cd, frame, sizeof(frame));
}

static void stp_timer_fn(struct timer_list *t)
{
	struct stp_engine *stp = container_of(t, struct stp_engine, timer);

	schedule_work(&stp->retry_work);
}

static void stp_retry_work(struct work_struct *work)
{
	struct stp_engine *stp = container_of(work, struct stp_engine,
					      retry_work);
	struct connsys_dev *cd = container_of(stp, struct connsys_dev, stp);
	unsigned long flags;

	spin_lock_irqsave(&stp->tx_lock, flags);
	if (!stp->barrier && stp->inflight) {
		if (++stp->retries > STP_RETRY_MAX) {
			dev_err(cd->dev, "STP link stalled, resetting\n");
			stp->retries = 0;
			stp->inflight = 0;
			stp->seq = 0;
			stp->ack = 0;
		}
		dev_warn_ratelimited(cd->dev,
				     "STP timeout, replaying %u frame(s)\n",
				     stp->inflight);
		stp->tx_len = 0;
		stp_flush_locked(cd, stp);
		mod_timer(&stp->timer, jiffies + STP_TX_TIMEOUT);
	}
	spin_unlock_irqrestore(&stp->tx_lock, flags);
}

int connsys_stp_init(struct connsys_dev *cd)
{
	struct stp_engine *stp = &cd->stp;
	int i, ret;

	memset(stp, 0, sizeof(*stp));
	spin_lock_init(&stp->tx_lock);
	mutex_init(&stp->rx_lock);
	init_waitqueue_head(&stp->rx_wq);
	init_waitqueue_head(&stp->wmt_wq);
	stp->parser.state = STP_RX_SYNC;

	stp->tx_buf = kvzalloc(STP_BUF_SIZE, GFP_KERNEL);
	if (!stp->tx_buf)
		return -ENOMEM;

	for (i = 0; i < CONNSYS_STP_MAX; i++) {
		ret = kfifo_alloc(&stp->rings[i], SZ_32K, GFP_KERNEL);
		if (ret)
			goto err_free;
	}

	timer_setup(&stp->timer, stp_timer_fn, 0);
	INIT_WORK(&stp->retry_work, stp_retry_work);

	return 0;

err_free:
	while (--i >= 0)
		kfifo_free(&stp->rings[i]);
	kvfree(stp->tx_buf);
	stp->tx_buf = NULL;

	return ret;
}

void connsys_stp_exit(struct connsys_dev *cd)
{
	struct stp_engine *stp = &cd->stp;
	int i;

	timer_delete_sync(&stp->timer);
	cancel_work_sync(&stp->retry_work);

	for (i = 0; i < CONNSYS_STP_MAX; i++)
		kfifo_free(&stp->rings[i]);

	kvfree(stp->tx_buf);
	stp->tx_buf = NULL;
}

/**
 * connsys_rx_work - drain BTIF and push the bytes through the STP parser
 *
 * Scheduled by the BTIF interrupt (and by the in-band paths when the STP
 * link is polled).
 */
void connsys_rx_work(struct work_struct *work)
{
	struct connsys_dev *cd = container_of(work, struct connsys_dev,
					      rx_work);
	u8 *buf;
	size_t n;

	buf = kmalloc(CONNSYS_RX_CHUNK, GFP_KERNEL);
	if (!buf)
		return;

	while ((n = connsys_btif_rx(cd, buf, CONNSYS_RX_CHUNK)))
		connsys_stp_input(cd, buf, n);

	kfree(buf);
}

void connsys_stp_set_barrier(struct connsys_dev *cd, bool on)
{
	struct stp_engine *stp = &cd->stp;
	unsigned long flags;

	spin_lock_irqsave(&stp->tx_lock, flags);
	stp->barrier = on;
	if (!on)
		stp_flush_locked(cd, stp);
	spin_unlock_irqrestore(&stp->tx_lock, flags);
}

void connsys_stp_reset(struct connsys_dev *cd)
{
	struct stp_engine *stp = &cd->stp;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&stp->tx_lock, flags);
	stp->seq = 0;
	stp->ack = 0;
	stp->inflight = 0;
	stp->retries = 0;
	stp->tx_len = 0;
	timer_delete(&stp->timer);
	spin_unlock_irqrestore(&stp->tx_lock, flags);

	mutex_lock(&stp->rx_lock);
	for (i = 0; i < CONNSYS_STP_MAX; i++)
		kfifo_reset(&stp->rings[i]);
	mutex_unlock(&stp->rx_lock);

	memset(&stp->parser, 0, sizeof(stp->parser));
	stp->parser.state = STP_RX_SYNC;
}

/**
 * connsys_stp_send - frame and queue a payload for one STP channel
 *
 * Returns the number of payload bytes handed to the transport, or a
 * negative errno.  A retryable condition (window or request buffer full)
 * yields -EAGAIN so that callers can wait and retry.
 */
int connsys_stp_send(struct connsys_dev *cd, u8 type, const u8 *data,
		     size_t len)
{
	struct stp_engine *stp = &cd->stp;
	unsigned long flags;
	size_t flen = STP_HDR_LEN + len + STP_CRC_LEN;
	u8 *frame;
	u16 crc;
	int ret;

	if (!stp_type_valid(type) || len > CONNSYS_BTIF_MAX_LEN)
		return -EINVAL;

	frame = kmalloc(flen, GFP_ATOMIC);
	if (!frame)
		return -ENOMEM;

	crc = crc_itu_t(0, data, len);

	frame[0] = STP_SYNC | (stp->seq << 3) | stp->ack;
	frame[1] = (type << 4) | ((len >> 8) & 0x0f);
	frame[2] = len & 0xff;
	frame[3] = (frame[0] + frame[1] + frame[2]) & 0xff;
	memcpy(frame + STP_HDR_LEN, data, len);
	frame[STP_HDR_LEN + len] = crc & 0xff;
	frame[STP_HDR_LEN + len + 1] = crc >> 8;

	spin_lock_irqsave(&stp->tx_lock, flags);

	if (stp->barrier) {
		ret = -EAGAIN;
		goto unlock;
	}
	if (stp->inflight >= STP_WIN_SIZE ||
	    stp->tx_len + flen > STP_BUF_SIZE) {
		ret = -EAGAIN;
		goto unlock;
	}
	if (flen > STP_BUF_SIZE) {
		ret = -EMSGSIZE;
		goto unlock;
	}

	memcpy(stp->tx_buf + stp->tx_len, frame, flen);
	stp->tx_len += flen;
	stp->seq = (stp->seq + 1) & 0x07;
	stp->inflight++;
	stp->retries = 0;

	ret = stp_flush_locked(cd, stp);
	if (!ret) {
		mod_timer(&stp->timer, jiffies + STP_TX_TIMEOUT);
		ret = len;
	}

unlock:
	spin_unlock_irqrestore(&stp->tx_lock, flags);
	kfree(frame);

	return ret;
}

static void stp_process_frame(struct connsys_dev *cd, const u8 *payload,
			      size_t len)
{
	struct stp_engine *stp = &cd->stp;
	unsigned long flags;
	u8 ack = stp->parser.ack;
	u8 type = stp->parser.type;
	unsigned int acked;

	spin_lock_irqsave(&stp->tx_lock, flags);
	if (stp->inflight) {
		acked = (ack - stp->ack) & 0x07;
		if (acked > stp->inflight)
			acked = stp->inflight;
		stp->inflight -= acked;
		stp->ack = ack;
		stp->retries = 0;
		stp->tx_len = 0;
	}
	if (stp->inflight)
		mod_timer(&stp->timer, jiffies + STP_TX_TIMEOUT);
	else
		timer_delete(&stp->timer);
	spin_unlock_irqrestore(&stp->tx_lock, flags);

	stp_send_ack(cd, stp, ack);

	if (!len)
		return;

	/* MCU events are consumed by the WMT engine, never queued. */
	if (type == CONNSYS_STP_WMT) {
		connsys_wmt_rx(cd, payload, len);
		return;
	}

	if (type == CONNSYS_STP_STP || type == CONNSYS_STP_INFO) {
		dev_dbg(cd->dev, "STP %s frame with %zu bytes\n",
			type == CONNSYS_STP_STP ? "control" : "info", len);
		return;
	}

	/* Packets of a closed channel are dropped, like the vendor driver. */
	if (type == CONNSYS_STP_BT && !test_bit(CONNSYS_STP_BT, &cd->funcs)) {
		dev_dbg_ratelimited(cd->dev, "BT packet with BT off\n");
		return;
	}

	if (type == CONNSYS_STP_WIFI) {
		if (!test_bit(CONNSYS_STP_WIFI, &cd->funcs)) {
			dev_dbg_ratelimited(cd->dev,
					    "Wi-Fi packet with Wi-Fi off\n");
			return;
		}
		/* the WLAN driver owns its own ring and DMA path */
		mutex_lock(&stp->rx_lock);
		if (kfifo_avail(&stp->rings[type]) < len)
			dev_warn_ratelimited(cd->dev, "Wi-Fi ring full\n");
		kfifo_in(&stp->rings[type], payload, len);
		mutex_unlock(&stp->rx_lock);
		connsys_wlan_rx(cd);
		return;
	}

	mutex_lock(&stp->rx_lock);
	if (kfifo_len(&stp->rings[type]) >= kfifo_size(&stp->rings[type]))
		dev_warn_ratelimited(cd->dev, "channel %u ring full\n", type);
	kfifo_in(&stp->rings[type], payload, len);
	mutex_unlock(&stp->rx_lock);

	wake_up_interruptible_all(&stp->rx_wq);
}

/**
 * connsys_stp_input - feed bytes coming from BTIF into the parser
 */
int connsys_stp_input(struct connsys_dev *cd, const u8 *buf, size_t len)
{
	struct stp_parser *p = &cd->stp.parser;
	size_t i;

	for (i = 0; i < len;) {
		u8 c = buf[i];

		switch (p->state) {
		case STP_RX_SYNC:
			if ((c & 0xc0) == STP_SYNC) {
				p->hdr[0] = c;
				p->seq = (c >> 3) & 0x07;
				p->ack = c & 0x07;
				p->hdr_pos = 1;
				p->state = STP_RX_HEADER;
			} else if (c != STP_DELIMITER) {
				dev_dbg_ratelimited(cd->dev,
						    "STP resync on 0x%02x\n", c);
			}
			i++;
			break;

		case STP_RX_HEADER:
			p->hdr[p->hdr_pos++] = c;
			i++;
			switch (p->hdr_pos) {
			case 2:
				p->type = (c & 0x70) >> 4;
				p->len = (c & 0x0f) << 8;
				continue;
			case 3:
				p->len |= c;
				continue;
			default:
				break;
			}

			if (!stp_type_valid(p->type) ||
			    p->len > CONNSYS_BTIF_MAX_LEN ||
			    ((p->hdr[0] + p->hdr[1] + p->hdr[2]) & 0xff) !=
			    p->hdr[3]) {
				dev_dbg_ratelimited(cd->dev,
						    "bad STP header %02x:%02x:%02x:%02x\n",
						    p->hdr[0], p->hdr[1], p->hdr[2],
						    p->hdr[3]);
				p->state = STP_RX_SYNC;
				break;
			}

			if (!p->len) {
				p->state = STP_RX_SYNC;
				stp_process_frame(cd, NULL, 0);
				break;
			}
			p->pos = 0;
			p->state = STP_RX_DATA;
			break;

		case STP_RX_DATA: {
			size_t n = min_t(size_t, len - i, p->len - p->pos);

			memcpy(p->data + p->pos, buf + i, n);
			p->pos += n;
			i += n;
			if (p->pos == p->len) {
				p->crc_pos = 0;
				p->state = STP_RX_CRC;
			}
			break;
		}

		case STP_RX_CRC:
			p->crc[p->crc_pos++] = c;
			i++;
			if (p->crc_pos < STP_CRC_LEN)
				break;
			p->state = STP_RX_SYNC;
			{
				u16 crc = p->crc[0] | (p->crc[1] << 8);

				if (crc != crc_itu_t(0, p->data, p->len)) {
					dev_warn_ratelimited(cd->dev,
							     "STP crc error on channel %u\n",
							     p->type);
					break;
				}
				stp_process_frame(cd, p->data, p->len);
			}
			break;
		}
	}

	return 0;
}

/**
 * connsys_stp_read - pop the next payload of a channel
 *
 * The caller must provide a buffer of at least CONNSYS_BTIF_MAX_LEN bytes.
 * Returns the payload length, 0 when nothing is queued.
 */
size_t connsys_stp_read(struct connsys_dev *cd, u8 type, u8 *buf, size_t len)
{
	struct stp_engine *stp = &cd->stp;
	size_t ret;

	if (!stp_type_valid(type))
		return 0;

	mutex_lock(&stp->rx_lock);
	ret = kfifo_out(&stp->rings[type], buf, len);
	mutex_unlock(&stp->rx_lock);

	return ret;
}

bool connsys_stp_rx_pending(struct connsys_dev *cd, u8 type)
{
	struct stp_engine *stp = &cd->stp;
	bool pending;

	if (!stp_type_valid(type))
		return false;

	mutex_lock(&stp->rx_lock);
	pending = kfifo_len(&stp->rings[type]) > 0;
	mutex_unlock(&stp->rx_lock);

	return pending;
}

void connsys_stp_flush(struct connsys_dev *cd, u8 type)
{
	struct stp_engine *stp = &cd->stp;

	if (!stp_type_valid(type))
		return;

	mutex_lock(&stp->rx_lock);
	kfifo_reset(&stp->rings[type]);
	mutex_unlock(&stp->rx_lock);
}
