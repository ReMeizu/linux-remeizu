// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6797 IR transmitter (IRTX) controller
 *
 * The MX6 (Meizu M95) carries the SoC IR LED on GPIO137 (pad function
 * IRTX_OUT, mode 5), driven by the IRTX block at 0x1101d000.  Unlike the
 * receiver-only mtk-cir driver this block also transmits: a pattern of
 * mark/space slots is played out of its wave FIFO, with a PWM channel in
 * memory mode providing the slot time base and IRTXMT the carrier that
 * fills every mark.
 *
 * Interfaces:
 *
 *  - rc-core transmit (LIRC): ir-ctl / lircd / in-kernel encoders hand over
 *    raw durations, an edge encoder here turns them into the FIFO pattern.
 *  - /dev/irtx with the MTK vendor ioctl ABI (carrier frequency and LED pad
 *    enable).  The vendor write() blob format is not reproduced: its slot
 *    time base is not documented and guessing it would put a wrong carrier
 *    on the air, so write() returns -EOPNOTSUPP and callers use rc-core.
 *
 * Register layout follows the vendor 3.18 driver
 * (drivers/misc/mediatek/irtx/mt6797/mt_irtx.{c,h}) and the MT6797 PWM
 * block used by pwm-mediatek.c.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#include <media/rc-core.h>

#define IRTX_DRV_NAME			"mtk-irtx"

#define IRTXCFG				0x000
#define IRTXCFG_START			BIT(0)
#define IRTXCFG_MODE_SW			(0x3u << 1)
#define IRTXCFG_SWO			BIT(3)
#define IRTXCFG_B_ORD_LSB		BIT(4)
#define IRTXCFG_IR_OS			BIT(6)
#define IRTXCFG_IR_INV			BIT(7)
#define IRTXCFG_BIT_NUM(n)		((((n) & 0x7f) << 8))
#define IRTXCFG_RESET_VALUE		(IRTXCFG_IR_INV | IRTXCFG_SWO)

#define IRTXD0				0x004
#define IRTXD1				0x008
#define IRTXD2				0x00c
#define IRTX_L0H			0x010
#define IRTX_L0L			0x014
#define IRTX_L1H			0x018
#define IRTX_L1L			0x01c
#define IRTXSYNCH			0x020
#define IRTXSYNCL			0x024
#define IRTXMT				0x028

/* MT6797 PWM: IRTX uses PWM3 in memory mode as its slot time base */
#define PWM_BASE_ADDR			0x11006000
#define PWM_ENABLE			0x000
#define PWM_ENABLE_MASK			0x0000000f
#define PWM_REG_STRIDE			0x040
#define PWM_CH(n)			(PWM_BASE_ADDR + PWM_REG_STRIDE * (n))
#define PWM_CON				0x00
#define PWM_CON_CLKSEL_NEW		BIT(3)
#define PWM_CON_SRCSEL_MEMORY		BIT(5)
#define PWM_CON_MODE_MEMORY		BIT(6)
#define PWM_CON_STOP_BITS(b)		(((b) << 9) & GENMASK(14, 9))
#define PWM_HI_DURATION			0x04
#define PWM_LOW_DURATION		0x08
#define PWM_GUARD_DURATION		0x0c
#define PWM_BUF0_BASE			0x10
#define PWM_BUF0_SIZE			0x14
#define PWM_WAVE_NUM			0x28
#define PWM_SEND_WAVENUM		0x34
#define PWM_3DLCM_ENABLE		0x1d0

/*
 * Each PWM wave word is one IRTX slot: bits 31..16 are the LED-on part and
 * bits 15..0 the carrier period inside it.  A word with the mark part
 * cleared and only the carrier left is an idle (LED-off) slot, which is how
 * a space is produced between two marks.
 */
#define IRTX_SLOT_WAVE_LEN		0x3f

