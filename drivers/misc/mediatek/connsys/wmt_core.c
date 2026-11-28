/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Meizu Linux mainline project
 *
 * WMT command/event engine and MCU firmware downloader for MediaTek CONSYS.
 *
 * The WMT layer sits on top of STP and speaks the "MT66xx" command format
 * used by the MCU on MT6735/MT6755/MT6797:
 *
 *	cmd[0] = 0x01			(host -> MCU)
 *	cmd[1] = group / type
 *	cmd[2..3] = payload length, little endian (type byte + data)
 *	cmd[4] = type
 *	cmd[5..] = data
 *
 * and the MCU answers with an event:
 *
 *	evt[0] = 0x02			(MCU -> host)
 *	evt[1] = group / type
 *	evt[2..3] = payload length, little endian
 *	evt[4] = status (0 = success)
 *
 * The byte sequences of the power-on script (power-on DLM, MCU clock during
 * download, reset, coexistence, RF calibration, patch addressing, firmware
 * coredump setup and FM strap) are the ones the vendor driver sends, see
 * core/wmt_ic_soc.c of the downstream tree.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "connsys.h"
#include "wmt_core.h"

/* --- generic WMT commands ------------------------------------------- */
/* query the STP configuration of the MCU (option 0x04) */
static const u8 wmt_query_stp_cmd[] = { 0x01, 0x04, 0x01, 0x00, 0x04 };
/* set the STP configuration: full mode, window 0x0edf, 0x0168 timeout */
static const u8 wmt_set_stp_cmd[] = {
	0x01, 0x04, 0x05, 0x00, 0x03, 0xdf, 0x0e, 0x68, 0x01
};
static const u8 wmt_set_stp_evt[] = { 0x02, 0x04, 0x02, 0x00, 0x00, 0x03 };
/* reset the MCU (option 0x04 = keep the RF calibration) */
static const u8 wmt_reset_cmd[] = { 0x01, 0x07, 0x01, 0x00, 0x04 };
static const u8 wmt_reset_evt[] = { 0x02, 0x07, 0x01, 0x00, 0x00 };

/* --- power-on DLM sequence (group 0x08, register write helper) -------- */
/* power-on regulators: ISN1 lite on */
static const u8 wmt_power_dlm_cmd1[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x60, 0x00, 0x10, 0x80, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x0f, 0x00, 0x00,
};
/* power-on regulators: PU/PD switch */
static const u8 wmt_power_dlm_cmd2[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x60, 0x00, 0x10, 0x80, 0x00, 0x00, 0x00, 0x00,
	0xf0, 0x00, 0x00, 0x00,
};
/* power-on regulators: LDO / BT band-gap */
static const u8 wmt_power_dlm_cmd3[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x60, 0x00, 0x10, 0x80, 0x00, 0x00, 0x00, 0x00,
	0x08, 0x00, 0x00, 0x00,
};
static const u8 wmt_reg_write_evt[] = {
	0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01
};

/* --- MCU clock during patch download --------------------------------- */
/* enable software MCU clock control */
static const u8 wmt_mcu_clk_en_cmd[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x34, 0x03, 0x00, 0x80, 0x00, 0x00, 0x01, 0x00,
	0xff, 0xff, 0xff, 0xff,
};
/* 138.67 MHz while the download runs */
static const u8 wmt_mcu_clk_138_cmd[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x0c, 0x01, 0x00, 0x80, 0x59, 0x4d, 0x84, 0x00,
	0xff, 0xff, 0xff, 0xff,
};
/* back to 26 MHz */
static const u8 wmt_mcu_clk_26_cmd[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x0c, 0x01, 0x00, 0x80, 0x00, 0x4d, 0x84, 0x00,
	0xff, 0xff, 0xff, 0xff,
};
/* disable software MCU clock control */
static const u8 wmt_mcu_clk_dis_cmd[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x34, 0x03, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff,
};

