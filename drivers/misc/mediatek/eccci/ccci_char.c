// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MD1 cellular modem (ECCCI) - character ports.
 *
 * The Android userspace talks to the modem through the device nodes the
 * vendor driver exposes:
 *
 *   /dev/ccci_fs         channel 14/15  NVRAM file service (ccci_fsd)
 *   /dev/ttyC0           channel 6/8    AT command channel (RILD, META)
 *   /dev/ttyC1           channel 10/12  MUX / IMS channel
 *   /dev/ccci_monitor    modem assert and watchdog messages
 *   /dev/ccci_ioctl0..4  modem wide control (md_init, mdlogger, muxreport)
 *   /dev/ccci_aud        channel 4/5    voice speech (PCM) IPC
 *   /dev/ccci_ipc_0..4   channel 34..41 inter processor communication
 *
 * A port is a thin channel endpoint: read() returns whole CCCI packets
 * (header first) queued by the receive path, write() takes one or more CCCI
 * packets, validates their headers and hands them to CLDMA, ioctl()
 * implements the modem wide control surface and the per port operations.
 */
#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <uapi/linux/mtk_ccci.h>

#include "ccci_priv.h"

/*
 * Character port table, in registration order. The first three entries are
 * the vital users: while one of them is open the modem is never reset behind
 * the userspace's back, exactly like the vendor driver keeps FS, AT2 and
 * META from being torn down.
 */
static const struct ccci_char_descr {
	const char *name;
	int rx_ch;
	int tx_ch;
	unsigned int queue_max;
	int critical_index;	/* -1 for non critical ports */
} ccci_char_ports[] = {
	{ "ccci_fs",      CCCI_FS_RX,    CCCI_FS_TX,    32, 0 },
	{ "ttyC1",        CCCI_UART2_RX, CCCI_UART2_TX, 32, 1 },
	{ "ttyC0",        CCCI_UART1_RX, CCCI_UART1_TX, 32, 3 },
	{ "ccci_monitor", CCCI_MONITOR_CH, CCCI_MD_LOG_TX, 64, -1 },
	{ "ccci_aud",     CCCI_PCM_RX,   CCCI_PCM_TX,   16, -1 },
	{ "ccci_ipc_0",   CCCI_IPC_RX,   CCCI_IPC_TX,   32, -1 },
	{ "ccci_ipc_1",   CCCI_IPC_RX,   CCCI_IPC_TX,   32, -1 },
	{ "ccci_ipc_2",   CCCI_IPC_RX,   CCCI_IPC_TX,   32, -1 },
	{ "ccci_ipc_3",   CCCI_IPC_RX,   CCCI_IPC_TX,   32, -1 },
	{ "ccci_ipc_4",   CCCI_IPC_RX,   CCCI_IPC_TX,   32, -1 },
};

#define CCCI_IOCTL_DEVS		5

struct ccci_char {
	struct ccci_modem *md;
	struct class *class;
	dev_t devid;
	unsigned int nports;
	struct ccci_port *ports[ARRAY_SIZE(ccci_char_ports)];
	unsigned int nioctl;
	struct ccci_ioctl_dev *ioctl_devs[CCCI_IOCTL_DEVS];
};

/**
 * struct ccci_ioctl_dev - one /dev/ccci_ioctlN node
 * @char_dev: owning registration block
 * @cdev: character device
 * @dev: device node
 * @name: "ccci_ioctlN"
 */
struct ccci_ioctl_dev {
	struct ccci_char *char_dev;
	struct cdev cdev;
	struct device *dev;
	char name[16];
};

static struct ccci_char *ccci_char_of(struct ccci_modem *md)
{
	return md->char_dev;
}

/* ---------------------------------------------------------------------- */
/* Port life cycle                                                        */
/* ---------------------------------------------------------------------- */

static void ccci_port_flush(struct ccci_port *port)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&port->rx_queue)))
		kfree_skb(skb);
	port->busy = false;
	port->last_seq = 0;
}

int ccci_port_register(struct ccci_modem *md, struct ccci_port *port)
{
	if (port->rx_ch < 0 || port->rx_ch >= CCCI_MAX_CH_NUM)
		return -EINVAL;

	mutex_lock(&md->port_lock);
	if (md->ports[port->rx_ch]) {
		mutex_unlock(&md->port_lock);
		return -EBUSY;
	}
	md->ports[port->rx_ch] = port;
	mutex_unlock(&md->port_lock);
	return 0;
}

