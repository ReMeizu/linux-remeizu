/* SPDX-License-Identifier: GPL-2.0-only
 *
 * MediaTek CONSYS per-function character devices:
 *
 *   /dev/stpbt    Bluetooth HCI over STP channel 0
 *   /dev/stpgps   GPS NMEA / MNLD over STP channel 2
 *   /dev/wmtWifi  Wi-Fi power and mode control ('0', '1', 'S', 'P', 'A', 'D')
 *
 * Bluetooth and GPS userspace talks to the MCU through these nodes without
 * knowing anything about STP: read() returns whole HCI/NMEA packets and
 * write() frames them onto the right STP channel.
 *
 * Wi-Fi uses the same trick as the vendor wmt_chrdev_wifi: the power
 * sequence is triggered by writing a single character, while the data path
 * is owned by the WLAN driver (its STP channel and HIF rings).
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kdev_t.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "connsys.h"

#define CONNSYS_CHRDEV_MAX	2048

/* ------------------------------------------------------------------ */
/* Function power management                                          */
/* ------------------------------------------------------------------ */

bool connsys_func_active(struct connsys_dev *cd, unsigned int func)
{
	return test_bit(func, &cd->funcs);
}

int connsys_func_on(struct connsys_dev *cd, unsigned int func)
{
	int ret;

	if (func >= WMT_FUNC_MAX)
		return -EINVAL;

	if (func == WMT_FUNC_BT || func == WMT_FUNC_GPS ||
	    func == WMT_FUNC_WIFI || func == WMT_FUNC_LPBK) {
		/* no MCU command for these: the MCU side is always ready */
		if (!cd->pwr_on_done) {
			dev_warn(cd->dev, "function %u on requested before boot\n",
				 func);
			return -EAGAIN;
		}
		set_bit(func, &cd->funcs);
		return 0;
	}

	mutex_lock(&cd->lock);
	ret = connsys_wmt_func_on(cd, func);
	mutex_unlock(&cd->lock);

	return ret;
}

int connsys_func_off(struct connsys_dev *cd, unsigned int func)
{
	int ret;

	if (func >= WMT_FUNC_MAX)
		return -EINVAL;

	if (func == WMT_FUNC_BT || func == WMT_FUNC_GPS ||
	    func == WMT_FUNC_WIFI || func == WMT_FUNC_LPBK) {
		clear_bit(func, &cd->funcs);
		connsys_stp_flush(cd, func);
		return 0;
	}

	mutex_lock(&cd->lock);
	ret = connsys_wmt_func_off(cd, func);
	mutex_unlock(&cd->lock);

	return ret;
}

int connsys_func_mask_on(struct connsys_dev *cd, unsigned long mask)
{
	unsigned int func;
	int ret = 0;

	for_each_set_bit(func, &mask, WMT_FUNC_MAX) {
		int err = connsys_func_on(cd, func);

		if (err && err != -EAGAIN)
			ret = err;
	}

	return ret;
}

int connsys_func_mask_off(struct connsys_dev *cd, unsigned long mask)
{
	unsigned int func;
	int ret = 0;

	for_each_set_bit(func, &mask, WMT_FUNC_MAX) {
		int err = connsys_func_off(cd, func);

		if (err)
			ret = err;
	}

	return ret;
}

int connsys_func_lpbk(struct connsys_dev *cd, bool on)
{
	return on ? connsys_func_on(cd, WMT_FUNC_LPBK) :
		    connsys_func_off(cd, WMT_FUNC_LPBK);
}

/* ------------------------------------------------------------------ */
/* STP channel character device                                       */
/* ------------------------------------------------------------------ */

struct connsys_stp_chrdev {
	struct miscdevice mdev;
	struct connsys_dev *cd;
	u8 channel;
	bool opened;
};

