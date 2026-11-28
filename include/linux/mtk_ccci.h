/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek MD1 cellular modem - public kernel interface.
 *
 * This is the only header the rest of the kernel may include to talk to the
 * modem; everything else lives inside drivers/misc/mediatek/eccci.
 */
#ifndef _LINUX_MTK_CCCI_H
#define _LINUX_MTK_CCCI_H

#include <linux/types.h>

/**
 * enum ccci_md_state - modem life cycle state as seen by ports and userspace
 * @CCCI_STATE_INVALID: no modem object (about to be removed)
 * @CCCI_STATE_GATED: no traffic, MD held in reset
 * @CCCI_STATE_BOOT_WAITING_FOR_HS1: image loaded, waiting for MD handshake
 * @CCCI_STATE_BOOT_WAITING_FOR_HS2: handshake 1 done, waiting for handshake 2
 * @CCCI_STATE_READY: modem booted, all channels usable
 * @CCCI_STATE_EXCEPTION: MD asserted / watchdog fired, recovering
 * @CCCI_STATE_RESET: reset in progress
 */
enum ccci_md_state {
	CCCI_STATE_INVALID = 0,
	CCCI_STATE_GATED,
	CCCI_STATE_BOOT_WAITING_FOR_HS1,
	CCCI_STATE_BOOT_WAITING_FOR_HS2,
	CCCI_STATE_READY,
	CCCI_STATE_EXCEPTION,
	CCCI_STATE_RESET,
};

struct ccci_modem;

/* Modem life cycle, all of them are callable from atomic context. */
int ccci_md_power_on(struct ccci_modem *md);
int ccci_md_power_off(struct ccci_modem *md, const char *reason);
int ccci_md_reset(struct ccci_modem *md, const char *reason);

/* State query used by the char ports and by the debugfs file. */
enum ccci_md_state ccci_md_get_state(struct ccci_modem *md);
const char *ccci_md_state_name(enum ccci_md_state state);

#endif /* _LINUX_MTK_CCCI_H */
