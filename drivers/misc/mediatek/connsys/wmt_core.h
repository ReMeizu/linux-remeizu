/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Meizu Linux mainline project
 *
 * Internal WMT engine structures.
 */

#ifndef _MTK_CONNSYS_WMT_H
#define _MTK_CONNSYS_WMT_H

#include <linux/firmware.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "connsys_types.h"


/**
 * struct wmt_engine - state of the WMT command/event engine
 *
 * @evt_lock:  protects @evt / @evt_len / @evt_valid
 * @evt_wq:    signalled when a new event is available
 * @evt:       last event delivered by the MCU
 * @evt_len:   its length
 * @evt_valid: @evt holds an unconsumed event
 * @abort:     an in-flight exchange must be given up
 * @patches:   firmware segments registered by the launcher
 * @patch_info: launcher descriptors (name, RAM address, order)
 * @patch_count: number of registered segments
 * @cfg_name:  WMT configuration file name given by the launcher
 * @stp_mode:  selected STP mode (WMT_IOCTL_SET_STP_MODE)
 */
struct wmt_engine {
	spinlock_t evt_lock;
	wait_queue_head_t evt_wq;
	u8 evt[64];
	size_t evt_len;
	bool evt_valid;
	bool abort;

	const struct firmware *patches[WMT_MAX_PATCH_NUM];
	struct mtk_wmt_patch_info patch_info[WMT_MAX_PATCH_NUM];
	unsigned int patch_count;
	u8 hdr_len[WMT_MAX_PATCH_NUM];

	char cfg_name[64];
	u32 stp_mode;

	u32 region_base;
	u16 region_len;
};

#endif /* _MTK_CONNSYS_WMT_H */