/* --- Wi-Fi/LTE coexistence ------------------------------------------- */
/* filter specification for the 0x6752-class CONNSYS (MT6735/MT6755) */
static const u8 wmt_coex_filter_old_cmd[] = {
	0x01, 0x10, 0x45, 0x00, 0x11, 0x00, 0x00, 0x01,
	0x00, 0x11, 0x11, 0x16, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x63, 0x63, 0x63, 0x00, 0x39, 0x43, 0x63,
	0x63, 0x02, 0x02, 0x03, 0x00, 0x01, 0x01, 0x01,
	0x01, 0x0e, 0x0e, 0x0e, 0x00, 0x0a, 0x0c, 0x0e,
	0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00,
};
static const u8 wmt_coex_lte_idx_old_cmd[] = {
	0x01, 0x10, 0x21, 0x00, 0x12, 0xfc, 0x08, 0x15,
	0x09, 0x2e, 0x09, 0x47, 0x09, 0xc4, 0x09, 0xdd,
	0x09, 0xf6, 0x09, 0x0f, 0x0a, 0x14, 0x09, 0x2d,
	0x09, 0x46, 0x09, 0x5f, 0x09, 0xdd, 0x09, 0xf5,
	0x09, 0x0d, 0x0a, 0x27, 0x0a,
};
/* default filter specification for the other generations */
static const u8 wmt_coex_filter_def_cmd[] = {
	0x01, 0x10, 0x45, 0x00, 0x11, 0x00, 0x00, 0x01,
	0x00, 0x16, 0x16, 0x16, 0x16, 0x00, 0x00, 0x00,
	0x00, 0x63, 0x63, 0x63, 0x63, 0x3c, 0x3c, 0x3c,
	0x3c, 0x04, 0x04, 0x04, 0x04, 0x01, 0x01, 0x01,
	0x01, 0x0e, 0x0e, 0x0e, 0x0e, 0x0b, 0x0b, 0x0b,
	0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00,
};
static const u8 wmt_coex_lte_idx_def_cmd[] = {
	0x01, 0x10, 0x21, 0x00, 0x12, 0xfc, 0x08, 0x15,
	0x09, 0x2e, 0x09, 0x47, 0x09, 0xc4, 0x09, 0xdd,
	0x09, 0xf6, 0x09, 0x0f, 0x0a, 0x14, 0x09, 0x2d,
	0x09, 0x46, 0x09, 0x5f, 0x09, 0xdd, 0x09, 0xf5,
	0x09, 0x0d, 0x0a, 0x27, 0x0a,
};
/* "this project has an LTE modem" indication */
static const u8 wmt_coex_is_lte_cmd[] = { 0x01, 0x10, 0x02, 0x00, 0x15, 0x01 };
/* WMT-level coexistence configuration, value patched from WMT.cfg later */
static u8 wmt_coex_setting_cmd[] = { 0x01, 0x10, 0x02, 0x00, 0x01, 0x00 };
static const u8 wmt_coex_evt[] = { 0x02, 0x10, 0x01, 0x00, 0x00 };

