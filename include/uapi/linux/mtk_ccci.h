/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek MD1 cellular modem (ECCCI) - public UAPI.
 *
 * Numeric values are part of the Android userspace ABI (RILD, ccci_fsd,
 * mdlogger, META, the audio HAL) and must never change: they are copied from
 * the vendor <mt-plat/mt_ccci_common.h> shipped with the 3.18/4.9 trees.
 */
#ifndef _UAPI_LINUX_MTK_CCCI_H
#define _UAPI_LINUX_MTK_CCCI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define CCCI_IOC_MAGIC 'C'

#define CCCI_IOC_MD_RESET		_IO(CCCI_IOC_MAGIC, 0)
#define CCCI_IOC_GET_MD_STATE		_IOR(CCCI_IOC_MAGIC, 1, unsigned int)
#define CCCI_IOC_PCM_BASE_ADDR		_IOR(CCCI_IOC_MAGIC, 2, unsigned int)
#define CCCI_IOC_PCM_LEN		_IOR(CCCI_IOC_MAGIC, 3, unsigned int)
#define CCCI_IOC_FORCE_MD_ASSERT	_IO(CCCI_IOC_MAGIC, 4)
#define CCCI_IOC_ALLOC_MD_LOG_MEM	_IO(CCCI_IOC_MAGIC, 5)
#define CCCI_IOC_DO_MD_RST		_IO(CCCI_IOC_MAGIC, 6)
#define CCCI_IOC_SEND_RUN_TIME_DATA	_IO(CCCI_IOC_MAGIC, 7)
#define CCCI_IOC_GET_MD_INFO		_IOR(CCCI_IOC_MAGIC, 8, unsigned int)
#define CCCI_IOC_GET_MD_EX_TYPE		_IOR(CCCI_IOC_MAGIC, 9, unsigned int)
#define CCCI_IOC_SEND_STOP_MD_REQUEST	_IO(CCCI_IOC_MAGIC, 10)
#define CCCI_IOC_SEND_START_MD_REQUEST	_IO(CCCI_IOC_MAGIC, 11)
#define CCCI_IOC_DO_STOP_MD		_IO(CCCI_IOC_MAGIC, 12)
#define CCCI_IOC_DO_START_MD		_IO(CCCI_IOC_MAGIC, 13)
#define CCCI_IOC_ENTER_DEEP_FLIGHT	_IO(CCCI_IOC_MAGIC, 14)
#define CCCI_IOC_LEAVE_DEEP_FLIGHT	_IO(CCCI_IOC_MAGIC, 15)
#define CCCI_IOC_POWER_ON_MD		_IO(CCCI_IOC_MAGIC, 16)
#define CCCI_IOC_POWER_OFF_MD		_IO(CCCI_IOC_MAGIC, 17)
#define CCCI_IOC_POWER_ON_MD_REQUEST	_IO(CCCI_IOC_MAGIC, 18)
#define CCCI_IOC_POWER_OFF_MD_REQUEST	_IO(CCCI_IOC_MAGIC, 19)
#define CCCI_IOC_SIM_SWITCH		_IOW(CCCI_IOC_MAGIC, 20, unsigned int)
#define CCCI_IOC_SEND_BATTERY_INFO	_IO(CCCI_IOC_MAGIC, 21)
#define CCCI_IOC_SIM_SWITCH_TYPE	_IOR(CCCI_IOC_MAGIC, 22, unsigned int)
#define CCCI_IOC_STORE_SIM_MODE		_IOW(CCCI_IOC_MAGIC, 23, unsigned int)
#define CCCI_IOC_GET_SIM_MODE		_IOR(CCCI_IOC_MAGIC, 24, unsigned int)
#define CCCI_IOC_RELOAD_MD_TYPE		_IO(CCCI_IOC_MAGIC, 25)
#define CCCI_IOC_GET_SIM_TYPE		_IOR(CCCI_IOC_MAGIC, 26, unsigned int)
#define CCCI_IOC_ENABLE_GET_SIM_TYPE	_IOW(CCCI_IOC_MAGIC, 27, unsigned int)
#define CCCI_IOC_SEND_ICUSB_NOTIFY	_IOW(CCCI_IOC_MAGIC, 28, unsigned int)
#define CCCI_IOC_SET_MD_IMG_EXIST	_IOW(CCCI_IOC_MAGIC, 29, unsigned int)
#define CCCI_IOC_GET_MD_IMG_EXIST	_IOR(CCCI_IOC_MAGIC, 30, unsigned int)
#define CCCI_IOC_GET_MD_TYPE		_IOR(CCCI_IOC_MAGIC, 31, unsigned int)
#define CCCI_IOC_STORE_MD_TYPE		_IOW(CCCI_IOC_MAGIC, 32, unsigned int)
#define CCCI_IOC_GET_MD_TYPE_SAVING	_IOR(CCCI_IOC_MAGIC, 33, unsigned int)
#define CCCI_IOC_GET_EXT_MD_POST_FIX	_IOR(CCCI_IOC_MAGIC, 34, unsigned int)
#define CCCI_IOC_FORCE_FD		_IOW(CCCI_IOC_MAGIC, 35, unsigned int)
#define CCCI_IOC_AP_ENG_BUILD		_IOW(CCCI_IOC_MAGIC, 36, unsigned int)
#define CCCI_IOC_GET_MD_MEM_SIZE	_IOR(CCCI_IOC_MAGIC, 37, unsigned int)
#define CCCI_IOC_UPDATE_SIM_SLOT_CFG	_IOW(CCCI_IOC_MAGIC, 38, unsigned int)
#define CCCI_IOC_GET_CFG_SETTING	_IOW(CCCI_IOC_MAGIC, 39, unsigned int)
#define CCCI_IOC_SET_MD_SBP_CFG		_IOW(CCCI_IOC_MAGIC, 40, unsigned int)
#define CCCI_IOC_GET_MD_SBP_CFG		_IOW(CCCI_IOC_MAGIC, 41, unsigned int)
#define CCCI_IOC_GET_MD_PROTOCOL_TYPE	_IOR(CCCI_IOC_MAGIC, 42, char[16])
#define CCCI_IOC_SEND_SIGNAL_TO_USER	_IOW(CCCI_IOC_MAGIC, 43, unsigned int)
#define CCCI_IOC_RESET_MD1_MD3_PCCIF	_IO(CCCI_IOC_MAGIC, 45)
#define CCCI_IOC_RESET_AP		_IOW(CCCI_IOC_MAGIC, 46, unsigned int)

