/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Meizu Linux mainline project
 *
 * UAPI definitions for the MediaTek CONSYS (Wi-Fi/BT/GPS combo subsystem)
 * driver stack exported through /dev/wmtdetect and /dev/stpwmt.
 *
 * The ioctl numbers, argument layout and semantics are the ones the Android
 * vendor userspace (wmt_loader, 6620_launcher and their open-source
 * re-implementation) expects; they were recovered from those binaries and
 * are kept bit for bit identical here so that unmodified vendor blobs work.
 */

#ifndef _UAPI_LINUX_MTK_CONNSYS_H
#define _UAPI_LINUX_MTK_CONNSYS_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ------------------------------------------------------------------ */
/* /dev/wmtdetect                                                      */
/* ------------------------------------------------------------------ */

struct mtk_connsys_chip_id {
	__s32 chip_id;
};

#define WMT_DETECT_IOC_MAGIC		'w'

/* Query the chip id of an external (SDIO) combo chip. */
#define COMBO_IOCTL_GET_CHIP_ID		_IOR(WMT_DETECT_IOC_MAGIC, 0, int)
/* Tell the kernel which chip id was detected. */
#define COMBO_IOCTL_SET_CHIP_ID		_IOW(WMT_DETECT_IOC_MAGIC, 1, int)
/* Probe for an external combo chip. 0 means "external chip absent". */
#define COMBO_IOCTL_EXT_CHIP_DETECT	_IOR(WMT_DETECT_IOC_MAGIC, 2, int)
/* Return the CONNSYS chip id for the SoC the kernel runs on. */
#define COMBO_IOCTL_GET_SOC_CHIP_ID	_IOR(WMT_DETECT_IOC_MAGIC, 3, int)
/*
 * Bring the whole connectivity stack up for the given chip id: map CONSYS
 * registers, request the BGF watchdog and BTIF interrupts, enable the clocks
 * and arms the launcher handshake.
 */
#define COMBO_IOCTL_DO_MODULE_INIT	_IOR(WMT_DETECT_IOC_MAGIC, 4, int)
/* Tear the stack down again. */
#define COMBO_IOCTL_MODULE_CLEANUP	_IOR(WMT_DETECT_IOC_MAGIC, 5, int)
/* Power up an external combo chip (always fails on integrated CONNSYS). */
#define COMBO_IOCTL_EXT_CHIP_PWR_ON	_IOR(WMT_DETECT_IOC_MAGIC, 6, int)
/* Power down an external combo chip. */
#define COMBO_IOCTL_EXT_CHIP_PWR_OFF	_IOR(WMT_DETECT_IOC_MAGIC, 7, int)
/* Legacy SDIO audio-detect autok; a no-op on integrated parts. */
#define COMBO_IOCTL_DO_SDIO_AUDS	_IOR(WMT_DETECT_IOC_MAGIC, 8, int)
#define COMBO_IOCTL_DO_SDIO_AUDOK	COMBO_IOCTL_DO_SDIO_AUDS

/* ------------------------------------------------------------------ */
/* /dev/stpwmt                                                         */
/* ------------------------------------------------------------------ */

#define WMT_IOC_MAGIC			0xa0

/* Maximum number of firmware patch segments the launcher may register. */
#define WMT_MAX_PATCH_NUM		10
/* Patch fragment payload size used by the MCU download protocol. */
#define WMT_PATCH_FRAG_SIZE		1000

/*
 * struct mtk_wmt_patch_info - firmware patch descriptor
 *
 * @download_seq: 1-based download order of the segment
 * @address:      four "address" bytes sent to the firmware before the body
 * @patch_name:   firmware file name, relative to the firmware search path
 */
struct mtk_wmt_patch_info {
	__s32 download_seq;
	__u8 address[4];
	char patch_name[256];
};

#define WMT_IOCTL_SET_PATCH_NAME	_IOW(WMT_IOC_MAGIC, 4, char *)
#define WMT_IOCTL_SET_STP_MODE		_IOW(WMT_IOC_MAGIC, 5, int)
#define WMT_IOCTL_FUNC_ONOFF_CTRL	_IOW(WMT_IOC_MAGIC, 6, int)
#define WMT_IOCTL_LPBK_POWER_CTRL	_IOW(WMT_IOC_MAGIC, 7, int)
#define WMT_IOCTL_LPBK_TEST		_IOWR(WMT_IOC_MAGIC, 8, char *)
#define WMT_IOCTL_GET_CHIP_INFO		_IOR(WMT_IOC_MAGIC, 12, int)
#define WMT_IOCTL_SET_LAUNCHER_KILL	_IOW(WMT_IOC_MAGIC, 13, int)
#define WMT_IOCTL_SET_PATCH_NUM		_IOW(WMT_IOC_MAGIC, 14, int)
#define WMT_IOCTL_SET_PATCH_INFO	_IOW(WMT_IOC_MAGIC, 15, char *)
#define WMT_IOCTL_PORT_NAME		_IOWR(WMT_IOC_MAGIC, 20, char *)
#define WMT_IOCTL_WMT_CFG_NAME		_IOWR(WMT_IOC_MAGIC, 21, char *)
#define WMT_IOCTL_WMT_QUERY_CHIPID	_IOR(WMT_IOC_MAGIC, 22, int)
#define WMT_IOCTL_WMT_TELL_CHIPID	_IOW(WMT_IOC_MAGIC, 23, int)
#define WMT_IOCTL_WMT_COREDUMP_CTRL	_IOW(WMT_IOC_MAGIC, 24, int)
#define WMT_IOCTL_SEND_BGW_DS_CMD	_IOW(WMT_IOC_MAGIC, 25, char *)
#define WMT_IOCTL_ADIE_LPBK_TEST	_IOWR(WMT_IOC_MAGIC, 26, char *)
#define WMT_IOCTL_GET_APO_FLAG		_IOR(WMT_IOC_MAGIC, 28, int)

/* WMT_IOCTL_GET_CHIP_INFO selectors */
#define WMTCHIN_CHIPID			0
#define WMTCHIN_HWVER			1
#define WMTCHIN_MAPPINGHWVER		2
#define WMTCHIN_FWVER			3

/* WMT_IOCTL_SET_STP_MODE values: low nibble is the HIF, high nibble the mode */
#define WMT_STP_MODE_UART		0x00
#define WMT_STP_MODE_SDIO		0x01
#define WMT_STP_MODE_BTIF		0x02
#define WMT_STP_MODE_BTIF_FULL		0x03	/* MediaTek launcher BTIF full */
#define WMT_STP_MODE_FULL		0x10
#define WMT_STP_MODE_MAND		0x20

/* WMT_IOCTL_FUNC_ONOFF_CTRL: high bit selects on/off, low bits the function */
#define WMT_FUNC_CTRL_ON_FLAG		0x80000000
#define WMT_FUNC_MASK			0xF

/* Logical functions served by CONSYS */
#define WMT_FUNC_BT			0
#define WMT_FUNC_FM			1
#define WMT_FUNC_GPS			2
#define WMT_FUNC_WIFI			3
#define WMT_FUNC_WMT			4
#define WMT_FUNC_STP			5
#define WMT_FUNC_LPBK			6
#define WMT_FUNC_COREDUMP		7
#define WMT_FUNC_MAX			8

#endif /* _UAPI_LINUX_MTK_CONNSYS_H */