void ccci_port_unregister(struct ccci_modem *md, struct ccci_port *port)
{
	mutex_lock(&md->port_lock);
	if (port->rx_ch >= 0 && port->rx_ch < CCCI_MAX_CH_NUM &&
	    md->ports[port->rx_ch] == port)
		md->ports[port->rx_ch] = NULL;
	mutex_unlock(&md->port_lock);

	ccci_port_flush(port);
	wake_up_all(&port->rx_wait);
}

/**
 * ccci_port_recv() - queue a packet received for this port
 * @port: destination port
 * @skb: packet, ownership is transferred
 *
 * Called from the CLDMA receive path, which may run in NAPI context.
 */
void ccci_port_recv(struct ccci_port *port, struct sk_buff *skb)
{
	struct ccci_msg *hdr = (struct ccci_msg *)skb->data;
	unsigned long flags;

	spin_lock_irqsave(&port->rx_lock, flags);
	if (port->busy || skb_queue_len(&port->rx_queue) >= port->queue_max) {
		port->dropped++;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		kfree_skb(skb);
		return;
	}
	port->last_seq = CCCI_HDR_SEQ(hdr);
	__skb_queue_tail(&port->rx_queue, skb);
	spin_unlock_irqrestore(&port->rx_lock, flags);

	wake_up_interruptible(&port->rx_wait);
}

void ccci_port_broadcast_state(struct ccci_modem *md, enum ccci_md_state state)
{
	struct ccci_port *port;
	int ch;

	if (state == CCCI_STATE_READY)
		return;

	/*
	 * On any state change away from READY the queues hold stale packets:
	 * sequence numbers restart from zero on the next boot, so userspace
	 * must never mix packets from two boot cycles.
	 */
	for (ch = 0; ch < CCCI_MAX_CH_NUM; ch++) {
		port = READ_ONCE(md->ports[ch]);
		if (!port)
			continue;
		ccci_port_flush(port);
		wake_up_interruptible(&port->rx_wait);
	}
}

/* ---------------------------------------------------------------------- */
/* File operations                                                        */
/* ---------------------------------------------------------------------- */

static int ccci_char_open(struct inode *inode, struct file *file)
{
	struct ccci_port *port = container_of(inode->i_cdev,
					      struct ccci_port, cdev);

	file->private_data = port;

	mutex_lock(&port->open_lock);
	if (port->is_open) {
		mutex_unlock(&port->open_lock);
		return -EBUSY;
	}
	port->is_open = true;
	mutex_unlock(&port->open_lock);

	if (port->critical_index >= 0)
		atomic_set(&port->md->critical_user_active
				  [port->critical_index], 1);

	return nonseekable_open(inode, file);
}

static int ccci_char_release(struct inode *inode, struct file *file)
{
	struct ccci_port *port = file->private_data;

	mutex_lock(&port->open_lock);
	port->is_open = false;
	mutex_unlock(&port->open_lock);

	if (port->critical_index >= 0)
		atomic_set(&port->md->critical_user_active
				  [port->critical_index], 0);

	wake_up_interruptible(&port->rx_wait);
	return 0;
}

static ssize_t ccci_char_read(struct file *file, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct ccci_port *port = file->private_data;
	struct sk_buff *skb;
	unsigned long flags;
	size_t len;
	int ret;

	if (count < CCCI_HDR_SIZE)
		return -EINVAL;

	for (;;) {
		spin_lock_irqsave(&port->rx_lock, flags);
		skb = __skb_dequeue(&port->rx_queue);
		spin_unlock_irqrestore(&port->rx_lock, flags);
		if (skb)
			break;

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(port->rx_wait,
					       skb_queue_len(&port->rx_queue) ||
					       !READ_ONCE(port->is_open));
		if (ret)
			return ret;
		if (!READ_ONCE(port->is_open))
			return -ENODEV;
	}

	/*
	 * A packet is never split: a short user buffer gets the packet
	 * trimmed to its size and the remainder is dropped, which is what the
	 * vendor driver does and what ccci_fsd expects (it always reads into
	 * a FS_MAX_BUF_SIZE large buffer).
	 */
	len = min(skb->len, count);
	if (copy_to_user(buf, skb->data, len)) {
		kfree_skb(skb);
		return -EFAULT;
	}

	port->rx_bytes += len;
	port->rx_pkts++;
	if (len < skb->len)
		port->dropped++;
	kfree_skb(skb);

	return len;
}

