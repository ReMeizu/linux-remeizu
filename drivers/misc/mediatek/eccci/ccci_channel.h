/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek MD1 cellular modem (ECCCI) - channel identifiers.
 *
 * Part of the kernel internal ABI: the numbers are used by the Android
 * userspace (RILD, ccci_fsd, mdlogger, META, audio HAL) and by the modem
 * firmware itself, so they are copied verbatim from the vendor
 * <mt-plat/mt_ccci_common.h>.
 */
#ifndef __CCCI_CHANNEL_H__
#define __CCCI_CHANNEL_H__

/* MD_SYS indices: internal numbering starts at zero, MD_SYS names at one. */
enum {
	MD_SYS1 = 0,
	MD_SYS2,
	MD_SYS3,
	MD_SYS4,
	MD_SYS5 = 4,
	MAX_MD_NUM
};

enum ccci_ch {
	CCCI_CONTROL_RX = 0,
	CCCI_CONTROL_TX = 1,
	CCCI_SYSTEM_RX = 2,
	CCCI_SYSTEM_TX = 3,
	CCCI_PCM_RX = 4,
	CCCI_PCM_TX = 5,
	CCCI_UART1_RX = 6,		/* META */
	CCCI_UART1_RX_ACK = 7,
	CCCI_UART1_TX = 8,
	CCCI_UART1_TX_ACK = 9,
	CCCI_UART2_RX = 10,		/* MUX / IMS */
	CCCI_UART2_RX_ACK = 11,
	CCCI_UART2_TX = 12,
	CCCI_UART2_TX_ACK = 13,
	CCCI_FS_RX = 14,
	CCCI_FS_TX = 15,
	CCCI_PMIC_RX = 16,
	CCCI_PMIC_TX = 17,
	CCCI_UEM_RX = 18,
	CCCI_UEM_TX = 19,
	CCCI_CCMNI1_RX = 20,
	CCCI_CCMNI1_RX_ACK = 21,
	CCCI_CCMNI1_TX = 22,
	CCCI_CCMNI1_TX_ACK = 23,
	CCCI_CCMNI2_RX = 24,
	CCCI_CCMNI2_RX_ACK = 25,
	CCCI_CCMNI2_TX = 26,
	CCCI_CCMNI2_TX_ACK = 27,
	CCCI_CCMNI3_RX = 28,
	CCCI_CCMNI3_RX_ACK = 29,
	CCCI_CCMNI3_TX = 30,
	CCCI_CCMNI3_TX_ACK = 31,
	CCCI_RPC_RX = 32,
	CCCI_RPC_TX = 33,
	CCCI_IPC_RX = 34,
	CCCI_IPC_RX_ACK = 35,
	CCCI_IPC_TX = 36,
	CCCI_IPC_TX_ACK = 37,
	CCCI_IPC_UART_RX = 38,
	CCCI_IPC_UART_RX_ACK = 39,
	CCCI_IPC_UART_TX = 40,
	CCCI_IPC_UART_TX_ACK = 41,
	CCCI_MD_LOG_RX = 42,
	CCCI_MD_LOG_TX = 43,
	/* 44..49 reserved for the ARM7 firmware */
	CCCI_IT_RX = 50,
	CCCI_IT_TX = 51,
	CCCI_IMSV_UL = 52,
	CCCI_IMSV_DL = 53,
	CCCI_IMSC_UL = 54,
	CCCI_IMSC_DL = 55,
	CCCI_IMSA_UL = 56,
	CCCI_IMSA_DL = 57,
	CCCI_IMSDC_UL = 58,
	CCCI_IMSDC_DL = 59,
	CCCI_ICUSB_RX = 60,
	CCCI_ICUSB_TX = 61,
	CCCI_LB_IT_RX = 62,
	CCCI_LB_IT_TX = 63,
	CCCI_CCMNI1_DL_ACK = 64,
	CCCI_CCMNI2_DL_ACK = 65,
	CCCI_CCMNI3_DL_ACK = 66,
	CCCI_STATUS_RX = 67,
	CCCI_STATUS_TX = 68,
	CCCI_CCMNI4_RX = 69,
	CCCI_CCMNI4_RX_ACK = 70,
	CCCI_CCMNI4_TX = 71,
	CCCI_CCMNI4_TX_ACK = 72,
	CCCI_CCMNI4_DLACK_RX = 73,
	CCCI_CCMNI5_RX = 74,
	CCCI_CCMNI5_RX_ACK = 75,
	CCCI_CCMNI5_TX = 76,
	CCCI_CCMNI5_TX_ACK = 77,
	CCCI_CCMNI5_DLACK_RX = 78,
	CCCI_CCMNI6_RX = 79,
	CCCI_CCMNI6_RX_ACK = 80,
	CCCI_CCMNI6_TX = 81,
	CCCI_CCMNI6_TX_ACK = 82,
	CCCI_CCMNI6_DLACK_RX = 83,
	CCCI_CCMNI7_RX = 84,
	CCCI_CCMNI7_RX_ACK = 85,
	CCCI_CCMNI7_TX = 86,
	CCCI_CCMNI7_TX_ACK = 87,
	CCCI_CCMNI7_DLACK_RX = 88,
	CCCI_CCMNI8_RX = 89,
	CCCI_CCMNI8_RX_ACK = 90,
	CCCI_CCMNI8_TX = 91,
	CCCI_CCMNI8_TX_ACK = 92,
	CCCI_CCMNI8_DLACK_RX = 93,
	CCCI_MDL_MONITOR_DL = 94,
	CCCI_MDL_MONITOR_UL = 95,
	CCCI_CCMNILAN_RX = 96,
	CCCI_CCMNILAN_RX_ACK = 97,
	CCCI_CCMNILAN_TX = 98,
	CCCI_CCMNILAN_TX_ACK = 99,
	CCCI_CCMNILAN_DLACK_RX = 100,
	/* C2K (MD3) only channels, kept so that the numbering stays identical */
	CCCI_C2K_PPP_DATA = 101,
	CCCI_C2K_AT = 102,
	CCCI_C2K_AT2 = 103,
	CCCI_C2K_AT3 = 104,
	CCCI_C2K_AT4 = 105,
	CCCI_C2K_LB_DL = 106,
	/* virtual channels, never present on the wire */
	CCCI_MONITOR_CH = 107,
	CCCI_DUMMY_CH = 108,
	CCCI_SMEM_CH = 109,