static ssize_t connsys_stp_file_read(struct file *filp, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct connsys_stp_chrdev *dev = filp->private_data;
	struct connsys_dev *cd = dev->cd;
	u8 *kbuf;
	size_t len;
	int ret;

	if (!count)
		return -EINVAL;

	kbuf = kmalloc(CONNSYS_BTIF_MAX_LEN, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	for (;;) {
		len = connsys_stp_read(cd, dev->channel, kbuf,
				       CONNSYS_BTIF_MAX_LEN);
		if (len)
			break;

		if (filp->f_flags & O_NONBLOCK) {
			kfree(kbuf);
			return -EAGAIN;
		}

		ret = wait_event_interruptible(cd->stp.rx_wq,
					       connsys_stp_rx_pending(cd,
								      dev->channel) ||
					       cd->launcher_dead);
		if (ret == -ERESTARTSYS) {
			kfree(kbuf);
			return -ERESTARTSYS;
		}
		if (cd->launcher_dead) {
			kfree(kbuf);
			return 0;
		}
	}

	if (len > count)
		len = count;
	if (copy_to_user(buf, kbuf, len)) {
		kfree(kbuf);
		return -EFAULT;
	}

	kfree(kbuf);
	return len;
}

static ssize_t connsys_stp_file_write(struct file *filp, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct connsys_stp_chrdev *dev = filp->private_data;
	struct connsys_dev *cd = dev->cd;
	u8 *kbuf;
	size_t len;
	int ret;

	if (!count || count > CONNSYS_BTIF_MAX_LEN)
		return -EMSGSIZE;

	kbuf = memdup_user(buf, count);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	len = count;
	for (;;) {
		ret = connsys_stp_send(cd, dev->channel, kbuf, len);
		if (ret >= 0)
			break;
		if (ret != -EAGAIN) {
			kfree(kbuf);
			return ret;
		}
		if (filp->f_flags & O_NONBLOCK) {
			kfree(kbuf);
			return -EAGAIN;
		}
		if (msleep_interruptible(1)) {
			kfree(kbuf);
			return -ERESTARTSYS;
		}
	}

	kfree(kbuf);
	return ret;
}

static __poll_t connsys_stp_poll(struct file *filp, poll_table *wait)
{
	struct connsys_stp_chrdev *dev = filp->private_data;
	struct connsys_dev *cd = dev->cd;
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;

	poll_wait(filp, &cd->stp.rx_wq, wait);
	if (connsys_stp_rx_pending(cd, dev->channel))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (cd->launcher_dead)
		mask |= EPOLLHUP;

	return mask;
}

static int connsys_stp_open(struct inode *inode, struct file *filp)
{
	struct connsys_stp_chrdev *dev = container_of(filp->private_data,
						      struct connsys_stp_chrdev,
						      mdev);
	struct connsys_dev *cd = dev->cd;
	int ret;

	filp->private_data = dev;

	ret = connsys_func_on(cd, dev->channel == CONNSYS_STP_BT ?
			      WMT_FUNC_BT : WMT_FUNC_GPS);
	if (ret)
		return ret;

	connsys_stp_flush(cd, dev->channel);
	dev_dbg(cd->dev, "channel %u opened\n", dev->channel);

	return 0;
}

static int connsys_stp_release(struct inode *inode, struct file *filp)
{
	struct connsys_stp_chrdev *dev = filp->private_data;
	struct connsys_dev *cd = dev->cd;

	connsys_func_off(cd, dev->channel == CONNSYS_STP_BT ?
			 WMT_FUNC_BT : WMT_FUNC_GPS);
	dev_dbg(cd->dev, "channel %u closed\n", dev->channel);

	return 0;
}

static const struct file_operations connsys_stp_fops = {
	.owner		= THIS_MODULE,
	.open		= connsys_stp_open,
	.release	= connsys_stp_release,
	.read		= connsys_stp_file_read,
	.write		= connsys_stp_file_write,
	.poll		= connsys_stp_poll,
	.llseek		= noop_llseek,
};

static struct connsys_stp_chrdev connsys_bt_dev = {
	.mdev = {
		.minor	= MISC_DYNAMIC_MINOR,
		.name	= "stpbt",
		.fops	= &connsys_stp_fops,
	},
	.channel = CONNSYS_STP_BT,
};

static struct connsys_stp_chrdev connsys_gps_dev = {
	.mdev = {
		.minor	= MISC_DYNAMIC_MINOR,
		.name	= "stpgps",
		.fops	= &connsys_stp_fops,
	},
	.channel = CONNSYS_STP_GPS,
};

/* ------------------------------------------------------------------ */
/* /dev/wmtWifi - power and mode control                              */
/* ------------------------------------------------------------------ */

enum connsys_wlan_mode {
	CONNSYS_WLAN_MODE_HALT = 0,
	CONNSYS_WLAN_MODE_AP,
	CONNSYS_WLAN_MODE_STA_P2P,
};

static struct {
	struct connsys_dev *cd;
	struct mutex lock;
	bool powered;
	enum connsys_wlan_mode mode;
	unsigned int open_count;
} connsys_wifi;

static int connsys_wifi_power_on(void)
{
	struct connsys_dev *cd = connsys_wifi.cd;
	int ret;

	if (connsys_wifi.powered)
		return 0;

	/* the MCU needs its boot script before any function is enabled */
	if (!cd->pwr_on_done) {
		ret = connsys_boot_barrier(cd);
		if (ret == -ENODEV) {
			dev_warn(cd->dev,
				 "no launcher, booting the MCU in-band\n");
			connsys_boot_decision(cd, true);
		}
	}

	ret = connsys_func_on(cd, WMT_FUNC_WIFI);
	if (ret)
		return ret;

	connsys_wifi.powered = true;
	connsys_wifi.mode = CONNSYS_WLAN_MODE_HALT;
	dev_info(cd->dev, "Wi-Fi powered on\n");

	return 0;
}

static int connsys_wifi_power_off(void)
{
	struct connsys_dev *cd = connsys_wifi.cd;

	if (!connsys_wifi.powered)
		return 0;

	connsys_wlan_set_p2p_mode(NULL, false, false);
	connsys_func_off(cd, WMT_FUNC_WIFI);
	connsys_wifi.powered = false;
	connsys_wifi.mode = CONNSYS_WLAN_MODE_HALT;
	dev_info(cd->dev, "Wi-Fi powered off\n");

	return 0;
}

static int connsys_wifi_set_mode(char mode)
{
	struct connsys_dev *cd = connsys_wifi.cd;
	struct net_device *ndev;
	bool ap = mode == 'A';
	enum connsys_wlan_mode want = ap ? CONNSYS_WLAN_MODE_AP :
					   CONNSYS_WLAN_MODE_STA_P2P;

	if (connsys_wifi.powered && connsys_wifi.mode == want)
		return 0;

	ndev = connsys_wlan_netdev(cd);
	if (!ndev) {
		dev_err(cd->dev, "wlan device not available\n");
		return -ENODEV;
	}

	return connsys_wlan_set_p2p_mode(ndev, true, ap);
}

static ssize_t connsys_wifi_write(struct file *filp, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	char cmd[16];
	size_t len = min(count, sizeof(cmd) - 1);
	int ret = 0;

	if (!len)
		return -EINVAL;
	if (copy_from_user(cmd, buf, len))
		return -EFAULT;
	cmd[len] = '\0';

	mutex_lock(&connsys_wifi.lock);
	switch (cmd[0]) {
	case '0':
		ret = connsys_wifi_power_off();
		break;
	case '1':
		ret = connsys_wifi_power_on();
		break;
	case 'S':
	case 'P':
	case 'A':
		ret = connsys_wifi_power_on();
		if (!ret)
			ret = connsys_wifi_set_mode(cmd[0]);
		break;
	case 'D':
		connsys_wlan_set_debug_level(cmd[1] - '0');
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&connsys_wifi.lock);

	return ret ? ret : (ssize_t)count;
}

static int connsys_wifi_open(struct inode *inode, struct file *filp)
{
	mutex_lock(&connsys_wifi.lock);
	if (connsys_wifi.cd != connsys_instance) {
		mutex_unlock(&connsys_wifi.lock);
		return -ENODEV;
	}
	connsys_wifi.open_count++;
	mutex_unlock(&connsys_wifi.lock);

	return 0;
}

static int connsys_wifi_release(struct inode *inode, struct file *filp)
{
	mutex_lock(&connsys_wifi.lock);
	if (connsys_wifi.open_count)
		connsys_wifi.open_count--;
	if (!connsys_wifi.open_count && connsys_wifi.powered)
		connsys_wifi_power_off();
	mutex_unlock(&connsys_wifi.lock);

	return 0;
}

static const struct file_operations connsys_wifi_fops = {
	.owner		= THIS_MODULE,
	.open		= connsys_wifi_open,
	.release	= connsys_wifi_release,
	.write		= connsys_wifi_write,
	.llseek		= noop_llseek,
};

static struct miscdevice connsys_wifi_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "wmtWifi",
	.fops	= &connsys_wifi_fops,
};