static int ccci_char_send_one(struct ccci_port *port, const void __user *data,
			      size_t count)
{
	struct ccci_modem *md = port->md;
	struct ccci_msg hdr, *phdr;
	struct sk_buff *skb;
	unsigned int ch, len;

	if (count < CCCI_HDR_SIZE || count > CCCI_PKT_SIZE_MAX)
		return -EINVAL;

	if (copy_from_user(&hdr, data, CCCI_HDR_SIZE))
		return -EFAULT;

	len = CCCI_HDR_LEN(&hdr);
	ch = CCCI_HDR_CHANNEL(&hdr);
	if (len < CCCI_HDR_SIZE || len > CCCI_PKT_SIZE_MAX || len != count)
		return -EINVAL;
	if (ch >= CCCI_MAX_CH_NUM)
		return -EINVAL;

	skb = ccci_alloc_skb(len, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	if (copy_from_user(skb->data, data, len)) {
		kfree_skb(skb);
		return -EFAULT;
	}

	/* Rewrite the header so that a malformed one cannot reach the MD. */
	phdr = (struct ccci_msg *)skb->data;
	phdr->data[0] = cpu_to_le32(CCCI_MONITOR_CH_ID);
	phdr->data[1] = cpu_to_le32(len);
	phdr->reserved = hdr.reserved;

	return ccci_send_packet(md, ch, skb);
}

static ssize_t ccci_char_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct ccci_port *port = file->private_data;
	size_t done = 0;
	u32 hdr_len;
	int ret;

	/*
	 * A single write() may carry several complete CCCI packets (RILD
	 * batches AT commands that way); every packet states its own length
	 * in data[1].
	 */
	while (count - done >= CCCI_HDR_SIZE) {
		size_t pkt;

		if (get_user(hdr_len,
			     (const __u32 __user *)(buf + done + 4)))
			return done ? done : -EFAULT;
		hdr_len = le32_to_cpu(hdr_len);
		if (hdr_len < CCCI_HDR_SIZE || hdr_len > CCCI_PKT_SIZE_MAX)
			return done ? done : -EINVAL;
		if (hdr_len > count - done)
			break;
		pkt = hdr_len;

		ret = ccci_char_send_one(port, buf + done, pkt);
		if (ret) {
			if (done)
				break;
			return ret;
		}
		port->tx_bytes += pkt;
		port->tx_pkts++;
		done += pkt;
	}

	return done ? done : -EINVAL;
}