#define IRTX_CLOCK_HZ			26000000
#define IRTX_CARRIER_MIN_HZ		30000
#define IRTX_CARRIER_MAX_HZ		60000
#define IRTX_CARRIER_HZ_DEFAULT		38000
#define IRTX_MAX_SLOTS			1024
#define IRTX_DEFAULT_TIMEOUT		200000

/* Legacy ioctl ABI of the Flyme IR HAL (mt_irtx.h) */
#define IRTX_IOC_SET_CARRIER_FREQ	_IOW('R', 0, unsigned int)
#define IRTX_IOC_SET_IRTX_LED_EN	_IOW('R', 10, unsigned int)

struct mtk_irtx {
	struct device *dev;
	void __iomem *base;
	void __iomem *pwm_base;
	struct clk *clk;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_default;
	struct pinctrl_state *pins_led;
	struct rc_dev *rc;
	struct miscdevice misc;
	struct mutex lock;		/* serialises both transmit paths */
	unsigned int carrier;
	unsigned int pwm_div;
	bool led_enabled;
};

static inline void irtx_write(struct mtk_irtx *ir, u32 val, u32 reg)
{
	writel(val, ir->base + reg);
}

static inline u32 irtx_read(struct mtk_irtx *ir, u32 reg)
{
	return readl(ir->base + reg);
}

static void irtx_select_pins(struct mtk_irtx *ir, bool led)
{
	if (IS_ERR_OR_NULL(ir->pinctrl))
		return;
	pinctrl_select_state(ir->pinctrl,
			     led ? ir->pins_led : ir->pins_default);
}

/**
 * irtx_program_carrier() - set the carrier modulation
 * @ir: controller
 * @duty_percent: carrier duty cycle
 *
 * IRTXMT holds the carrier period in 26 MHz cycles in the low half and the
 * LED-on part of that period in the upper half.  The vendor driver transmits
 * at a third of the period, which is what 38 kHz IR receivers expect.
 */
static void irtx_program_carrier(struct mtk_irtx *ir, int duty_percent)
{
	unsigned int cwt, cdt;

	cwt = DIV_ROUND_CLOSEST(IRTX_CLOCK_HZ, ir->carrier);
	if (!cwt)
		cwt = 1;
	cdt = DIV_ROUND_CLOSEST(cwt * duty_percent, 100);
	if (!cdt)
		cdt = 1;

	irtx_write(ir, (cdt << 16) | cwt, IRTXMT);
}

static void irtx_pwm_disable(struct mtk_irtx *ir)
{
	writel(readl(ir->pwm_base + PWM_ENABLE) & ~PWM_ENABLE_MASK,
	       ir->pwm_base + PWM_ENABLE);
}

/**
 * irtx_build_waves() - turn rc-core durations into PWM wave words
 * @ir: controller
 * @bufs: destination, @slots entries
 * @duration: alternating mark/space lengths in microseconds
 * @count: number of entries in @duration
 * @slots: number of words produced
 *
 * One word is one carrier period long.  A mark is a run of words with the
 * LED-on field set, a space is a run of words with it cleared; the LED is
 * switched by the PWM high duration, which the IRTX block gates onto the
 * IRTX_OUT pad.  Durations are rounded to whole carrier periods, which is
 * the resolution the hardware can produce.
 */
static int irtx_build_waves(struct mtk_irtx *ir, u32 *bufs,
			    const unsigned int *duration, unsigned int count,
			    unsigned int *slots)
{
	unsigned int period_ps = DIV_ROUND_CLOSEST(1000000000000ULL, ir->carrier);
	unsigned int i, n = 0;

	for (i = 0; i < count; i++) {
		unsigned int us = duration[i];
		unsigned int periods;

		if (!us)
			continue;

		periods = DIV_ROUND_CLOSEST(us * 1000000ULL, period_ps);
		if (!periods)
			periods = 1;
		if (n + periods > IRTX_MAX_SLOTS)
			periods = IRTX_MAX_SLOTS - n;
		if (!periods)
			break;

		while (periods--) {
			u32 on = (i & 1) ? 0 : IRTX_SLOT_WAVE_LEN;

			bufs[n++] = (on << 16) | IRTX_SLOT_WAVE_LEN;
		}
	}

	*slots = n;
	return 0;
}

