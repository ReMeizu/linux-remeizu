/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek MD1 cellular modem - CLDMA/CCIF register map.
 *
 * Every SoC in scope (MT6735/MT6737M, MT6750/MT6753/MT6755, MT6797) uses the
 * same CLDMA IP with the same register map. Only the physical base addresses
 * and the interrupt lines differ, and those come from the device tree.
 */
#ifndef __CCCI_CLDMA_REG_H__
#define __CCCI_CLDMA_REG_H__

/* AP ring descriptors, AP -> MD. */
#define CLDMA_UL_VERSION		0x0000
#define CLDMA_UL_START_ADDR(q)		(0x0004 + 4 * (q))
#define CLDMA_UL_CURRENT_ADDR(q)	(0x0028 + 4 * (q))
#define CLDMA_UL_STATUS			0x0050
#define CLDMA_UL_START_CMD		0x0054
#define CLDMA_UL_RESUME_CMD		0x0058
#define CLDMA_UL_STOP_CMD		0x005C
#define CLDMA_UL_ERROR			0x0060
#define CLDMA_UL_CFG			0x0074
#define CLDMA_UL_HPQTCR			0x0110
#define CLDMA_UL_LPQTCR			0x0114
#define CLDMA_UL_HPQR			0x0118
#define CLDMA_UL_TCR(q)			(0x011C + 4 * (q))
#define CLDMA_UL_TCR_CMD		0x013C
#define CLDMA_UL_LAST_UPDATE_ADDR(q)	(0x0144 + 4 * (q))

/* AP ring descriptors, MD -> AP. */
#define CLDMA_SO_VERSION		0x0200
#define CLDMA_SO_ERROR			0x0300
#define CLDMA_SO_START_ADDR(q)		(0x0A78 + 4 * (q))
#define CLDMA_SO_CURRENT_ADDR(q)	(0x0A98 + 4 * (q))
#define CLDMA_SO_STATUS			0x0AB8
#define CLDMA_SO_START_CMD		0x03BC
#define CLDMA_SO_RESUME_CMD		0x03C0
#define CLDMA_SO_STOP_CMD		0x03C4
#define CLDMA_SO_LAST_UPDATE_ADDR(q)	(0x0AD0 + 4 * (q))
#define CLDMA_SO_OUTDMA_CODA_VERSION	0x0200

/* Interrupt and arbitration registers. */
#define CLDMA_CODA_VERSION		0x0400
#define CLDMA_L2TISAR0			0x0410
#define CLDMA_L2TISAR1			0x0414
#define CLDMA_L2TIMR0			0x0418
#define CLDMA_L2TIMR1			0x041C
#define CLDMA_L2TIMCR0			0x0420
#define CLDMA_L2TIMCR1			0x0424
#define CLDMA_L2TIMSR0			0x0428
#define CLDMA_L2TIMSR1			0x042C
#define CLDMA_L3TISAR0			0x0430
#define CLDMA_L3TISAR1			0x0434
#define CLDMA_L3TIMR0			0x0438
#define CLDMA_L3TIMR1			0x043C
#define CLDMA_L3TIMCR0			0x0440
#define CLDMA_L3TIMCR1			0x0444
#define CLDMA_L3TIMSR0			0x0448
#define CLDMA_L3TIMSR1			0x044C
#define CLDMA_L2RISAR0			0x0450
#define CLDMA_L2RISAR1			0x0454
#define CLDMA_L3RISAR0			0x0470
#define CLDMA_L3RISAR1			0x0474
#define CLDMA_L3RIMR0			0x0478
#define CLDMA_L3RIMR1			0x047C
#define CLDMA_L3RIMCR0			0x0480
#define CLDMA_L3RIMCR1			0x0484
#define CLDMA_L3RIMSR0			0x0488
#define CLDMA_L3RIMSR1			0x048C
#define CLDMA_BUS_STA			0x049C
#define CLDMA_BUS_DBG			0x04A0
#define CLDMA_CHNL_IDLE			0x04B0
#define CLDMA_IP_BUSY			0x04B4

/*
 * Control register at 0x500 of each window. It latches the reset/gating
 * value of the block, the bits are write-one-to-clear so a value of 0b10 in
 * the "reset" bit field releases the block.
 */
#define CLDMA_MD_CTRL			0x0500
#define CLDMA_MD_BUS_STATUS		0x0554

/* MD side ring base registers, written by the AP when the rings move. */
#define CLDMA_MD_UL_START_ADDR(q)	(0x0004 + 4 * (q))
#define CLDMA_MD_SO_START_ADDR(q)	(0x0A78 + 4 * (q))

/* Interrupt status bits. The low byte of each nibble carries one queue. */
#define CLDMA_INT_DONE			0x000000ff
#define CLDMA_INT_QUEUE_EMPTY		0x0000ff00
#define CLDMA_INT_ERROR			0x00ff0000
#define CLDMA_INT_ACTIVE_START		0xff000000
#define CLDMA_INT_ALL			0xffffffff

/* Level 3 error classes, one bit per queue in every byte lane. */
#define CLDMA_L3_ERR_BD_LEN		0xff000000
#define CLDMA_L3_ERR_GPD_LEN		0x00ff0000
#define CLDMA_L3_ERR_BD_CS		0x0000ff00
#define CLDMA_L3_ERR_GPD_CS		0x000000ff
#define CLDMA_L3_ERR_MASK		0xffffff00

/* Traffic control registers only understand these values. */
#define CLDMA_TC_DISABLE		0
#define CLDMA_TC_ENABLE			1
#define CLDMA_TC_ENABLE_MARK		0x80	/* high byte: "commit" marker */

/* CCIF, AP and MD views share the same offsets. */
#define CCIF_CON			0x00
#define CCIF_BUSY			0x04
#define CCIF_START			0x08
#define CCIF_TCHNUM			0x0c
#define CCIF_RCHNUM			0x10
#define CCIF_ACK			0x14
#define CCIF_CHDATA			0x100

/* CCIF CON bits */
#define CCIF_CON_TX_START		BIT(0)
#define CCIF_CON_TX_IRQ			BIT(1)
#define CCIF_CON_RX_IRQ			BIT(2)
#define CCIF_CON_TX_ACK			BIT(3)
#define CCIF_CON_RX_ACK			BIT(4)
#define CCIF_CON_RESET			BIT(31)

#endif /* __CCCI_CLDMA_REG_H__ */