	CCCI_MONITOR_CH_ID = 0xf0000000,
	CCCI_FORCE_ASSERT_CH = 20090215,
	CCCI_INVALID_CH_ID = 0xffffffff,
};

#define CCCI_MAX_CH_NUM		111
#define CCCI_ASSERT_BIT		0x8000

/* Packet payload sizes, in bytes, header excluded. */
#define CCCI_MTU		3456
#define CCCI_NET_MTU		1500
#define CCCI_PKT_SIZE_MAX	(CCCI_HDR_SIZE + CCCI_MTU)

/**
 * struct ccci_msg - the 16 byte CCCI packet header
 * @data: data[1] is the total packet length, header included
 * @channel: logical channel number, one of the CCCI_CHANNEL values
 * @seq_num: 15 bit sequence number, bit 15 is the modem assert flag
 * @reserved: channel specific word, the data length for network frames
 */
struct ccci_msg {
	__le32 data[2];
	__le16 channel;
	__le16 seq_num;
	__le32 reserved;
} __packed;

#define CCCI_HDR_SIZE		((int)sizeof(struct ccci_msg))

#define CCCI_HDR_CHANNEL(hdr)	(le16_to_cpu((hdr)->channel))
#define CCCI_HDR_SET_CHANNEL(hdr, ch) \
	((hdr)->channel = cpu_to_le16((u16)(ch)))
#define CCCI_HDR_SEQ(hdr)	(le16_to_cpu((hdr)->seq_num) & 0x7fff)
#define CCCI_HDR_ASSERT(hdr)	(!!(le16_to_cpu((hdr)->seq_num) & CCCI_ASSERT_BIT))
#define CCCI_HDR_LEN(hdr)	(le32_to_cpu((hdr)->data[1]))
#define CCCI_HDR_SET_LEN(hdr, len) \
	((hdr)->data[1] = cpu_to_le32((u32)(len)))
#define CCCI_HDR_DATA_LEN(hdr)	(le32_to_cpu((hdr)->reserved))
#define CCCI_HDR_SET_DATA_LEN(hdr, len) \
	((hdr)->reserved = cpu_to_le32((u32)(len)))

/* Modem capability bits, from "mediatek,cldma_capability". */
#define MODEM_CAP_NAPI			BIT(0)
#define MODEM_CAP_TXBUSY_STOP		BIT(1)
#define MODEM_CAP_SGIO			BIT(2)
#define MODEM_CAP_CCMNI_DISABLE		BIT(16)
#define MODEM_CAP_DATA_ACK_DVD		BIT(17)
#define MODEM_CAP_CCMNI_SEQNO		BIT(18)
#define MODEM_CAP_CCMNI_IRAT		BIT(19)
#define MODEM_CAP_WORLD_PHONE		BIT(20)
#define MODEM_CAP_CCMNI_MQ		BIT(21)

/* CCCI error codes reported through ioctl and send paths. */
#define CCCI_ERR_MODULE_INIT_OK			0
#define CCCI_ERR_INIT_PLATFORM_FAIL		2
#define CCCI_ERR_FATAL_ERR			100
#define CCCI_ERR_MD_IN_RESET			104
#define CCCI_ERR_INVALID_PARAM			106
#define CCCI_ERR_HIF_NOT_POWER_ON		300
#define CCCI_ERR_CHANNEL_NUM_MIS_MATCH		301
#define CCCI_ERR_PORT_RX_FULL			302
#define CCCI_ERR_DROP_PACKET			303

/* Channels the modem uses to carry IP traffic (ccmni0..ccmni7). */
#define CCCI_CCMNI_NUM		8
#define CCCI_CCMNI_LAST_DL_CH	CCCI_CCMNILAN_DLACK_RX

static inline bool ccci_ch_is_net(int ch)
{
	return ch >= CCCI_CCMNI1_RX && ch <= CCCI_CCMNI8_DLACK_RX;
}

static inline bool ccci_ch_is_net_rx(int ch)
{
	return (ch >= CCCI_CCMNI1_RX && ch <= CCCI_CCMNI1_TX_ACK) ||
	       (ch >= CCCI_CCMNI2_RX && ch <= CCCI_CCMNI2_TX_ACK) ||
	       (ch >= CCCI_CCMNI3_RX && ch <= CCCI_CCMNI3_TX_ACK) ||
	       (ch >= CCCI_CCMNI4_RX && ch <= CCCI_CCMNI4_TX_ACK) ||
	       (ch >= CCCI_CCMNI5_RX && ch <= CCCI_CCMNI5_TX_ACK) ||
	       (ch >= CCCI_CCMNI6_RX && ch <= CCCI_CCMNI6_TX_ACK) ||
	       (ch >= CCCI_CCMNI7_RX && ch <= CCCI_CCMNI7_TX_ACK) ||
	       (ch >= CCCI_CCMNI8_RX && ch <= CCCI_CCMNI8_TX_ACK);
}

static inline bool ccci_ch_is_net_tx(int ch)
{
	return ccci_ch_is_net_rx(ch);
}

#endif /* __CCCI_CHANNEL_H__ */