static int irtx_pwm_load(struct mtk_irtx *ir, dma_addr_t addr, unsigned int slots)
{
	void __iomem *pwm = ir->pwm_base;
	u32 con;

	irtx_pwm_disable(ir);

	/* New-mode source, memory data, one wave, idle/guard low. */
	con = PWM_CON_CLKSEL_NEW | PWM_CON_SRCSEL_MEMORY | PWM_CON_MODE_MEMORY |
	      PWM_CON_STOP_BITS(31) | (ir->pwm_div & 0x7);
	writel(con, pwm + PWM_CON);
	writel(IRTX_SLOT_WAVE_LEN, pwm + PWM_HI_DURATION);
	writel(0, pwm + PWM_LOW_DURATION);
	writel(0, pwm + PWM_GUARD_DURATION);
	writel(lower_32_bits(addr), pwm + PWM_BUF0_BASE);
	writel(slots - 1, pwm + PWM_BUF0_SIZE);
	writel(1, pwm + PWM_WAVE_NUM);
	writel(1, pwm + PWM_SEND_WAVENUM);

	return 0;
}

/**
 * irtx_transmit() - push one mark/space sequence out of the LED
 * @ir: controller, lock held
 * @duration: alternating mark/space lengths in microseconds
 * @count: number of entries
 *
 * The DMA buffer is freed once the PWM has drained the FIFO.  The caller's
 * durations are the only timing source (the PWM completion interrupt is not
 * routed on this SoC), so the wait is derived from them like the vendor
 * driver's msleep(count * 8 / 1000), in microseconds and with a margin.
 */
static int irtx_transmit(struct mtk_irtx *ir, const unsigned int *duration,
			 unsigned int count)
{
	dma_addr_t phy;
	u32 *virt;
	unsigned int slots, total_us = 0, i;
	u32 cfg;
	int ret;

	virt = dma_alloc_coherent(ir->dev, IRTX_MAX_SLOTS * sizeof(u32), &phy,
				  GFP_KERNEL);
	if (!virt)
		return -ENOMEM;

	ret = irtx_build_waves(ir, virt, duration, count, &slots);
	if (ret || !slots) {
		ret = -EINVAL;
		goto free;
	}
	for (i = 0; i < count; i++)
		total_us += duration[i];

	/* Software mode: one bit per PWM slot, LED inverted, LSB first. */
	cfg = IRTXCFG_MODE_SW | IRTXCFG_IR_OS | IRTXCFG_IR_INV |
	      IRTXCFG_B_ORD_LSB | IRTXCFG_BIT_NUM(0);
	irtx_write(ir, cfg, IRTXCFG);
	irtx_write(ir, 0, IRTX_L0H);
	irtx_write(ir, 0, IRTX_L1H);
	irtx_write(ir, 0, IRTX_L1L);
	irtx_write(ir, 0, IRTXSYNCH);
	irtx_write(ir, 0, IRTXSYNCL);
	irtx_program_carrier(ir, 33);

	ret = irtx_pwm_load(ir, phy, slots);
	if (ret)
		goto free;

	irtx_select_pins(ir, true);
	irtx_write(ir, irtx_read(ir, IRTXCFG) | IRTXCFG_START, IRTXCFG);
	writel(readl(ir->pwm_base + PWM_ENABLE) | PWM_ENABLE_MASK,
	       ir->pwm_base + PWM_ENABLE);

	usleep_range(total_us + 1000, total_us + 2000);

	irtx_write(ir, irtx_read(ir, IRTXCFG) & ~IRTXCFG_START, IRTXCFG);
	irtx_pwm_disable(ir);
	irtx_select_pins(ir, false);
	irtx_write(ir, IRTXCFG_RESET_VALUE, IRTXCFG);

free:
	dma_free_coherent(ir->dev, IRTX_MAX_SLOTS * sizeof(u32), virt, phy);
	return ret;
}