/* md_init sets the MD boot environment before power on */
#define CCCI_IOC_SET_BOOT_DATA		_IOW(CCCI_IOC_MAGIC, 47, unsigned int[16])

/* userspace shared memory helper (ccci_fsd) */
#define CCCI_IOC_SMEM_BASE		_IOR(CCCI_IOC_MAGIC, 48, unsigned int)
#define CCCI_IOC_SMEM_LEN		_IOR(CCCI_IOC_MAGIC, 49, unsigned int)
#define CCCI_IOC_SMEM_TX_NOTIFY		_IOW(CCCI_IOC_MAGIC, 50, unsigned int)
#define CCCI_IOC_SMEM_RX_POLL		_IOR(CCCI_IOC_MAGIC, 51, unsigned int)
#define CCCI_IOC_SMEM_SET_STATE		_IOW(CCCI_IOC_MAGIC, 52, unsigned int)
#define CCCI_IOC_SMEM_GET_STATE		_IOR(CCCI_IOC_MAGIC, 53, unsigned int)

#define CCCI_IOC_SET_CCIF_CG		_IOW(CCCI_IOC_MAGIC, 54, unsigned int)
#define CCCI_IOC_SET_EFUN		_IOW(CCCI_IOC_MAGIC, 55, unsigned int)

#define CCCI_IOC_SET_HEADER		_IO(CCCI_IOC_MAGIC, 112)
#define CCCI_IOC_CLR_HEADER		_IO(CCCI_IOC_MAGIC, 113)
#define CCCI_IOC_DL_TRAFFIC_CONTROL	_IOW(CCCI_IOC_MAGIC, 119, unsigned int)

#define CCCI_IPC_MAGIC 'P'

#define CCCI_IPC_RESET_RECV		_IO(CCCI_IPC_MAGIC, 0)
#define CCCI_IPC_RESET_SEND		_IO(CCCI_IPC_MAGIC, 1)
#define CCCI_IPC_WAIT_MD_READY		_IO(CCCI_IPC_MAGIC, 2)
#define CCCI_IPC_KERN_WRITE_TEST	_IO(CCCI_IPC_MAGIC, 3)
#define CCCI_IPC_UPDATE_TIME		_IO(CCCI_IPC_MAGIC, 4)
#define CCCI_IPC_WAIT_TIME_UPDATE	_IO(CCCI_IPC_MAGIC, 5)
#define CCCI_IPC_UPDATE_TIMEZONE	_IO(CCCI_IPC_MAGIC, 6)

/* FS packet opcodes (see Documentation/userspace-api/mtk_ccci.rst) */
#define FS_CCCI_OP_OPEN		0x1001
#define FS_CCCI_OP_SEEK		0x1002
#define FS_CCCI_OP_READ		0x1003
#define FS_CCCI_OP_WRITE	0x1004
#define FS_CCCI_OP_CLOSE	0x1005
#define FS_CCCI_OP_CLOSE_ALL	0x1006
#define FS_CCCI_OP_CREATEDIR	0x1007
#define FS_CCCI_OP_REMOVEDIR	0x1008
#define FS_CCCI_OP_GETFILESIZE	0x1009
#define FS_CCCI_OP_GETFOLDER	0x100a
#define FS_CCCI_OP_RENAME	0x100b
#define FS_CCCI_OP_DELETE	0x100c
#define FS_CCCI_OP_COUNT	0x100d
#define FS_CCCI_OP_GETDISKINFO	0x100e
#define FS_CCCI_OP_XDELETE	0x100f
#define FS_CCCI_OP_OPENHINT	0x1010
#define FS_CCCI_OP_FINDFIRST	0x1011
#define FS_CCCI_OP_RESTORE	0x1012
#define FS_CCCI_OP_CMPT_READ	0x1013
#define FS_CCCI_OP_OTP_READ	0x1014
#define FS_CCCI_OP_CMPT_READ_V2	0x1015

#define FS_MAX_OPEN_FILES	64
#define FS_MAX_BUF_SIZE		(16 * 1024)
#define FS_MAX_ARG_NUM		6

/* FS stream packet: 16 byte ccci header then this payload. */
struct fs_stream_pkt {
	struct {
		__u32 data[2];
		__u16 channel;
		__u16 seq_num;
		__u32 reserved;
	} ccci_header;
	__u32 buffer_slot;
	__u32 op_id;
	__u32 arg_num;
	__u8 payload[FS_MAX_BUF_SIZE];
} __attribute__((packed));

#endif /* _UAPI_LINUX_MTK_CCCI_H */