/* ------------------------------------------------------------------ */
/* Setup                                                              */
/* ------------------------------------------------------------------ */

int connsys_chrdevs_init(struct connsys_dev *cd)
{
	int ret;

	connsys_bt_dev.cd = cd;
	connsys_gps_dev.cd = cd;
	connsys_wifi.cd = cd;
	mutex_init(&connsys_wifi.lock);
	connsys_wifi.powered = false;
	connsys_wifi.mode = CONNSYS_WLAN_MODE_HALT;

	ret = misc_register(&connsys_bt_dev.mdev);
	if (ret) {
		dev_err(cd->dev, "cannot register /dev/stpbt: %d\n", ret);
		return ret;
	}

	ret = misc_register(&connsys_gps_dev.mdev);
	if (ret) {
		dev_err(cd->dev, "cannot register /dev/stpgps: %d\n", ret);
		goto err_bt;
	}

	ret = misc_register(&connsys_wifi_dev);
	if (ret) {
		dev_err(cd->dev, "cannot register /dev/wmtWifi: %d\n", ret);
		goto err_gps;
	}

	return 0;

err_gps:
	misc_deregister(&connsys_gps_dev.mdev);
err_bt:
	misc_deregister(&connsys_bt_dev.mdev);

	return ret;
}

void connsys_chrdevs_exit(struct connsys_dev *cd)
{
	misc_deregister(&connsys_wifi_dev);
	misc_deregister(&connsys_gps_dev.mdev);
	misc_deregister(&connsys_bt_dev.mdev);

	mutex_lock(&connsys_wifi.lock);
	if (connsys_wifi.powered)
		connsys_wifi_power_off();
	mutex_unlock(&connsys_wifi.lock);
	connsys_wifi.cd = NULL;
}
