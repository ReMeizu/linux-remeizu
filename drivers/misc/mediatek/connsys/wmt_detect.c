/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Meizu Linux mainline project
 *
 * /dev/wmtdetect - CONSYS detect and bring-up control device.
 *
 * Userspace (wmt_loader, or its open re-implementation) drives the
 * connectivity stack through this node:
 *
 *	open("/dev/wmtdetect")
 *	COMBO_IOCTL_EXT_CHIP_PWR_ON	 try to power an external combo chip
 *	COMBO_IOCTL_EXT_CHIP_DETECT	 0 when no external chip is present
 *	COMBO_IOCTL_GET_SOC_CHIP_ID	 chip id of the on-SoC CONNSYS
 *	COMBO_IOCTL_SET_CHIP_ID		 select the chip description to use
 *	COMBO_IOCTL_DO_MODULE_INIT	 map registers, power the block, arm IRQs
 *
 * All Meizu MT6735/MT6737M/MT6750/MT6755/MT6797 devices use the integrated
 * CONNSYS, so the external-chip path always reports "absent".
 */

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "connsys.h"

#define CONNSYS_DRIVER_NAME	"mtk_connsys"

struct connsys_dev *connsys_instance;
EXPORT_SYMBOL_GPL(connsys_instance);

/* ------------------------------------------------------------------ */
/* Module wide setup                                                  */
/* ------------------------------------------------------------------ */

u32 connsys_soc_chip_id(struct connsys_dev *cd)
{
	struct device_node *np = cd->dev->of_node;
	u32 chip_id;

	if (of_property_read_u32(np, "mediatek,connsys-chip-id", &chip_id))
		chip_id = CONNSYS_CHIP_MT6735;

	return chip_id;
}

int connsys_set_chip_id(struct connsys_dev *cd, u32 chip_id)
{
	lockdep_assert_held(&cd->lock);

	if (!connsys_soc_by_chip_id(chip_id))
		return -EOPNOTSUPP;

	cd->chip_id = chip_id;
	cd->soc = connsys_soc_by_chip_id(chip_id);

	return 0;
}

/**
 * connsys_module_init - bring the CONSYS stack up for a chip id
 *
 * Maps the register windows, powers the block, wires the shared DRAM
 * window and requests the interrupts.  Called from COMBO_IOCTL_DO_MODULE_INIT.
 */
int connsys_module_init(struct connsys_dev *cd)
{
	int ret;

	mutex_lock(&cd->lock);

	if (cd->init_done) {
		ret = 0;
		goto out;
	}

	ret = connsys_power_on(cd);
	if (ret)
		goto out;

	connsys_activate_irqs(cd);
	cd->init_done = true;

	dev_info(cd->dev, "%s CONSYS initialised (chip id 0x%04x)\n",
		 cd->soc->name, cd->chip_id);

out:
	mutex_unlock(&cd->lock);
	return ret;
}

void connsys_module_cleanup(struct connsys_dev *cd)
{
	mutex_lock(&cd->lock);

	if (!cd->init_done) {
		mutex_unlock(&cd->lock);
		return;
	}

	connsys_wmt_power_off(cd);
	connsys_deactivate_irqs(cd);
	connsys_power_off(cd);
	cd->init_done = false;

	dev_info(cd->dev, "CONNSYS released\n");
	mutex_unlock(&cd->lock);
}

/* ------------------------------------------------------------------ */
/* /dev/wmtdetect                                                     */
/* ------------------------------------------------------------------ */

static ssize_t connsys_detect_read(struct file *filp, char __user *buf,
				   size_t count, loff_t *ppos)
{
	return 0;
}

static ssize_t connsys_detect_write(struct file *filp,
				    const char __user *buf, size_t count,
				    loff_t *ppos)
{
	return 0;
}

static int connsys_detect_ext_chip_pwr(struct connsys_dev *cd, bool on)
{
	/*
	 * Every supported Meizu device integrates CONNSYS; there is no
	 * SDIO combo chip and therefore no external power rail to drive.
	 */
	dev_dbg(cd->dev, "external combo chip %s: not present\n",
		on ? "power-on" : "power-off");
	return on ? -ENODEV : 0;
}

static long connsys_detect_ioctl(struct file *filp, unsigned int cmd,
				 unsigned long arg)
{
	struct connsys_dev *cd = connsys_instance;
	int chip_id = 0;
	int ret = 0;

	if (!cd)
		return -ENODEV;

	switch (cmd) {
	case COMBO_IOCTL_GET_CHIP_ID:
		return chip_id;

	case COMBO_IOCTL_SET_CHIP_ID:
		mutex_lock(&cd->lock);
		ret = connsys_set_chip_id(cd, arg);
		mutex_unlock(&cd->lock);
		break;

	case COMBO_IOCTL_GET_SOC_CHIP_ID:
		return connsys_soc_chip_id(cd);

	case COMBO_IOCTL_EXT_CHIP_PWR_ON:
	case COMBO_IOCTL_EXT_CHIP_PWR_OFF:
		return connsys_detect_ext_chip_pwr(cd,
						   cmd == COMBO_IOCTL_EXT_CHIP_PWR_ON);

	case COMBO_IOCTL_EXT_CHIP_DETECT:
		/* 0 means "no external combo chip", which is always our case */
		dev_dbg(cd->dev, "no external combo chip on %s\n", cd->soc->name);
		return 0;

	case COMBO_IOCTL_DO_SDIO_AUDS:
		return 0;

	case COMBO_IOCTL_DO_MODULE_INIT:
		if (arg)
			connsys_set_chip_id(cd, arg);
		return connsys_module_init(cd);

	case COMBO_IOCTL_MODULE_CLEANUP:
		connsys_module_cleanup(cd);
		break;

	default:
		dev_dbg(cd->dev, "unknown wmtdetect ioctl 0x%08x\n", cmd);
		return -EINVAL;
	}

	return ret;
}

