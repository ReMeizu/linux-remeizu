/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Meizu Linux mainline project
 *
 * /dev/stpwmt - WMT control device and MCU launcher handshake.
 *
 * The vendor stack has a userspace launcher (6620_launcher) that feeds the
 * kernel with the chip id, the STP mode and the list of MCU patch files,
 * and that services kernel requests through read(/dev/stpwmt): "srh_patch"
 * when the patches must be registered, "pwr_on" when the MCU has to be
 * started.  This driver keeps that contract:
 *
 *	wmt_loader:	  COMBO_IOCTL_DO_MODULE_INIT on /dev/wmtdetect
 *	launcher:	  WMT_IOCTL_WMT_CFG_NAME, WMT_IOCTL_SET_STP_MODE,
 *			  WMT_IOCTL_SET_PATCH_NUM + WMT_IOCTL_SET_PATCH_INFO,
 *			  then blocks in read()
 *	kernel:		  queues "pwr_on", the launcher answers by closing
 *			  the device (run the boot script) or by killing the
 *			  launcher (keep the MCU in boot ROM)
 */

#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "connsys.h"

#define STPWMT_CMD_MAX		32

/* ------------------------------------------------------------------ */
/* Launcher requests                                                  */
/* ------------------------------------------------------------------ */

/**
 * connsys_launch_request - queue a command for the userspace launcher
 *
 * Returns 0 when queued, -ENODEV when no launcher is attached.
 */
int connsys_launch_request(struct connsys_dev *cd, enum connsys_launch_cmd cmd)
{
	unsigned long flags;
	u8 idx = cmd;

	if (cmd <= CONNSYS_LAUNCH_NONE || cmd >= CONNSYS_LAUNCH_MAX)
		return -EINVAL;

	spin_lock_irqsave(&cd->launch_lock, flags);
	if (!cd->launcher_pid || cd->launcher_dead) {
		spin_unlock_irqrestore(&cd->launch_lock, flags);
		return -ENODEV;
	}
	kfifo_in(&cd->launch_fifo, &idx, 1);
	spin_unlock_irqrestore(&cd->launch_lock, flags);

	dev_dbg(cd->dev, "launcher request: %s\n", connsys_launch_name(cmd));
	wake_up_interruptible_all(&cd->launcher_wq);

	return 0;
}

/**
 * connsys_boot_decision - act upon the launcher's answer to "pwr_on"
 *
 * Closing /dev/stpwmt means "start the MCU with the registered patches";
 * a launcher that dies while the request is pending means "keep the MCU in
 * its boot ROM", which still allows the STP link to be used for coredumps.
 */
void connsys_boot_decision(struct connsys_dev *cd, bool run_boot)
{
	bool pending;

	spin_lock_irq(&cd->launch_lock);
	pending = cd->boot_pending;
	cd->boot_pending = false;
	spin_unlock_irq(&cd->launch_lock);

	if (!pending)
		return;

	connsys_stp_set_barrier(cd, false);

	if (!run_boot) {
		dev_warn(cd->dev, "launcher did not take over, MCU stays in ROM\n");
		return;
	}

	mutex_lock(&cd->lock);
	if (!cd->pwr_on_done)
		connsys_wmt_power_on(cd);
	mutex_unlock(&cd->lock);

	cd->boot_done = true;
}

/**
 * connsys_boot_barrier - start the launcher handshake
 *
 * Called by the module init path: from now on the transport is blocked
 * until the launcher decides, and it is asked to load the MCU patches.
 */
int connsys_boot_barrier(struct connsys_dev *cd)
{
	int ret;

	connsys_stp_set_barrier(cd, true);

	spin_lock_irq(&cd->launch_lock);
	cd->boot_pending = false;
	spin_unlock_irq(&cd->launch_lock);

	ret = connsys_launch_request(cd, CONNSYS_LAUNCH_SEARCH_PATCH);
	if (!ret)
		ret = connsys_launch_request(cd, CONNSYS_LAUNCH_POWER_ON);

	spin_lock_irq(&cd->launch_lock);
	cd->boot_pending = true;
	spin_unlock_irq(&cd->launch_lock);

	return ret;
}

/**
 * connsys_launcher_gone - release everything tied to the launcher process
 */