/* --- RF calibration and firmware housekeeping ------------------------ */
static const u8 wmt_rf_calib_cmd[] = { 0x01, 0x14, 0x01, 0x00, 0x01 };
static const u8 wmt_rf_calib_evt[] = { 0x02, 0x14, 0x02, 0x00, 0x00, 0x01 };
/* co-clock (TCXO sharing with the modem) master mode */
static const u8 wmt_co_clock_cmd[] = { 0x01, 0x0a, 0x02, 0x00, 0x08, 0x03 };
static const u8 wmt_co_clock_evt[] = { 0x02, 0x0a, 0x01, 0x00, 0x00 };
/* FM strap: internal 0x02, I2C 0x01 */
static u8 wmt_fm_strap_cmd[] = { 0x01, 0x05, 0x02, 0x00, 0x02, 0x02 };
static u8 wmt_fm_strap_evt[] = { 0x02, 0x05, 0x02, 0x00, 0x00, 0x02 };
/* full firmware coredump on assert */
static const u8 wmt_coredump_cmd[] = {
	0x01, 0x0f, 0x07, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const u8 wmt_coredump_evt[] = { 0x02, 0x0f, 0x01, 0x00, 0x00 };

/* --- MCU patch download --------------------------------------------- */
static const u8 wmt_patch_cmd[] = { 0x01, 0x01, 0x00, 0x00, 0x00 };
static const u8 wmt_patch_evt[] = { 0x02, 0x01, 0x01, 0x00, 0x00 };
/*
 * "WMT part" of the patch: selects the patch RAM / PMEM region of the MCU,
 * cmd[8..9] hold the region base and cmd[10..11] its length.
 */
static const u8 wmt_patch_region_old_cmd[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x40, 0x02, 0x09, 0x02, 0xff, 0xff, 0xff, 0xff,
};
static const u8 wmt_patch_region_6797_cmd[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x08, 0x05, 0x0b, 0x02, 0xff, 0xff, 0xff, 0xff,
};
static const u8 wmt_patch_addr_evt[] = {
	0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01
};
/* "P" (second) part of the patch: RAM address of the segment */
static u8 wmt_patch_p_addr_cmd[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0xc4, 0x04, 0x09, 0x02, 0x00, 0x3f, 0x00, 0x01,
	0xff, 0xff, 0xff, 0xff,
};
static const u8 wmt_patch_p_addr_evt[] = {
	0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01
};

/*
 * MCU ROM patch file layout (verified against the Flyme MT6735 blobs,
 * ROMv2_lm_patch_1_0_hdr.bin / _1_1_hdr.bin):
 *
 *	+0x00  build date and time, ASCII "YYYYMMDDhhmmss", newline
 *	+0x10  platform, ASCII "1636" (MT6735)
 *	+0x14  hw version, big endian   (0x8a00)
 *	+0x16  sw version, big endian   (0x8a00)
 *	+0x18  patch version, big endian
 *	+0x1c  symbol type / queue id, inline patch body starts here
 *
 * Note there is no separate symbol-type field: the body follows the 26 byte
 * header directly.
 */
#define WMT_PATCH_HDR_LEN	26
#define WMT_PATCH_FRAG_1ST	0x1
#define WMT_PATCH_FRAG_MID	0x2
#define WMT_PATCH_FRAG_LAST	0x3

/* WMT event groups (evt[1] >> 4) */
#define WMT_EVT_GROUP_DBG	0x0e
#define WMT_EVT_GROUP_COREDUMP	0x0f
#define WMT_EVT_GROUP_PATCH	0x01

/* ------------------------------------------------------------------ */
/* Primitive transfer helpers                                          */
/* ------------------------------------------------------------------ */

/**
 * connsys_wmt_send - queue one command on the WMT channel
 */
int connsys_wmt_send(struct connsys_dev *cd, const u8 *cmd, size_t len)
{
	return connsys_stp_send(cd, CONNSYS_STP_WMT, cmd, len);
}

/**
 * connsys_wmt_recv - wait for and fetch the next WMT event
 *
 * Returns the event length, 0 on timeout or a negative errno when the
 * launcher died or the exchange was interrupted.
 */
int connsys_wmt_recv(struct connsys_dev *cd, u8 *buf, size_t len,
		     unsigned long timeout)
{
	struct wmt_engine *w = &cd->wmt;
	long wait;
	size_t ret;

	wait = wait_event_interruptible_timeout(w->evt_wq, w->evt_valid ||
						 cd->launcher_dead || w->abort,
						 timeout);
	if (!wait)
		return 0;
	if (cd->launcher_dead)
		return -ESHUTDOWN;
	if (w->abort)
		return -ECANCELED;

	spin_lock_bh(&w->evt_lock);
	ret = min(len, w->evt_len);
	memcpy(buf, w->evt, ret);
	w->evt_valid = false;
	w->evt_len = 0;
	spin_unlock_bh(&w->evt_lock);

	return ret;
}

void connsys_wmt_abort(struct connsys_dev *cd)
{
	cd->wmt.abort = true;
	wake_up_interruptible_all(&cd->wmt.evt_wq);
}

/**
 * connsys_wmt_rx - hand a received WMT event to the command engine
 *
 * Called from the STP RX path.
 */
void connsys_wmt_rx(struct connsys_dev *cd, const u8 *buf, size_t len)
{
	struct wmt_engine *w = &cd->wmt;

	if (len < 5)
		return;

	spin_lock_bh(&w->evt_lock);
	w->evt_len = min_t(size_t, len, sizeof(w->evt));
	memcpy(w->evt, buf, w->evt_len);
	w->evt_valid = true;
	spin_unlock_bh(&w->evt_lock);

	switch ((buf[1] >> 4) & 0x7) {
	case WMT_EVT_GROUP_COREDUMP:
		if (!cd->coredump) {
			cd->coredump = true;
			dev_err(cd->dev, "MCU reported a firmware coredump\n");
		}
		break;
	case WMT_EVT_GROUP_DBG:
		dev_dbg(cd->dev, "MCU debug event 0x%02x\n", buf[4]);
		break;
	default:
		break;
	}

	wake_up_interruptible_all(&w->evt_wq);
}

/**
 * connsys_wmt_send_script - run a table of command/response exchanges
 *
 * Each entry is sent and its expected event consumed.  Mismatches are
 * logged but do not abort the script, as the vendor driver keeps going for
 * the optional tables; transport errors do abort.
 */
int connsys_wmt_send_script(struct connsys_dev *cd,
			    const struct connsys_script *script, size_t n)
{
	u8 evt[64];
	size_t i;
	int ret, err = 0;

	memset(evt, 0, sizeof(evt));

	for (i = 0; i < n; i++) {
		ret = connsys_wmt_send(cd, script[i].cmd, script[i].cmd_len);
		if (ret < 0)
			return ret;

		ret = connsys_wmt_recv(cd, evt, sizeof(evt),
				       CONNSYS_CTRL_TIMEOUT);
		if (ret <= 0) {
			dev_err(cd->dev, "WMT %s: no event (%d)\n",
				script[i].name, ret);
			return ret ?: -ETIMEDOUT;
		}
		if (script[i].evt_len &&
		    (ret != script[i].evt_len ||
		     memcmp(evt, script[i].evt, ret))) {
			dev_warn(cd->dev,
				 "WMT %s: unexpected event %02x %02x %02x %02x\n",
				 script[i].name, evt[0], evt[1], evt[2], evt[3]);
			err = -EPROTO;
		}
	}

	return err;
}

/* ------------------------------------------------------------------ */
/* Register write helper                                              */
/* ------------------------------------------------------------------ */

/**
 * connsys_wmt_reg_write - write a MCU register through the command channel
 *
 * The MCU applies the masked write and answers with one register status
 * event.  Used for the RF front-end configuration (PALDO and AFE tuning).
 */
int connsys_wmt_reg_write(struct connsys_dev *cd, u32 addr, u32 val,
			  u32 mask, const char *name)
{
	u8 cmd[20] = {
		0x01, 0x08, 0x10, 0x00,	/* host -> MCU, group 0x08, 16 bytes */
		0x01, 0x01, 0x00, 0x01,	/* write, register, 1 register	 */
	};
	u8 evt[16];
	int ret;

	put_unaligned_le32(addr, cmd + 8);
	put_unaligned_le32(val, cmd + 12);
	put_unaligned_le32(mask, cmd + 16);

	ret = connsys_wmt_send(cd, cmd, sizeof(cmd));
	if (ret < 0)
		return ret;

	ret = connsys_wmt_recv(cd, evt, sizeof(evt), CONNSYS_CTRL_TIMEOUT);
	if (ret <= 0) {
		dev_err(cd->dev, "WMT reg %s: no event\n", name);
		return ret ?: -ETIMEDOUT;
	}

	return evt[4] ? -EIO : 0;
}

/* ------------------------------------------------------------------ */
/* Firmware patch handling                                            */
/* ------------------------------------------------------------------ */

/* A ROM patch begins with an ASCII "YYYYMMDD" build date. */
static bool is_rom_patch(const struct firmware *fw)
{
	unsigned int i;

	if (fw->size < WMT_PATCH_HDR_LEN)
		return false;
	for (i = 0; i < 8; i++)
		if (fw->data[i] < '0' || fw->data[i] > '9')
			return false;

	return true;
}

void connsys_patch_reset(struct connsys_dev *cd)
{
	struct wmt_engine *w = &cd->wmt;
	int i;

	for (i = 0; i < WMT_MAX_PATCH_NUM; i++) {
		connsys_release_firmware(w->patches[i]);
		w->patches[i] = NULL;
		w->hdr_len[i] = 0;
		memset(&w->patch_info[i], 0, sizeof(w->patch_info[i]));
	}
	w->patch_count = 0;
}

/**
 * connsys_patch_register - load and register one firmware patch segment
 *
 * Called from WMT_IOCTL_SET_PATCH_INFO.  The firmware is requested right
 * away so that a missing or broken file is reported to the launcher before
 * the download starts.
 */
const struct firmware *
connsys_patch_register(struct connsys_dev *cd, int seq, const char *name,
		       const u8 address[4])
{
	struct wmt_engine *w = &cd->wmt;
	struct mtk_wmt_patch_info *info;
	const struct firmware *fw;
	int ret;

	if (seq < 1 || seq > WMT_MAX_PATCH_NUM)
		return ERR_PTR(-EINVAL);

	if (!name[0] || !strnlen(name, sizeof(info->patch_name)))
		return ERR_PTR(-EINVAL);

	info = &w->patch_info[seq - 1];
	connsys_release_firmware(w->patches[seq - 1]);
	w->patches[seq - 1] = NULL;

	strscpy(info->patch_name, name, sizeof(info->patch_name));
	memcpy(info->address, address, sizeof(info->address));
	info->download_seq = seq;

	ret = connsys_request_firmware(cd, info->patch_name, &fw);
	if (ret)
		return ERR_PTR(ret);

	if (fw->size <= WMT_PATCH_HDR_LEN) {
		dev_err(cd->dev, "%s is too small to be a MCU patch\n",
			info->patch_name);
		connsys_release_firmware(fw);
		return ERR_PTR(-EINVAL);
	}

	/*
	 * A ROM patch starts with an ASCII build date; anything else (for
	 * instance the sectioned "MTKW" Wi-Fi RAM code image) is sent to
	 * the MCU verbatim, section table included.
	 */
	if (!is_rom_patch(fw)) {
		dev_info(cd->dev, "%s: raw firmware image, header kept\n",
			 info->patch_name);
	}

	w->patches[seq - 1] = fw;
	w->hdr_len[seq - 1] = is_rom_patch(fw) ? WMT_PATCH_HDR_LEN : 0;
	if (w->patch_count < WMT_MAX_PATCH_NUM)
		w->patch_count++;

	dev_info(cd->dev, "patch [%d] %s, %zu bytes\n", seq,
		 info->patch_name, fw->size);

	return fw;
}

static int connsys_patch_set_region(struct connsys_dev *cd)
{
	u8 cmd[sizeof(wmt_patch_region_old_cmd)];
	u8 evt[16];
	int ret, i;

	memcpy(cmd, cd->soc->family == CONNSYS_FW_FAMILY_MT6797 ?
	       wmt_patch_region_6797_cmd : wmt_patch_region_old_cmd,
	       sizeof(cmd));

	ret = connsys_wmt_send(cd, cmd, sizeof(cmd));
	if (ret < 0)
		return ret;

	ret = connsys_wmt_recv(cd, evt, sizeof(evt), CONNSYS_CTRL_TIMEOUT);
	if (ret != sizeof(wmt_patch_addr_evt) ||
	    memcmp(evt, wmt_patch_addr_evt, ret)) {
		dev_err(cd->dev, "patch region command rejected\n");
		return -EPROTO;
	}

	/* The vendor driver pushes four "address" vAddrs for BT, GPS,
	 * Wi-Fi and the WMT part itself; the MCU accepts them in order. */
	for (i = 0; i < 4; i++) {
		u8 addr = wmt_patch_p_addr_cmd[12 + 2] + i;

		wmt_patch_p_addr_cmd[12] = addr;
		ret = connsys_wmt_send(cd, wmt_patch_p_addr_cmd,
				       sizeof(wmt_patch_p_addr_cmd));
		if (ret < 0)
			return ret;
		ret = connsys_wmt_recv(cd, evt, sizeof(evt),
				       CONNSYS_CTRL_TIMEOUT);
		if (ret != sizeof(wmt_patch_p_addr_evt) ||
		    memcmp(evt, wmt_patch_p_addr_evt, ret)) {
			dev_err(cd->dev, "patch vAddr %d rejected\n", i);
			return -EPROTO;
		}
	}

	return 0;
}

/**
 * connsys_patch_download - push one firmware segment into the MCU RAM
 *
 * The segment is split in WMT_PATCH_FRAG_SIZE fragments, each prefixed by
 * the five-byte patch command (first/middle/last flag plus the fragment
 * length).  The MCU acknowledges every fragment.
 */
static int connsys_patch_download(struct connsys_dev *cd, int seq,
				  const struct firmware *fw)
{
	const struct mtk_wmt_patch_info *info = &cd->wmt.patch_info[seq];
	const u8 *body = fw->data + cd->wmt.hdr_len[seq];
	size_t size = fw->size - cd->wmt.hdr_len[seq];
	size_t offset;
	size_t frag_size;
	size_t frag;
	u16 cmd_len;
	u8 *frame, *evt;
	int ret = 0;

	frame = kmalloc(sizeof(wmt_patch_cmd) + WMT_PATCH_FRAG_SIZE, GFP_KERNEL);
	evt = kmalloc(16, GFP_KERNEL);
	if (!frame || !evt) {
		ret = -ENOMEM;
		goto out;
	}

	/* RAM address of this segment ("P" part of the patch) */
	memcpy(&wmt_patch_p_addr_cmd[12], info->address, 4);
	ret = connsys_wmt_send(cd, wmt_patch_p_addr_cmd,
			       sizeof(wmt_patch_p_addr_cmd));
	if (ret < 0)
		goto out;
	frag = connsys_wmt_recv(cd, evt, 16, CONNSYS_CTRL_TIMEOUT);
	if (frag != sizeof(wmt_patch_p_addr_evt) ||
	    memcmp(evt, wmt_patch_p_addr_evt, frag)) {
		dev_err(cd->dev, "patch %d RAM address rejected\n", seq);
		ret = -EPROTO;
		goto out;
	}

	for (offset = 0; offset < size; offset += frag_size) {
		frag_size = min_t(size_t, WMT_PATCH_FRAG_SIZE, size - offset);
		memcpy(frame, wmt_patch_cmd, sizeof(wmt_patch_cmd));
		cmd_len = frag_size + 1;
		put_unaligned_le16(cmd_len, frame + 2);
		frame[4] = (offset + frag_size >= size) ? WMT_PATCH_FRAG_LAST :
			   offset ? WMT_PATCH_FRAG_MID : WMT_PATCH_FRAG_1ST;
		memcpy(frame + sizeof(wmt_patch_cmd), body + offset, frag_size);

		ret = connsys_wmt_send(cd, frame,
				       sizeof(wmt_patch_cmd) + frag_size);
		if (ret < 0)
			break;

		frag = connsys_wmt_recv(cd, evt, 16, CONNSYS_CTRL_TIMEOUT);
		if (frag != sizeof(wmt_patch_evt) ||
		    memcmp(evt, wmt_patch_evt, frag)) {
			dev_err(cd->dev, "patch %d fragment at 0x%zx rejected\n",
				seq, offset);
			ret = -EPROTO;
			break;
		}
		ret = 0;
	}

	if (!ret)
		dev_info(cd->dev, "patch %d: %zu bytes downloaded\n", seq,
			 size);

out:
	kfree(frame);
	kfree(evt);
	return ret;
}

int connsys_patch_load(struct connsys_dev *cd)
{
	struct connsys_script table[2];
	struct wmt_engine *w = &cd->wmt;
	int i, ret;

	if (!w->patch_count) {
		dev_warn(cd->dev, "no MCU patch registered\n");
		return -ENOENT;
	}

	ret = connsys_patch_set_region(cd);
	if (ret)
		return ret;

	/* speed up the download: MCU PLL to 138.67 MHz */
	table[0] = (struct connsys_script) {
		wmt_mcu_clk_en_cmd, sizeof(wmt_mcu_clk_en_cmd),
		wmt_reg_write_evt, sizeof(wmt_reg_write_evt), "mcu clk en",
	};
	table[1] = (struct connsys_script) {
		wmt_mcu_clk_138_cmd, sizeof(wmt_mcu_clk_138_cmd),
		wmt_reg_write_evt, sizeof(wmt_reg_write_evt), "mcu clk 138M",
	};
	ret = connsys_wmt_send_script(cd, table, ARRAY_SIZE(table));
	if (ret)
		dev_warn(cd->dev, "MCU clock setup failed: %d\n", ret);

	for (i = 0; i < w->patch_count; i++) {
		if (!w->patches[i])
			continue;
		ret = connsys_patch_download(cd, i, w->patches[i]);
		if (ret)
			return ret;
	}

	table[0] = (struct connsys_script) {
		wmt_mcu_clk_26_cmd, sizeof(wmt_mcu_clk_26_cmd),
		wmt_reg_write_evt, sizeof(wmt_reg_write_evt), "mcu clk 26M",
	};
	table[1] = (struct connsys_script) {
		wmt_mcu_clk_dis_cmd, sizeof(wmt_mcu_clk_dis_cmd),
		wmt_reg_write_evt, sizeof(wmt_reg_write_evt), "mcu clk off",
	};
	ret = connsys_wmt_send_script(cd, table, ARRAY_SIZE(table));
	if (ret)
		dev_warn(cd->dev, "MCU clock restore failed: %d\n", ret);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Power-on script                                                    */
/* ------------------------------------------------------------------ */

static int connsys_wmt_query_stp(struct connsys_dev *cd)
{
	u8 evt[16];
	int ret;

	ret = connsys_wmt_send(cd, wmt_query_stp_cmd, sizeof(wmt_query_stp_cmd));
	if (ret < 0)
		return ret;

	ret = connsys_wmt_recv(cd, evt, sizeof(evt), CONNSYS_CTRL_TIMEOUT);
	if (ret <= 0) {
		dev_err(cd->dev, "MCU does not answer the STP query\n");
		return ret ?: -ETIMEDOUT;
	}
	if (evt[1] != 0x04) {
		dev_err(cd->dev, "unexpected STP query answer 0x%02x\n", evt[1]);
		return -EPROTO;
	}

	return 0;
}

/**
 * connsys_wmt_power_on - bring the MCU up
 *
 * Sequence (vendor mtk_wcn_soc_sw_init):
 *   1. query the STP options, program full STP mode
 *   2. power-on DLM commands
 *   3. download the ROM patches
 *   4. MCU reset
 *   5. Wi-Fi/LTE coexistence tables
 *   6. RF calibration
 *   7. coredump level, WMT coexistence, co-clock and FM strap
 */
int connsys_wmt_power_on(struct connsys_dev *cd)
{
	struct connsys_script table[4];
	struct device_node *np = cd->dev->of_node;
	u32 fm_strap = 2;
	int ret;

	lockdep_assert_held(&cd->lock);

	if (cd->state != CONNSYS_POWER_ON)
		return -ENODEV;
	if (cd->pwr_on_done)
		return 0;

	connsys_wmt_abort_reset(cd);
	connsys_stp_reset(cd);

	ret = connsys_btif_open(cd);
	if (ret)
		return ret;

	/* 1. STP configuration */
	ret = connsys_wmt_query_stp(cd);
	if (ret)
		goto err_out;

	table[0] = (struct connsys_script) {
		wmt_set_stp_cmd, sizeof(wmt_set_stp_cmd),
		wmt_set_stp_evt, sizeof(wmt_set_stp_evt), "set stp",
	};
	ret = connsys_wmt_send_script(cd, table, 1);
	if (ret) {
		dev_err(cd->dev, "cannot enable full STP mode: %d\n", ret);
		goto err_out;
	}
	msleep(10);

	/* 2. power-on regulators */
	table[0] = (struct connsys_script) {
		wmt_power_dlm_cmd1, sizeof(wmt_power_dlm_cmd1),
		wmt_reg_write_evt, sizeof(wmt_reg_write_evt), "power dlm 1",
	};
	table[1] = (struct connsys_script) {
		wmt_power_dlm_cmd2, sizeof(wmt_power_dlm_cmd2),
		wmt_reg_write_evt, sizeof(wmt_reg_write_evt), "power dlm 2",
	};
	table[2] = (struct connsys_script) {
		wmt_power_dlm_cmd3, sizeof(wmt_power_dlm_cmd3),
		wmt_reg_write_evt, sizeof(wmt_reg_write_evt), "power dlm 3",
	};
	ret = connsys_wmt_send_script(cd, table, 3);
	if (ret)
		dev_warn(cd->dev, "power-on DLM failed: %d\n", ret);

	/* 3. ROM patches */
	ret = connsys_patch_load(cd);
	if (ret == -ENOENT)
		dev_warn(cd->dev, "running the MCU ROM without patch\n");
	else if (ret)
		goto err_out;

	/* 4. MCU reset */
	table[0] = (struct connsys_script) {
		wmt_reset_cmd, sizeof(wmt_reset_cmd),
		wmt_reset_evt, sizeof(wmt_reset_evt), "wmt reset",
	};
	ret = connsys_wmt_send_script(cd, table, 1);
	if (ret) {
		dev_err(cd->dev, "MCU reset failed: %d\n", ret);
		goto err_out;
	}

	/* 5. Wi-Fi/LTE coexistence */
	table[0] = (struct connsys_script) {
		cd->soc->family == CONNSYS_FW_FAMILY_MT6797 ?
			wmt_coex_filter_def_cmd : wmt_coex_filter_old_cmd,
		sizeof(wmt_coex_filter_def_cmd),
		wmt_coex_evt, sizeof(wmt_coex_evt), "coex filter",
	};
	table[1] = (struct connsys_script) {
		cd->soc->family == CONNSYS_FW_FAMILY_MT6797 ?
			wmt_coex_lte_idx_def_cmd : wmt_coex_lte_idx_old_cmd,
		sizeof(wmt_coex_lte_idx_def_cmd),
		wmt_coex_evt, sizeof(wmt_coex_evt), "coex lte idx",
	};
	table[2] = (struct connsys_script) {
		wmt_coex_is_lte_cmd, sizeof(wmt_coex_is_lte_cmd),
		wmt_coex_evt, sizeof(wmt_coex_evt), "is lte project",
	};
	ret = connsys_wmt_send_script(cd, table, 3);
	if (ret)
		dev_warn(cd->dev, "coexistence setup failed: %d\n", ret);

	/* 6. RF calibration */
	table[0] = (struct connsys_script) {
		wmt_rf_calib_cmd, sizeof(wmt_rf_calib_cmd),
		wmt_rf_calib_evt, sizeof(wmt_rf_calib_evt), "rf calibration",
	};
	ret = connsys_wmt_send_script(cd, table, 1);
	if (ret)
		dev_warn(cd->dev, "RF calibration failed: %d\n", ret);

	/* 7. coredump, coexistence, co-clock and FM strap */
	of_property_read_u32(np, "mediatek,wmt-fm-strap", &fm_strap);
	wmt_fm_strap_cmd[5] = fm_strap & 0xff;
	wmt_fm_strap_evt[5] = fm_strap & 0xff;
	table[0] = (struct connsys_script) {
		wmt_coex_setting_cmd, sizeof(wmt_coex_setting_cmd),
		wmt_coex_evt, sizeof(wmt_coex_evt), "coex wmt",
	};
	table[1] = (struct connsys_script) {
		wmt_co_clock_cmd, sizeof(wmt_co_clock_cmd),
		wmt_co_clock_evt, sizeof(wmt_co_clock_evt), "co-clock",
	};
	table[2] = (struct connsys_script) {
		wmt_fm_strap_cmd, sizeof(wmt_fm_strap_cmd),
		wmt_fm_strap_evt, sizeof(wmt_fm_strap_evt), "fm strap",
	};
	table[3] = (struct connsys_script) {
		wmt_coredump_cmd, sizeof(wmt_coredump_cmd),
		wmt_coredump_evt, sizeof(wmt_coredump_evt), "coredump level",
	};
	ret = connsys_wmt_send_script(cd, table, 4);
	if (ret)
		dev_warn(cd->dev, "MCU housekeeping failed: %d\n", ret);

	cd->pwr_on_done = true;
	dev_info(cd->dev, "%s MCU firmware running\n", cd->soc->name);

	return 0;

err_out:
	connsys_btif_close(cd);
	return ret;
}

/**
 * connsys_wmt_power_off - freeze the MCU side of the transport
 */
void connsys_wmt_power_off(struct connsys_dev *cd)
{
	lockdep_assert_held(&cd->lock);

	if (!cd->pwr_on_done)
		return;

	connsys_btif_close(cd);
	connsys_stp_reset(cd);
	cd->pwr_on_done = false;
}

/* ------------------------------------------------------------------ */
/* Function on/off                                                    */
/* ------------------------------------------------------------------ */

static int connsys_wmt_func_ctrl(struct connsys_dev *cd, unsigned int func,
				 bool on)
{
	u8 cmd[] = { 0x01, 0x10, 0x02, 0x00, 0x00, 0x00 };
	u8 evt[16];
	int ret;

	if (func >= WMT_FUNC_MAX || func == WMT_FUNC_FM)
		return -EINVAL;

	cmd[4] = 0x01 + func;
	cmd[5] = on ? 0x01 : 0x00;

	ret = connsys_wmt_send(cd, cmd, sizeof(cmd));
	if (ret < 0)
		return ret;

	ret = connsys_wmt_recv(cd, evt, sizeof(evt), CONNSYS_CTRL_TIMEOUT);
	if (ret <= 0)
		return ret ?: -ETIMEDOUT;
	if (evt[4]) {
		dev_err(cd->dev, "MCU rejected function %u %s (0x%02x)\n",
			func, on ? "on" : "off", evt[4]);
		return -EIO;
	}

	return 0;
}

int connsys_wmt_func_on(struct connsys_dev *cd, unsigned int func)
{
	int ret;

	lockdep_assert_held(&cd->lock);

	if (!cd->pwr_on_done)
		return -ENODEV;
	if (test_bit(func, &cd->funcs))
		return 0;

	/* the loopback test lives entirely in the AP */
	if (func == WMT_FUNC_LPBK) {
		set_bit(func, &cd->funcs);
		return 0;
	}

	ret = connsys_wmt_func_ctrl(cd, func, true);
	if (ret)
		return ret;

	set_bit(func, &cd->funcs);
	dev_info(cd->dev, "function %u on\n", func);

	return 0;
}

int connsys_wmt_func_off(struct connsys_dev *cd, unsigned int func)
{
	int ret;

	lockdep_assert_held(&cd->lock);

	if (!test_bit(func, &cd->funcs))
		return 0;

	if (func == WMT_FUNC_LPBK) {
		clear_bit(func, &cd->funcs);
		return 0;
	}

	ret = connsys_wmt_func_ctrl(cd, func, false);
	if (ret)
		return ret;

	clear_bit(func, &cd->funcs);
	connsys_stp_flush(cd, func);
	dev_info(cd->dev, "function %u off\n", func);

	return 0;
}

void connsys_wmt_abort_reset(struct connsys_dev *cd)
{
	cd->wmt.abort = false;
}

int connsys_wmt_init(struct connsys_dev *cd)
{
	memset(&cd->wmt, 0, sizeof(cd->wmt));
	spin_lock_init(&cd->wmt.evt_lock);
	init_waitqueue_head(&cd->wmt.evt_wq);

	return 0;
}

void connsys_wmt_exit(struct connsys_dev *cd)
{
	connsys_patch_reset(cd);
}