static const struct file_operations connsys_detect_fops = {
	.owner		= THIS_MODULE,
	.read		= connsys_detect_read,
	.write		= connsys_detect_write,
	.unlocked_ioctl	= connsys_detect_ioctl,
	.compat_ioctl	= connsys_detect_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice connsys_detect_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "wmtdetect",
	.fops	= &connsys_detect_fops,
};

/* ------------------------------------------------------------------ */
/* Probe / remove                                                     */
/* ------------------------------------------------------------------ */

static int connsys_probe(struct platform_device *pdev)
{
	struct connsys_dev *cd;
	int ret;

	cd = devm_kzalloc(&pdev->dev, sizeof(*cd), GFP_KERNEL);
	if (!cd)
		return -ENOMEM;

	cd->dev = &pdev->dev;
	mutex_init(&cd->lock);
	spin_lock_init(&cd->launch_lock);
	init_waitqueue_head(&cd->launcher_wq);
	cd->timeout_ms = 100;
	/* Optional interrupts: the bindings differ slightly per SoC. */
	ret = platform_get_irq_byname_optional(pdev, "bgf");
	cd->bgf_irq = ret > 0 ? ret : -ENXIO;
	ret = platform_get_irq_byname_optional(pdev, "wdt");
	cd->wdt_irq = ret > 0 ? ret : -ENXIO;
	ret = platform_get_irq_byname_optional(pdev, "conn2ap");
	cd->conn2ap_irq = ret > 0 ? ret : -ENXIO;
	cd->state = CONNSYS_POWER_OFF;

	ret = kfifo_alloc(&cd->launch_fifo, CONNSYS_LAUNCH_FIFO, GFP_KERNEL);
	if (ret)
		return ret;

	ret = of_property_read_u32(pdev->dev.of_node, "mediatek,connsys-chip-id",
				   &cd->chip_id);
	if (ret)
		cd->chip_id = CONNSYS_CHIP_MT6735;

	cd->soc = connsys_soc_by_chip_id(cd->chip_id);
	if (!cd->soc) {
		dev_err(&pdev->dev, "unsupported chip id 0x%04x\n", cd->chip_id);
		return -EINVAL;
	}

	ret = connsys_map_resources(cd);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, cd);
	connsys_instance = cd;

	ret = connsys_stp_init(cd);
	if (ret)
		goto err_out;

	ret = connsys_wmt_init(cd);
	if (ret)
		goto err_out;

	ret = connsys_stpwmt_init(cd);
	if (ret)
		goto err_wmt;

	INIT_WORK(&cd->rx_work, connsys_rx_work);

	ret = connsys_wlan_init(cd);
	if (ret) {
		dev_warn(&pdev->dev, "cfg80211 wlan0 not registered: %d\n", ret);
	} else {
		/* the WLAN driver may use its own STP channel from now on */
		cd->wlan_ready = true;
	}

	ret = connsys_chrdevs_init(cd);
	if (ret)
		goto err_wlan;

	ret = misc_register(&connsys_detect_dev);
	if (ret) {
		dev_err(&pdev->dev, "cannot register /dev/wmtdetect: %d\n", ret);
		goto err_chrdevs;
	}

	dev_info(&pdev->dev, "%s CONSYS controller, chip id 0x%04x\n",
		 cd->soc->name, cd->chip_id);

	return 0;

err_chrdevs:
	connsys_chrdevs_exit(cd);
err_wlan:
	if (cd->wlan_ready)
		connsys_wlan_exit(cd);
	connsys_stpwmt_exit(cd);
err_wmt:
	connsys_wmt_exit(cd);
	connsys_stp_exit(cd);
err_out:
	connsys_unmap_resources(cd);
	connsys_instance = NULL;

	return ret;
}

static void connsys_remove(struct platform_device *pdev)
{
	struct connsys_dev *cd = platform_get_drvdata(pdev);

	misc_deregister(&connsys_detect_dev);
	connsys_chrdevs_exit(cd);
	if (cd->wlan_ready)
		connsys_wlan_exit(cd);
	connsys_stpwmt_exit(cd);
	connsys_wmt_exit(cd);
	connsys_stp_exit(cd);
	connsys_module_cleanup(cd);
	connsys_unmap_resources(cd);
	platform_set_drvdata(pdev, NULL);
	if (connsys_instance == cd)
		connsys_instance = NULL;
}

static const struct of_device_id connsys_of_match[] = {
	{ .compatible = "mediatek,mt6735-connsys" },
	{ .compatible = "mediatek,mt6755-connsys" },
	{ .compatible = "mediatek,mt6797-connsys" },
	{ .compatible = "mediatek,connsys-soc" },
	{ }
};
MODULE_DEVICE_TABLE(of, connsys_of_match);

static struct platform_driver connsys_driver = {
	.probe	= connsys_probe,
	.remove	= connsys_remove,
	.driver	= {
		.name		= CONNSYS_DRIVER_NAME,
		.of_match_table	= connsys_of_match,
	},
};
module_platform_driver(connsys_driver);

MODULE_DESCRIPTION("MediaTek CONSYS Wi-Fi/BT/GPS combo controller");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CONNSYS");