void connsys_launcher_gone(struct connsys_dev *cd)
{
	unsigned long flags;

	spin_lock_irqsave(&cd->launch_lock, flags);
	cd->launcher_dead = true;
	cd->launcher_pid = NULL;
	kfifo_reset(&cd->launch_fifo);
	spin_unlock_irqrestore(&cd->launch_lock, flags);

	connsys_wmt_abort(cd);
	wake_up_interruptible_all(&cd->launcher_wq);
	wake_up_interruptible_all(&cd->stp.rx_wq);
}

bool connsys_stp_ready(struct connsys_dev *cd)
{
	return cd->init_done;
}

/* ------------------------------------------------------------------ */
/* File operations                                                    */
/* ------------------------------------------------------------------ */

static ssize_t connsys_stpwmt_read(struct file *filp, char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct connsys_dev *cd = filp->private_data;
	char out[STPWMT_CMD_MAX];
	u8 idx;
	size_t len;
	int ret;

	if (!count)
		return -EINVAL;
	if (count > sizeof(out))
		count = sizeof(out);

	for (;;) {
		spin_lock_irq(&cd->launch_lock);
		ret = kfifo_out(&cd->launch_fifo, &idx, 1);
		spin_unlock_irq(&cd->launch_lock);
		if (ret)
			break;

		if (cd->launcher_dead)
			return 0;
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(cd->launcher_wq,
					       !kfifo_is_empty(&cd->launch_fifo) ||
					       cd->launcher_dead);
		if (ret)
			return ret;
	}

	len = strscpy(out, connsys_launch_name(idx), sizeof(out));
	out[len++] = '\n';

	if (count < len)
		return -ENOBUFS;

	if (copy_to_user(buf, out, len))
		return -EFAULT;

	return len;
}

static ssize_t connsys_stpwmt_write(struct file *filp, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	struct connsys_dev *cd = filp->private_data;
	char cmd[32];
	size_t len = min(count, sizeof(cmd) - 1);

	if (copy_from_user(cmd, buf, len))
		return -EFAULT;
	cmd[len] = '\0';

	/* newer launchers acknowledge the boot request on the write path */
	if (!strncmp(cmd, "pwr_on", 6))
		connsys_boot_decision(cd, true);

	return count;
}

static int connsys_stpwmt_open(struct inode *inode, struct file *filp)
{
	struct connsys_dev *cd = connsys_instance;
	unsigned long flags;

	if (!cd)
		return -ENODEV;

	spin_lock_irqsave(&cd->launch_lock, flags);
	if (cd->launcher_pid && !cd->launcher_dead) {
		spin_unlock_irqrestore(&cd->launch_lock, flags);
		return -EBUSY;
	}
	cd->launcher_pid = get_pid(task_pid(current));
	cd->launcher_dead = false;
	kfifo_reset(&cd->launch_fifo);
	spin_unlock_irqrestore(&cd->launch_lock, flags);

	filp->private_data = cd;
	dev_info(cd->dev, "launcher (pid %d) attached\n", task_pid_nr(current));

	return 0;
}

static int connsys_stpwmt_release(struct inode *inode, struct file *filp)
{
	struct connsys_dev *cd = filp->private_data;

	dev_info(cd->dev, "launcher detached\n");
	connsys_boot_decision(cd, true);
	put_pid(xchg(&cd->launcher_pid, NULL));

	return 0;
}