static __poll_t ccci_char_poll(struct file *file, poll_table *wait)
{
	struct ccci_port *port = file->private_data;
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;

	poll_wait(file, &port->rx_wait, wait);
	if (skb_queue_len(&port->rx_queue))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

/* ---------------------------------------------------------------------- */
/* Modem wide ioctl surface                                               */
/* ---------------------------------------------------------------------- */

static long ccci_md_ioctl(struct ccci_modem *md, unsigned int cmd,
			  unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	unsigned int val;

	switch (cmd) {
	case CCCI_IOC_GET_MD_STATE:
		val = READ_ONCE(md->state);
		if (put_user(val, (unsigned int __user *)uarg))
			return -EFAULT;
		return 0;
	case CCCI_IOC_GET_MD_INFO:
		val = (READ_ONCE(md->boot_stage) << 8) |
		      (READ_ONCE(md->state) & 0xff);
		if (put_user(val, (unsigned int __user *)uarg))
			return -EFAULT;
		return 0;
	case CCCI_IOC_GET_MD_TYPE:
	case CCCI_IOC_GET_MD_EX_TYPE:
		/* World phone modem, LTE + 2G/3G (vendor MD_LWG value). */
		val = 5;
		if (put_user(val, (unsigned int __user *)uarg))
			return -EFAULT;
		return 0;
	case CCCI_IOC_GET_MD_MEM_SIZE:
		val = md->hw.smem_size;
		if (put_user(val, (unsigned int __user *)uarg))
			return -EFAULT;
		return 0;
	case CCCI_IOC_SMEM_BASE:
		/*
		 * ccci_fsd maps the shared window to skip the CLDMA round
		 * trip; a board without a modem-share region reports 0 and the
		 * daemon falls back to the character device.
		 */
		val = md->hw.smem_len ? (u32)md->hw.smem_base : 0;
		if (put_user(val, (unsigned int __user *)uarg))
			return -EFAULT;
		return 0;
	case CCCI_IOC_SMEM_LEN:
		val = md->hw.smem_len;
		if (put_user(val, (unsigned int __user *)uarg))
			return -EFAULT;
		return 0;
	case CCCI_IOC_SMEM_GET_STATE:
		val = READ_ONCE(md->state) == CCCI_STATE_READY;
		if (put_user(val, (unsigned int __user *)uarg))
			return -EFAULT;
		return 0;
	case CCCI_IOC_SMEM_TX_NOTIFY:
	case CCCI_IOC_SMEM_SET_STATE:
		/* The polled shared memory protocol is not used by ccci_fsd. */
		return -EOPNOTSUPP;
	case CCCI_IOC_POWER_ON_MD:
	case CCCI_IOC_POWER_ON_MD_REQUEST:
	case CCCI_IOC_DO_START_MD:
		return ccci_md_power_on(md);
	case CCCI_IOC_POWER_OFF_MD:
	case CCCI_IOC_POWER_OFF_MD_REQUEST:
	case CCCI_IOC_DO_STOP_MD:
		return ccci_md_power_off(md, "userspace request");
	case CCCI_IOC_MD_RESET:
	case CCCI_IOC_DO_MD_RST:
		return ccci_md_reset(md, "userspace request");
	case CCCI_IOC_FORCE_MD_ASSERT:
		dev_warn(md->dev, "modem assert requested by %s\n",
			 current->comm);
		return ccci_md_reset(md, "forced assert");
	case CCCI_IOC_ENTER_DEEP_FLIGHT:
		if (READ_ONCE(md->state) == CCCI_STATE_READY)
			return -EBUSY;
		md->flown = true;
		return 0;
	case CCCI_IOC_LEAVE_DEEP_FLIGHT:
		if (!md->flown)
			return -EINVAL;
		md->flown = false;
		return ccci_md_power_on(md);
	case CCCI_IOC_SEND_STOP_MD_REQUEST:
	case CCCI_IOC_SEND_START_MD_REQUEST:
	case CCCI_IOC_SEND_RUN_TIME_DATA:
	case CCCI_IOC_ALLOC_MD_LOG_MEM:
		/* Advisories only: the runtime data is part of the handshake. */
		return 0;
	case CCCI_IOC_SIM_SWITCH:
	case CCCI_IOC_SET_MD_IMG_EXIST:
	case CCCI_IOC_STORE_MD_TYPE:
	case CCCI_IOC_STORE_SIM_MODE:
	case CCCI_IOC_SEND_ICUSB_NOTIFY:
	case CCCI_IOC_SEND_BATTERY_INFO:
	case CCCI_IOC_RELOAD_MD_TYPE:
	case CCCI_IOC_SET_CCIF_CG:
	case CCCI_IOC_SET_EFUN:
	case CCCI_IOC_DL_TRAFFIC_CONTROL:
	case CCCI_IOC_RESET_AP: {
		int size = _IOC_SIZE(cmd);

		if (size > sizeof(val))
			return -EINVAL;
		val = 0;
		if (size && copy_from_user(&val, uarg, size))
			return -EFAULT;
		if (cmd == CCCI_IOC_SIM_SWITCH)
			dev_info(md->dev, "SIM switch to slot %u requested\n",
				 val);
		/* Baseband advisories, nothing for the kernel to program. */
		return 0;
	}
	case CCCI_IOC_GET_CFG_SETTING:
	case CCCI_IOC_GET_MD_SBP_CFG:
	case CCCI_IOC_SIM_SWITCH_TYPE:
	case CCCI_IOC_GET_SIM_TYPE:
		val = 0;
		if (put_user(val, (unsigned int __user *)uarg))
			return -EFAULT;
		return 0;
	case CCCI_IOC_GET_MD_PROTOCOL_TYPE: {
		char proto[16] = "AP_TST";

		if (copy_to_user(uarg, proto, sizeof(proto)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static long ccci_char_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct ccci_port *port = file->private_data;
	struct ccci_modem *md = port->md;

	switch (cmd) {
	case CCCI_IPC_WAIT_MD_READY:
		if (READ_ONCE(md->state) == CCCI_STATE_READY)
			return 0;
		return -EAGAIN;
	case CCCI_IPC_RESET_RECV:
	case CCCI_IPC_RESET_SEND:
		ccci_port_flush(port);
		return 0;
	default:
		return ccci_md_ioctl(md, cmd, arg);
	}
}

#ifdef CONFIG_COMPAT
static long ccci_char_compat_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	return ccci_char_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations ccci_char_fops = {
	.owner = THIS_MODULE,
	.open = ccci_char_open,
	.release = ccci_char_release,
	.read = ccci_char_read,
	.write = ccci_char_write,
	.poll = ccci_char_poll,
	.unlocked_ioctl = ccci_char_ioctl,
	.compat_ioctl = ccci_char_compat_ioctl,
	.llseek = noop_llseek,
};

/* ---------------------------------------------------------------------- */
/* /dev/ccci_ioctl0..4                                                    */
/* ---------------------------------------------------------------------- */

static int ccci_ioctl_dev_open(struct inode *inode, struct file *file)
{
	file->private_data = container_of(inode->i_cdev,
					  struct ccci_ioctl_dev, cdev);
	return nonseekable_open(inode, file);
}

static long ccci_ioctl_dev_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct ccci_ioctl_dev *idev = file->private_data;

	return ccci_md_ioctl(idev->char_dev->md, cmd, arg);
}

#ifdef CONFIG_COMPAT
static long ccci_ioctl_dev_compat_ioctl(struct file *file, unsigned int cmd,
					 unsigned long arg)
{
	return ccci_ioctl_dev_ioctl(file, cmd,
				    (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations ccci_ioctl_fops = {
	.owner = THIS_MODULE,
	.open = ccci_ioctl_dev_open,
	.unlocked_ioctl = ccci_ioctl_dev_ioctl,
	.compat_ioctl = ccci_ioctl_dev_compat_ioctl,
	.llseek = noop_llseek,
};

/* ---------------------------------------------------------------------- */
/* Registration                                                           */
/* ---------------------------------------------------------------------- */

static struct ccci_port *ccci_port_alloc(struct ccci_modem *md,
					 const struct ccci_char_descr *d)
{
	struct ccci_port *port;

	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return NULL;

	port->md = md;
	port->rx_ch = d->rx_ch;
	port->tx_ch = d->tx_ch;
	port->type = CCCI_PORT_TYPE_CHAR;
	port->critical_index = d->critical_index;
	port->queue_max = d->queue_max;
	strscpy(port->name, d->name, sizeof(port->name));
	skb_queue_head_init(&port->rx_queue);
	spin_lock_init(&port->rx_lock);
	mutex_init(&port->open_lock);
	init_waitqueue_head(&port->rx_wait);

	return port;
}

static void ccci_port_destroy(struct ccci_port *port)
{
	/*
	 * Block new users, then unregister so that the receive path stops
	 * handing packets to a dying port.
	 */
	mutex_lock(&port->open_lock);
	port->is_open = false;
	mutex_unlock(&port->open_lock);

	ccci_port_unregister(port->md, port);
	if (port->dev) {
		cdev_device_del(&port->cdev, port->dev);
		put_device(port->dev);
		port->dev = NULL;
	}
	skb_queue_purge(&port->rx_queue);
	kfree(port);
}

static int ccci_port_create(struct ccci_char *chr,
			    const struct ccci_char_descr *d, dev_t devt)
{
	struct ccci_port *port;
	int ret;

	port = ccci_port_alloc(chr->md, d);
	if (!port)
		return -ENOMEM;

	ret = ccci_port_register(chr->md, port);
	if (ret) {
		kfree(port);
		return ret;
	}

	cdev_init(&port->cdev, &ccci_char_fops);
	port->cdev.owner = THIS_MODULE;

	port->dev = device_create(chr->class, chr->md->dev, devt, port,
				  "%s", d->name);
	if (IS_ERR(port->dev)) {
		ret = PTR_ERR(port->dev);
		port->dev = NULL;
		ccci_port_unregister(chr->md, port);
		kfree(port);
		return ret;
	}

	ret = cdev_device_add(&port->cdev, port->dev);
	if (ret) {
		device_destroy(chr->class, devt);
		ccci_port_unregister(chr->md, port);
		kfree(port);
		return ret;
	}

	chr->ports[chr->nports++] = port;
	return 0;
}

int ccci_char_init(struct ccci_modem *md)
{
	struct ccci_char *chr;
	unsigned int total, i;
	int ret;

	chr = kzalloc(sizeof(*chr), GFP_KERNEL);
	if (!chr)
		return -ENOMEM;
	chr->md = md;
	md->char_dev = chr;

	total = ARRAY_SIZE(ccci_char_ports) + CCCI_IOCTL_DEVS;
	ret = alloc_chrdev_region(&chr->devid, 0, total, "ccci");
	if (ret) {
		kfree(chr);
		md->char_dev = NULL;
		return ret;
	}

	/* class_create() takes a single argument since 6.4. */
	chr->class = class_create("ccci");
	if (IS_ERR(chr->class)) {
		ret = PTR_ERR(chr->class);
		goto err_region;
	}

	for (i = 0; i < ARRAY_SIZE(ccci_char_ports); i++) {
		dev_t devt = MKDEV(MAJOR(chr->devid), MINOR(chr->devid) + i);

		ret = ccci_port_create(chr, &ccci_char_ports[i], devt);
		if (ret)
			goto err_chars;
	}

	/* /dev/ccci_ioctl0..4: the modem wide control surface. */
	for (i = 0; i < CCCI_IOCTL_DEVS; i++) {
		struct ccci_ioctl_dev *idev;
		dev_t devt = MKDEV(MAJOR(chr->devid),
				   MINOR(chr->devid) +
				   ARRAY_SIZE(ccci_char_ports) + i);

		idev = kzalloc(sizeof(*idev), GFP_KERNEL);
		if (!idev) {
			ret = -ENOMEM;
			goto err_chars;
		}
		idev->char_dev = chr;
		snprintf(idev->name, sizeof(idev->name), "ccci_ioctl%u", i);
		cdev_init(&idev->cdev, &ccci_ioctl_fops);
		idev->cdev.owner = THIS_MODULE;

		idev->dev = device_create(chr->class, md->dev, devt, idev,
					  "%s", idev->name);
		if (IS_ERR(idev->dev)) {
			ret = PTR_ERR(idev->dev);
			kfree(idev);
			goto err_chars;
		}
		ret = cdev_device_add(&idev->cdev, idev->dev);
		if (ret) {
			device_destroy(chr->class, devt);
			kfree(idev);
			goto err_chars;
		}
		chr->ioctl_devs[chr->nioctl++] = idev;
	}

	return 0;

err_chars:
	ccci_char_deinit(md);
	return ret;
err_region:
	unregister_chrdev_region(chr->devid, total);
	kfree(chr);
	md->char_dev = NULL;
	return ret;
}

void ccci_char_deinit(struct ccci_modem *md)
{
	struct ccci_char *chr = ccci_char_of(md);
	unsigned int total;
	int i;

	if (!chr)
		return;

	for (i = chr->nioctl - 1; i >= 0; i--) {
		struct ccci_ioctl_dev *idev = chr->ioctl_devs[i];

		cdev_device_del(&idev->cdev, idev->dev);
		put_device(idev->dev);
		kfree(idev);
	}
	chr->nioctl = 0;

	while (chr->nports)
		ccci_port_destroy(chr->ports[--chr->nports]);

	if (chr->class) {
		class_destroy(chr->class);
		chr->class = NULL;
	}

	total = ARRAY_SIZE(ccci_char_ports) + CCCI_IOCTL_DEVS;
	unregister_chrdev_region(chr->devid, total);

	kfree(chr);
	md->char_dev = NULL;
}
