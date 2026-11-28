/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Meizu Linux mainline project
 *
 * Private definitions shared by the MediaTek CONSYS driver suite
 * (Wi-Fi / Bluetooth / GPS combo subsystem, SoC generation "gen2"/"gen3").
 */

#ifndef _MTK_CONNSYS_H
#define _MTK_CONNSYS_H

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <uapi/linux/mtk_connsys.h>

#include "connsys_types.h"
#include "stp_core.h"
#include "wmt_core.h"

#include "connsys_types.h"

#define CONNSYS_CTRL_TIMEOUT	(5 * HZ)
#define CONNSYS_LAUNCH_FIFO	16
#define CONNSYS_RX_CHUNK	256

/* Power state machine */
enum connsys_power_state {
	CONNSYS_POWER_OFF = 0,
	CONNSYS_POWER_ON,
};


/**
 * struct connsys_dev - one CONSYS instance (one per SoC)
 *
 * @dev:      platform device
 * @regs:     mapped register blocks
 * @reg_phys: physical addresses, for diagnostics
 * @btif:     BTIF UART register window
 * @clk_*:    clocks used by the CONN and BTIF blocks
 * @chip_id:  SoC chip id as reported to userspace
 * @lock:     serialises the whole control plane (power, functions, WMT)
 * @state:    power state (enum connsys_power_state)
 * @funcs:    bitmask of active logical functions (1 << wmt function)
 * @wdt_events: watchdog bites since boot
 * @launcher_pid: pid of the process owning /dev/stpwmt
 * @launcher_wq: signalled when a launcher command is queued
 * @launch_fifo: pending launcher command indices
 * @boot_pending: the launcher must take the boot decision
 * @boot_done: boot script already ran
 * @init_done: COMBO_IOCTL_DO_MODULE_INIT succeeded
 * @pwr_on_done: MCU firmware booted
 * @timeout_ms: register polling timeout in milliseconds
 * @stp:      STP transport engine
 * @wmt:      WMT command engine
 */
struct connsys_dev {
	struct device *dev;
	const struct connsys_soc_data *soc;

	void __iomem *regs[CONNSYS_REG_MAX];
	phys_addr_t reg_phys[CONNSYS_REG_MAX];

	void __iomem *btif;
	phys_addr_t btif_phys;
	int btif_irq;

	struct clk *clk_cfg;
	struct clk *clk_emi;
	struct clk *clk_btif;
	struct clk *clk_btif_dma;

	u32 chip_id;

	struct mutex lock;
	int state;
	unsigned long funcs;
	bool pwr_on_done;
	bool booting;

	int bgf_irq;
	int wdt_irq;
	int conn2ap_irq;
	bool irq_ready;
	unsigned int wdt_events;

	struct pid *launcher_pid;
	wait_queue_head_t launcher_wq;
	struct kfifo launch_fifo;
	spinlock_t launch_lock;
	bool boot_pending;
	bool boot_done;
	bool init_done;
	bool launcher_dead;
	bool coredump;
	bool wlan_ready;
	unsigned int timeout_ms;

	struct work_struct reset_work;
	struct work_struct rx_work;

	struct stp_engine stp;
	struct wmt_engine wmt;
};

/* connsys_soc.c / connsys_hw.c */
const struct connsys_soc_data *connsys_soc_by_chip_id(u32 chip_id);
void connsys_activate_irqs(struct connsys_dev *cd);
void connsys_deactivate_irqs(struct connsys_dev *cd);
int connsys_map_resources(struct connsys_dev *cd);
void connsys_unmap_resources(struct connsys_dev *cd);
int connsys_power_on(struct connsys_dev *cd);
void connsys_power_off(struct connsys_dev *cd);
int connsys_read_chip_id(struct connsys_dev *cd, u32 *chip_id);
int connsys_emi_setup(struct connsys_dev *cd);
int connsys_request_firmware(struct connsys_dev *cd, const char *name,
			     const struct firmware **fw);
void connsys_release_firmware(const struct firmware *fw);
int connsys_btif_open(struct connsys_dev *cd);
void connsys_btif_close(struct connsys_dev *cd);
int connsys_btif_tx(struct connsys_dev *cd, const u8 *data, size_t len);
size_t connsys_btif_rx(struct connsys_dev *cd, u8 *buf, size_t len);
irqreturn_t connsys_btif_isr(int irq, void *data);

