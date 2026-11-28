/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Meizu Linux mainline project
 *
 * STP engine internal interface.
 */

#ifndef _MTK_CONNSYS_STP_CORE_H
#define _MTK_CONNSYS_STP_CORE_H

#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "connsys_types.h"

#define STP_HDR_LEN		4
#define STP_CRC_LEN		2

/* STP logical channels */
enum connsys_stp_type {
	CONNSYS_STP_BT = 0,
	CONNSYS_STP_FM,
	CONNSYS_STP_GPS,
	CONNSYS_STP_WIFI,
	CONNSYS_STP_WMT,
	CONNSYS_STP_STP,
	CONNSYS_STP_INFO,
	CONNSYS_STP_ANT,
	CONNSYS_STP_MAX,
};

/* Parser states of the receive path */
enum stp_rx_state {
	STP_RX_SYNC,
	STP_RX_HEADER,
	STP_RX_DATA,
	STP_RX_CRC,
};

struct stp_parser {
	enum stp_rx_state state;
	u8 hdr[STP_HDR_LEN];
	size_t hdr_pos;
	u8 seq;
	u8 ack;
	u8 type;
	size_t len;
	size_t pos;
	u8 data[CONNSYS_BTIF_MAX_LEN];
	u8 crc[STP_CRC_LEN];
	size_t crc_pos;
};

/**
 * struct stp_engine - per-instance STP state
 *
 * @tx_buf:   request buffer shared by the whole suite, each request starts
 *            on a 16-byte aligned address so it can be handed to DMA users
 * @tx_len:   used bytes in @tx_buf
 * @tx_lock:  protects the request buffer and the sequence numbers
 * @barrier:  no TX while the launcher must take the boot decision
 * @seq:      next TX sequence number
 * @ack:      last sequence number acknowledged by the MCU
 * @inflight: number of unacknowledged packets in the window
 * @retries:  retransmissions since the last acknowledgement
 * @timer:    retransmission timer
 * @retry_work: deferred retransmission
 * @rx_work:  BTIF RX drain work
 * @rx_lock:  protects the rings
 * @rings:    length-prefixed payloads per STP channel
 * @rx_wq:    signalled when any ring receives data
 * @parser:   receive state machine
 * @wmt_wq:   control-event wait queue consumed by the WMT engine
 * @evt:      bytes of the last WMT event, for diagnostics
 */
struct stp_engine {
	u8 *tx_buf;
	size_t tx_len;
	spinlock_t tx_lock;
	bool barrier;
	u8 seq;
	u8 ack;
	unsigned int inflight;
	unsigned int retries;
	struct timer_list timer;
	struct work_struct retry_work;
	struct work_struct rx_work;
	struct mutex rx_lock;
	struct kfifo rings[CONNSYS_STP_MAX];
	wait_queue_head_t rx_wq;
	struct stp_parser parser;
	wait_queue_head_t wmt_wq;
};


#endif /* _MTK_CONNSYS_STP_CORE_H */