static int irtx_tx(struct rc_dev *rcdev, unsigned int *duration,
		   unsigned int count)
{
	struct mtk_irtx *ir = rcdev->priv;
	int ret;

	if (!count || (count & 1))
		return -EINVAL;

	mutex_lock(&ir->lock);
	ret = irtx_transmit(ir, duration, count);
	mutex_unlock(&ir->lock);

	return ret ? ret : count;
}

static int irtx_carrier_set(struct rc_dev *rcdev, u32 carrier)
{
	struct mtk_irtx *ir = rcdev->priv;

	if (carrier < IRTX_CARRIER_MIN_HZ || carrier > IRTX_CARRIER_MAX_HZ)
		return -EINVAL;

	mutex_lock(&ir->lock);
	ir->carrier = carrier;
	irtx_program_carrier(ir, 33);
	mutex_unlock(&ir->lock);

	return 0;
}

static int irtx_register_rc(struct mtk_irtx *ir)
{
	struct rc_dev *rc;
	int ret;

	rc = devm_rc_allocate_device(ir->dev, RC_DRIVER_IR_RAW_TX);
	if (!rc)
		return -ENOMEM;

	rc->priv = ir;
	rc->device_name = "MT6797 IR transmitter";
	rc->driver_name = IRTX_DRV_NAME;
	rc->tx_ir = irtx_tx;
	rc->s_tx_carrier = irtx_carrier_set;
	rc->allowed_protocols = RC_PROTO_BIT_ALL_IR_ENCODER;
	rc->map_name = RC_MAP_EMPTY;
	rc->dev.parent = ir->dev;
	rc->timeout = IRTX_DEFAULT_TIMEOUT;

	ret = devm_rc_register_device(ir->dev, rc);
	if (ret)
		return ret;

	ir->rc = rc;
	return 0;
}

/* ------------------------------ /dev/irtx ------------------------------ */

static ssize_t irtx_legacy_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	return -EOPNOTSUPP;
}

static long irtx_legacy_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct mtk_irtx *ir = file->private_data;
	unsigned int value;

	switch (cmd) {
	case IRTX_IOC_SET_CARRIER_FREQ:
		if (get_user(value, (unsigned int __user *)arg))
			return -EFAULT;
		if (!value)
			return -EINVAL;
		mutex_lock(&ir->lock);
		ir->carrier = clamp_val(value, IRTX_CARRIER_MIN_HZ,
					IRTX_CARRIER_MAX_HZ);
		irtx_program_carrier(ir, 33);
		mutex_unlock(&ir->lock);
		return 0;

	case IRTX_IOC_SET_IRTX_LED_EN:
		if (get_user(value, (unsigned int __user *)arg))
			return -EFAULT;
		mutex_lock(&ir->lock);
		ir->led_enabled = !!(value & 0xf);
		irtx_select_pins(ir, ir->led_enabled);
		mutex_unlock(&ir->lock);
		return 0;

	default:
		return -ENOTTY;
	}
}

static int irtx_legacy_open(struct inode *inode, struct file *file)
{
	struct mtk_irtx *ir = container_of(file->private_data,
					   struct mtk_irtx, misc);

	file->private_data = ir;
	return 0;
}

static int irtx_legacy_release(struct inode *inode, struct file *file)
{
	struct mtk_irtx *ir = file->private_data;

	mutex_lock(&ir->lock);
	ir->led_enabled = false;
	irtx_select_pins(ir, false);
	mutex_unlock(&ir->lock);

	return 0;
}

static const struct file_operations irtx_legacy_fops = {
	.owner = THIS_MODULE,
	.open = irtx_legacy_open,
	.write = irtx_legacy_write,
	.release = irtx_legacy_release,
	.unlocked_ioctl = irtx_legacy_ioctl,
	.compat_ioctl = irtx_legacy_ioctl,
};

/* ------------------------------- probe -------------------------------- */