static long connsys_stpwmt_ioctl(struct file *filp, unsigned int cmd,
				 unsigned long arg)
{
	struct connsys_dev *cd = filp->private_data;
	struct wmt_engine *w = &cd->wmt;
	struct mtk_wmt_patch_info info;
	char name[NAME_MAX + 1];
	const struct firmware *fw;
	int ret = 0;

	switch (cmd) {
	case WMT_IOCTL_WMT_QUERY_CHIPID:
	case WMT_IOCTL_GET_CHIP_INFO:
		return cd->chip_id;

	case WMT_IOCTL_WMT_TELL_CHIPID:
		mutex_lock(&cd->lock);
		ret = connsys_set_chip_id(cd, arg);
		mutex_unlock(&cd->lock);
		break;

	case WMT_IOCTL_WMT_CFG_NAME:
		ret = strncpy_from_user(w->cfg_name, (const char __user *)arg,
					sizeof(w->cfg_name) - 1);
		if (ret < 0)
			break;
		/* the launcher passes the buffer back for the kernel to fill */
		if (copy_to_user((char __user *)arg, w->cfg_name,
				 strlen(w->cfg_name) + 1)) {
			ret = -EFAULT;
			break;
		}
		ret = 0;
		break;

	case WMT_IOCTL_SET_PATCH_NAME:
		if (copy_from_user(name, (const char __user *)arg, NAME_MAX))
			return -EFAULT;
		name[NAME_MAX] = '\0';
		dev_dbg(cd->dev, "patch name %s\n", name);
		break;

	case WMT_IOCTL_SET_STP_MODE: {
		u32 mode = arg & 0xff;

		/* BTIF full mode is the only integrated transport we drive */
		if ((mode & 0x0f) != WMT_STP_MODE_BTIF &&
		    (mode & 0x0f) != WMT_STP_MODE_BTIF_FULL &&
		    mode != CONNSYS_STP_BTIF_FULL) {
			dev_warn(cd->dev, "unsupported STP mode 0x%x\n", mode);
			return -EOPNOTSUPP;
		}
		w->stp_mode = CONNSYS_STP_BTIF_FULL;
		dev_info(cd->dev, "STP mode: BTIF full\n");
		break;
	}

	case WMT_IOCTL_SET_PATCH_NUM:
		if (!arg || arg > WMT_MAX_PATCH_NUM) {
			dev_err(cd->dev, "invalid patch count %lu\n", arg);
			return -EINVAL;
		}
		connsys_patch_reset(cd);
		dev_dbg(cd->dev, "launcher announces %lu patches\n", arg);
		break;

	case WMT_IOCTL_SET_PATCH_INFO:
		if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
			return -EFAULT;
		info.patch_name[sizeof(info.patch_name) - 1] = '\0';

		fw = connsys_patch_register(cd, info.download_seq,
					    info.patch_name, info.address);
		if (IS_ERR(fw))
			return PTR_ERR(fw);
		break;

	case WMT_IOCTL_FUNC_ONOFF_CTRL: {
		unsigned long mask = arg & 0x0f;

		if (arg & WMT_FUNC_CTRL_ON_FLAG)
			ret = connsys_func_mask_on(cd, mask);
		else
			ret = connsys_func_mask_off(cd, mask);
		break;
	}

	case WMT_IOCTL_LPBK_POWER_CTRL:
		ret = connsys_func_lpbk(cd, arg & 0x01);
		break;

	case WMT_IOCTL_SET_LAUNCHER_KILL:
		dev_dbg(cd->dev, "launcher kill flag: %lu\n", arg);
		break;

	case WMT_IOCTL_WMT_COREDUMP_CTRL:
		cd->coredump = !!arg;
		break;

	case WMT_IOCTL_GET_APO_FLAG:
		return 0;

	case WMT_IOCTL_LPBK_TEST:
	case WMT_IOCTL_ADIE_LPBK_TEST:
	case WMT_IOCTL_SEND_BGW_DS_CMD:
	case WMT_IOCTL_PORT_NAME:
		dev_dbg(cd->dev, "ioctl 0x%08x not supported\n", cmd);
		return -EOPNOTSUPP;

	default:
		dev_dbg(cd->dev, "unknown stpwmt ioctl 0x%08x\n", cmd);
		return -EINVAL;
	}

	return ret;
}

static const struct file_operations connsys_stpwmt_fops = {
	.owner		= THIS_MODULE,
	.open		= connsys_stpwmt_open,
	.release	= connsys_stpwmt_release,
	.read		= connsys_stpwmt_read,
	.write		= connsys_stpwmt_write,
	.unlocked_ioctl	= connsys_stpwmt_ioctl,
	.compat_ioctl	= connsys_stpwmt_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice connsys_stpwmt_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "stpwmt",
	.fops	= &connsys_stpwmt_fops,
};

/* ------------------------------------------------------------------ */
/* Setup                                                              */
/* ------------------------------------------------------------------ */

int connsys_stpwmt_init(struct connsys_dev *cd)
{
	int ret;

	ret = misc_register(&connsys_stpwmt_dev);
	if (ret)
		dev_err(cd->dev, "cannot register /dev/stpwmt: %d\n", ret);

	return ret;
}

void connsys_stpwmt_exit(struct connsys_dev *cd)
{
	misc_deregister(&connsys_stpwmt_dev);
}