/* stp_core.c */
int connsys_stp_init(struct connsys_dev *cd);
void connsys_stp_exit(struct connsys_dev *cd);
void connsys_stp_reset(struct connsys_dev *cd);
void connsys_stp_set_barrier(struct connsys_dev *cd, bool on);
void connsys_rx_work(struct work_struct *work);
int connsys_stp_send(struct connsys_dev *cd, u8 type, const u8 *data,
		     size_t len);
int connsys_stp_input(struct connsys_dev *cd, const u8 *buf, size_t len);
size_t connsys_stp_read(struct connsys_dev *cd, u8 type, u8 *buf,
			size_t len);
bool connsys_stp_rx_pending(struct connsys_dev *cd, u8 type);
void connsys_stp_flush(struct connsys_dev *cd, u8 type);

/* wmt_core.c */
struct connsys_script {
	const u8 *cmd;
	size_t cmd_len;
	const u8 *evt;
	size_t evt_len;
	const char *name;
};

int connsys_wmt_init(struct connsys_dev *cd);
void connsys_wmt_exit(struct connsys_dev *cd);
int connsys_wmt_send_script(struct connsys_dev *cd,
			    const struct connsys_script *script, size_t n);
int connsys_wmt_send(struct connsys_dev *cd, const u8 *cmd, size_t len);
int connsys_wmt_recv(struct connsys_dev *cd, u8 *buf, size_t len,
		     unsigned long timeout);
void connsys_wmt_rx(struct connsys_dev *cd, const u8 *buf, size_t len);
void connsys_wmt_abort(struct connsys_dev *cd);
void connsys_wmt_abort_reset(struct connsys_dev *cd);
int connsys_wmt_power_on(struct connsys_dev *cd);
void connsys_wmt_power_off(struct connsys_dev *cd);
int connsys_wmt_func_on(struct connsys_dev *cd, unsigned int func);
int connsys_wmt_func_off(struct connsys_dev *cd, unsigned int func);
const struct firmware *connsys_patch_register(struct connsys_dev *cd,
					      int seq, const char *name,
					      const u8 address[4]);
void connsys_patch_reset(struct connsys_dev *cd);
int connsys_patch_load(struct connsys_dev *cd);
int connsys_wmt_reg_write(struct connsys_dev *cd, u32 addr, u32 val,
			  u32 mask, const char *name);

/* wmt_detect.c */
int connsys_detect_init(struct connsys_dev *cd);
void connsys_detect_exit(struct connsys_dev *cd);
int connsys_module_init(struct connsys_dev *cd);
void connsys_module_cleanup(struct connsys_dev *cd);
int connsys_set_chip_id(struct connsys_dev *cd, u32 chip_id);
u32 connsys_soc_chip_id(struct connsys_dev *cd);

/* stpwmt.c */
int connsys_launch_request(struct connsys_dev *cd, enum connsys_launch_cmd cmd);
void connsys_boot_decision(struct connsys_dev *cd, bool run_boot);
int connsys_boot_barrier(struct connsys_dev *cd);
void connsys_launcher_gone(struct connsys_dev *cd);
extern struct connsys_dev *connsys_instance;
int connsys_stpwmt_init(struct connsys_dev *cd);
void connsys_stpwmt_exit(struct connsys_dev *cd);
void connsys_launcher_gone(struct connsys_dev *cd);
bool connsys_stp_ready(struct connsys_dev *cd);

/* stp_chrdevs.c */
int connsys_func_on(struct connsys_dev *cd, unsigned int func);
int connsys_func_off(struct connsys_dev *cd, unsigned int func);
bool connsys_func_active(struct connsys_dev *cd, unsigned int func);
int connsys_func_mask_on(struct connsys_dev *cd, unsigned long mask);
int connsys_func_mask_off(struct connsys_dev *cd, unsigned long mask);
int connsys_func_lpbk(struct connsys_dev *cd, bool on);
int connsys_chrdevs_init(struct connsys_dev *cd);
void connsys_chrdevs_exit(struct connsys_dev *cd);

/* mtk_connsys_wlan.c */
int connsys_wlan_init(struct connsys_dev *cd);
void connsys_wlan_exit(struct connsys_dev *cd);
struct net_device *connsys_wlan_netdev(struct connsys_dev *cd);
int connsys_wlan_set_p2p_mode(struct net_device *ndev, bool enable, bool ap);
void connsys_wlan_set_debug_level(unsigned int level);
void connsys_wlan_rx(struct connsys_dev *cd);

#endif /* _MTK_CONNSYS_H */