static int irtx_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct mtk_irtx *ir;
	unsigned long rate;
	int ret;

	ir = devm_kzalloc(dev, sizeof(*ir), GFP_KERNEL);
	if (!ir)
		return -ENOMEM;

	ir->dev = dev;
	ir->carrier = IRTX_CARRIER_HZ_DEFAULT;
	mutex_init(&ir->lock);

	ir->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ir->base))
		return PTR_ERR(ir->base);

	ir->clk = devm_clk_get(dev, "clk-irtx-main");
	if (IS_ERR(ir->clk))
		return dev_err_probe(dev, PTR_ERR(ir->clk),
				     "failed to get IRTX clock\n");

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pwm");
	if (!res)
		return dev_err_probe(dev, -ENODEV,
				     "missing PWM resource for slot timing\n");
	ir->pwm_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(ir->pwm_base))
		return PTR_ERR(ir->pwm_base);

	/*
	 * The PWM block runs from a divided 26 MHz clock; the vendor sets
	 * clk_div = 1, i.e. divider register value 0.
	 */
	rate = clk_get_rate(ir->clk);
	if (!rate)
		rate = IRTX_CLOCK_HZ;
	ir->pwm_div = max_t(unsigned int, DIV_ROUND_UP(rate, IRTX_CLOCK_HZ), 1) - 1;
	ir->pwm_div = min(ir->pwm_div, 7u);

	ir->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(ir->pinctrl)) {
		dev_dbg(dev, "no pinctrl, keeping the bootloader pad state\n");
	} else {
		ir->pins_default = pinctrl_lookup_state(ir->pinctrl,
							"irtx_gpio_default");
		ir->pins_led = pinctrl_lookup_state(ir->pinctrl,
						    "irtx_gpio_led_set");
		if (IS_ERR(ir->pins_default) || IS_ERR(ir->pins_led)) {
			dev_warn(dev, "IRTX pinctrl states missing\n");
			ir->pinctrl = NULL;
			ir->pins_default = NULL;
			ir->pins_led = NULL;
		}
	}

	ret = clk_prepare_enable(ir->clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable IRTX clock\n");

	irtx_write(ir, IRTXCFG_RESET_VALUE, IRTXCFG);
	irtx_program_carrier(ir, 33);
	irtx_pwm_disable(ir);

	ret = irtx_register_rc(ir);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register rc device\n");
		goto err_clk;
	}

	ir->misc.minor = MISC_DYNAMIC_MINOR;
	ir->misc.name = "irtx";
	ir->misc.fops = &irtx_legacy_fops;

	ret = misc_register(&ir->misc);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register /dev/irtx\n");
		goto err_clk;
	}

	platform_set_drvdata(pdev, ir);

	return 0;

err_clk:
	clk_disable_unprepare(ir->clk);
	return ret;
}

static void irtx_shutdown(struct platform_device *pdev)
{
	struct mtk_irtx *ir = platform_get_drvdata(pdev);

	irtx_select_pins(ir, false);
	irtx_pwm_disable(ir);
	irtx_write(ir, IRTXCFG_RESET_VALUE, IRTXCFG);
}

static void irtx_remove(struct platform_device *pdev)
{
	struct mtk_irtx *ir = platform_get_drvdata(pdev);

	misc_deregister(&ir->misc);
	irtx_pwm_disable(ir);
	clk_disable_unprepare(ir->clk);
}

static const struct of_device_id irtx_of_match[] = {
	{ .compatible = "mediatek,mt6797-irtx" },
	{ }
};
MODULE_DEVICE_TABLE(of, irtx_of_match);

static struct platform_driver irtx_driver = {
	.probe = irtx_probe,
	.remove = irtx_remove,
	.shutdown = irtx_shutdown,
	.driver = {
		.name = IRTX_DRV_NAME,
		.of_match_table = irtx_of_match,
	},
};
module_platform_driver(irtx_driver);

MODULE_AUTHOR("ReMeizu");
MODULE_DESCRIPTION("MediaTek MT6797 IR transmitter with rc-core TX");
MODULE_LICENSE("GPL");
