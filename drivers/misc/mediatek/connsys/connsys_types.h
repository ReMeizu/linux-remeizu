/* SPDX-License-Identifier: GPL-2.0-only */
/* MediaTek CONSYS - shared constants, SoC descriptions and device state. */
#ifndef _MTK_CONNSYS_TYPES_H
#define _MTK_CONNSYS_TYPES_H

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <uapi/linux/mtk_connsys.h>

struct stp_engine;
struct wmt_engine;

/* Chip ids reported by COMBO_IOCTL_GET_SOC_CHIP_ID and WMT_IOCTL_*_CHIPID */
#define CONNSYS_CHIP_MT6735		0x6735
#define CONNSYS_CHIP_MT6735M		0x0335
#define CONNSYS_CHIP_MT6735S		0x0337
#define CONNSYS_CHIP_MT6755		0x6755
#define CONNSYS_CHIP_MT6755M		0x0326
#define CONNSYS_CHIP_MT6750		0x6750
#define CONNSYS_CHIP_MT6797		0x6797

/* Firmware families */
enum connsys_fw_family {
	CONNSYS_FW_FAMILY_MT6735,	/* MT6735 / MT6735M / MT6737M	  */
	CONNSYS_FW_FAMILY_MT6755,	/* MT6750 / MT6755		  */
	CONNSYS_FW_FAMILY_MT6797,	/* MT6797 (MT6797X, Helio X2x)	  */
};

/**
 * struct connsys_soc_data - per-SoC register layout and firmware naming
 *
 * @name:        SoC name used in log messages
 * @chip_id:     chip id reported to userspace
 * @family:      firmware file family (ROMv2 for MT6735/MT6755, ROMv3 for MT6797)
 * @topcg_off:   offset of the CONN clock-gate register in topckgen
 * @topcg_bit:   clock-gate bit of the CONN clock inside topckgen
 * @ap2conn_off: offset of the AP2CONN oscillator-control register
 * @ap2conn_bit: bit driving AP2CONN_OSC_EN
 * @emi_map_off: offset of the CONSYS EMI base remap register
 * @emi_map_shift: shift of the remapped base field
 * @emi_map_mask: width mask of the remapped base field
 * @axi_prot_en_off: AXI protection enable register offset
 * @axi_prot_sta_off: AXI protection status register offset
 * @axi_prot_mask: protected AXI client bits
 * @wd_reset_bit: watchdog system-reset bit
 * @conn2ap_mask_off: CONN2AP sleep mask, 0 when unused
 * @conn2ap_mask_bit: CONN2AP sleep mask bit
 * @rom_patch:   MCU ROM patch file name requested by the kernel
 * @firmware:    Wi-Fi RAM code file name
 */
struct connsys_soc_data {
	const char *name;
	u32 chip_id;
	enum connsys_fw_family family;
	u32 topcg_off;
	u32 topcg_bit;
	u32 ap2conn_off;
	u32 ap2conn_bit;
	u32 emi_map_off;
	u32 emi_map_shift;
	u32 emi_map_mask;
	u32 axi_prot_en_off;
	u32 axi_prot_sta_off;
	u32 axi_prot_mask;
	u32 wd_reset_bit;
	u32 conn2ap_mask_off;
	u32 conn2ap_mask_bit;
	const char *rom_patch;
	const char *firmware;
};

/* Register block indices for the "reg"/"reg-names" binding */
enum connsys_reg_block {
	CONNSYS_REG_MCU = 0,		/* CONN MCU configuration	  */
	CONNSYS_REG_AP_RGU,		/* AP reset generator unit	  */
	CONNSYS_REG_TOPCKGEN,		/* top level clock gate		  */
	CONNSYS_REG_SPM,		/* sleep power manager		  */
	CONNSYS_REG_AXI,		/* AXI protection (CONN)	  */
	CONNSYS_REG_EMI,		/* reserved shared DRAM		  */
	CONNSYS_REG_MAX,
};

#define CONNSYS_BTIF_TX_FIFO	16
#define CONNSYS_BTIF_RX_FIFO	8
#define CONNSYS_BTIF_MAX_LEN	2048

/* STP transport modes (WMT_IOCTL_SET_STP_MODE) */
enum connsys_stp_mode {
	CONNSYS_STP_NONE = 0,
	CONNSYS_STP_BTIF_FULL,
	CONNSYS_STP_BTIF_MAND,
};

/* Reasons handed to the userspace launcher through read(/dev/stpwmt) */
enum connsys_launch_cmd {
	CONNSYS_LAUNCH_NONE = 0,
	CONNSYS_LAUNCH_SEARCH_PATCH,
	CONNSYS_LAUNCH_POWER_ON,
	CONNSYS_LAUNCH_POWER_OFF,
	CONNSYS_LAUNCH_RESET,
	CONNSYS_LAUNCH_MAX,
};

static inline const char *connsys_launch_name(enum connsys_launch_cmd cmd)
{
	switch (cmd) {
	case CONNSYS_LAUNCH_SEARCH_PATCH:
		return "srh_patch";
	case CONNSYS_LAUNCH_POWER_ON:
		return "pwr_on";
	case CONNSYS_LAUNCH_POWER_OFF:
		return "pwr_off";
	case CONNSYS_LAUNCH_RESET:
		return "chip_rst";
	default:
		return "";
	}
}

#endif /* _MTK_CONNSYS_TYPES_H */
