// SPDX-License-Identifier: GPL-2.0-only
/*
 * Texas Instruments LM3644 / LM3643 dual flash LED controller
 *
 * Copyright (C) 2026 Meizu fleet kernel port
 *
 * The Meizu MX6 (MT6797) drives its dual-tone rear camera flash through an
 * LM3644 I2C controller at 0x63 on the camera I2C bus, behind a hardware
 * enable pin (vendor DTS node "strobe_main", driver
 * drivers/misc/mediatek/flashlight/src/mt6797/constant_flashlight/).  The
 * vendor code only ever touches the eight registers below, using the same
 * register map for the LM3644 and the LM3643, so one driver serves both.
 * This is the LM3644 register map (the LM3643 has its timeout field in
 * register 0x07 and is handled through the "ti,lm3643" compatible):
 *
 *   0x01 enable        bit0 LED1 enable, bit1 LED2 enable
 *                      bit2 LED1 torch (0 = flash), bit3 LED2 torch
 *                      bit4 hardware EN pin enable
 *   0x03 LED1 flash current (bits 6:0, ~11.7 mA per step)
 *   0x04 LED2 flash current
 *   0x05 LED1 torch current (bits 6:0, ~2.9 mA per step)
 *   0x06 LED2 torch current
 *   0x07 LM3643 flash timeout (bits 3:0, 32 ms .. 512 ms)
 *   0x08 LM3644 flash timeout (bits 3:0, 40 ms .. 1.6 s)
 *   0x0a flag register 0
 *   0x0b flag register 1
 *
 * The driver exposes the LED flash class interface: the torch level is the
 * led_classdev brightness and the strobe is driven by the flash ops, which
 * is all a V4L2 flash LED user (v4l2-flash-led-class) needs.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/led-class-flash.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>

#include <dt-bindings/leds/common.h>

#define LM3644_NAME			"lm3644"

#define LM3644_REG_ENABLE		0x01
#define LM3644_REG_FLASH_LED1		0x03
#define LM3644_REG_FLASH_LED2		0x04
#define LM3644_REG_TORCH_LED1		0x05
#define LM3644_REG_TORCH_LED2		0x06
#define LM3643_REG_TIMEOUT		0x07
#define LM3644_REG_TIMEOUT		0x08
#define LM3644_REG_FLAG0		0x0a
#define LM3644_REG_FLAG1		0x0b
#define LM3644_REG_MAX			0x0b

/* Enable register bits, see the vendor FL_Enable_led*() callbacks */
#define LM3644_EN_LED1			BIT(0)
#define LM3644_EN_LED2			BIT(1)
#define LM3644_EN_TORCH_LED1		BIT(2)	/* 1 = torch, 0 = flash */
#define LM3644_EN_TORCH_LED2		BIT(3)
#define LM3644_EN_HW			BIT(4)

/* Current register layout: bits 6:0 are the 7 bit code */
#define LM3644_CURRENT_MASK		GENMASK(6, 0)
#define LM3644_CURRENT_MAX		0x7f

/* Flash safety timeout register: bits 3:0 */
#define LM3644_TIMEOUT_MASK		GENMASK(3, 0)
#define LM3644_TIMEOUT_LM3644_US	40000
#define LM3644_TIMEOUT_LM3644_MAX	15
#define LM3643_TIMEOUT_LM3643_US	32000
#define LM3643_TIMEOUT_LM3643_MAX	15

/* Flag register 1 bits */
#define LM3644_FLAG1_TIMEOUT		BIT(0)
#define LM3644_FLAG1_THERMAL		BIT(1)
#define LM3644_FLAG1_LED2_SHORT		BIT(2)
#define LM3644_FLAG1_LED1_SHORT		BIT(3)
#define LM3644_FLAG1_UVLO		BIT(4)
#define LM3644_FLAG1_OVP		BIT(5)
#define LM3644_FLAG1_LED2_THERMAL	BIT(6)
#define LM3644_FLAG1_LED1_THERMAL	BIT(7)

#define LM3644_FLASH_FAULT_OVER_VOLTAGE		BIT(0)
#define LM3644_FLASH_FAULT_TIMEOUT		BIT(1)
#define LM3644_FLASH_FAULT_OVER_TEMPERATURE	BIT(2)
#define LM3644_FLASH_FAULT_SHORT_CIRCUIT	BIT(3)
#define LM3644_FLASH_FAULT_OVER_CURRENT		BIT(4)
#define LM3644_FLASH_FAULT_UNDER_VOLTAGE	BIT(5)
#define LM3644_FLASH_FAULT_INPUT_LDO_OUT	BIT(6)
#define LM3644_FLASH_FAULT_OUTPUT_OVER_VOLTAGE	BIT(7)

/* Unit currents of the LM3644/LM3643 current DACs, in microamperes */
#define LM3644_TORCH_UA_PER_STEP	2930
#define LM3644_FLASH_UA_PER_STEP	11700

#define LM3644_TORCH_MAX_UA	(LM3644_CURRENT_MAX * LM3644_TORCH_UA_PER_STEP)
#define LM3644_FLASH_MAX_UA	(LM3644_CURRENT_MAX * LM3644_FLASH_UA_PER_STEP)
#define LM3644_TORCH_MIN_UA	LM3644_TORCH_UA_PER_STEP
#define LM3644_FLASH_MIN_UA	LM3644_FLASH_UA_PER_STEP

struct lm3644;

struct lm3644_led {
	struct lm3644 *priv;
	struct led_classdev_flash fled;
	bool is_led1;
};

/**
 * struct lm3644 - one LM3644/LM3643 instance
 * @dev: I2C client device
 * @regmap: register map
 * @lock: serialises the register accesses and the cached state
 * @leds: LED1 (cool white) and LED2 (warm white) flash LED class devices
 * @hw_en: hardware EN pin, active high, may be NULL
 * @enable: cached enable register value
 * @timeout_reg: register holding the flash safety timeout
 * @timeout_step_us: resolution of the timeout register
 * @timeout_max: maximum valid timeout register code
 */
struct lm3644 {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock;
	struct lm3644_led leds[2];
	struct gpio_desc *hw_en;
	unsigned int enable;
	unsigned int timeout_reg;
	unsigned int timeout_step_us;
	unsigned int timeout_max;
};

static struct lm3644_led *flash_to_lm3644_led(struct led_classdev_flash *fled)
{
	return container_of(fled, struct lm3644_led, fled);
}

static int lm3644_update_enable(struct lm3644 *priv, unsigned int set,
				unsigned int clear)
{
	unsigned int enable;
	int ret;

	enable = (priv->enable & ~clear) | set;
	enable &= ~LM3644_EN_HW;
	if (priv->hw_en)
		enable |= LM3644_EN_HW;

	ret = regmap_write(priv->regmap, LM3644_REG_ENABLE, enable);
	if (ret) {
		dev_err(priv->dev, "failed to write EN register: %d\n", ret);
		return ret;
	}

	priv->enable = enable;

	return 0;
}

static int lm3644_led_set(struct led_classdev *led_cdev,
			  enum led_brightness brightness)
{
	struct led_classdev_flash *fled = lcdev_to_flcdev(led_cdev);
	struct lm3644_led *led = flash_to_lm3644_led(fled);
	struct lm3644 *priv = led->priv;
	bool is_led1 = led->is_led1;
	unsigned int torch_reg = is_led1 ? LM3644_REG_TORCH_LED1 :
					   LM3644_REG_TORCH_LED2;
	unsigned int en_bit = is_led1 ? LM3644_EN_LED1 : LM3644_EN_LED2;
	unsigned int torch_bit = is_led1 ? LM3644_EN_TORCH_LED1 :
					   LM3644_EN_TORCH_LED2;
	unsigned int code;
	int ret;

	guard(mutex)(&priv->lock);

	if (!brightness)
		return lm3644_update_enable(priv, 0, en_bit | torch_bit);

	code = min_t(unsigned int, brightness, LM3644_CURRENT_MAX);
	ret = regmap_write(priv->regmap, torch_reg, code);
	if (ret)
		return ret;

	return lm3644_update_enable(priv, en_bit | torch_bit, 0);
}

static int lm3644_flash_brightness_set(struct led_classdev_flash *fled,
				       u32 brightness_ua)
{
	struct lm3644_led *led = flash_to_lm3644_led(fled);
	struct lm3644 *priv = led->priv;
	bool is_led1 = led->is_led1;
	unsigned int reg = is_led1 ? LM3644_REG_FLASH_LED1 :
				     LM3644_REG_FLASH_LED2;
	unsigned int code;

	code = DIV_ROUND_CLOSEST(brightness_ua, LM3644_FLASH_UA_PER_STEP);
	code = min_t(unsigned int, code, LM3644_CURRENT_MAX);

	guard(mutex)(&priv->lock);

	return regmap_write(priv->regmap, reg, code);
}

static int lm3644_flash_brightness_get(struct led_classdev_flash *fled,
				       u32 *brightness_ua)
{
	struct lm3644_led *led = flash_to_lm3644_led(fled);
	struct lm3644 *priv = led->priv;
	bool is_led1 = led->is_led1;
	unsigned int reg = is_led1 ? LM3644_REG_FLASH_LED1 :
				     LM3644_REG_FLASH_LED2;
	unsigned int val;
	int ret;

	guard(mutex)(&priv->lock);

	ret = regmap_read(priv->regmap, reg, &val);
	if (ret)
		return ret;

	*brightness_ua = (val & LM3644_CURRENT_MASK) * LM3644_FLASH_UA_PER_STEP;

	return 0;
}

static int lm3644_strobe_set(struct led_classdev_flash *fled, bool state)
{
	struct lm3644_led *led = flash_to_lm3644_led(fled);
	struct lm3644 *priv = led->priv;
	bool is_led1 = led->is_led1;
	unsigned int en_bit = is_led1 ? LM3644_EN_LED1 : LM3644_EN_LED2;
	unsigned int torch_bit = is_led1 ? LM3644_EN_TORCH_LED1 :
					   LM3644_EN_TORCH_LED2;

	guard(mutex)(&priv->lock);

	if (state) {
		/* Flash (not torch) mode: clear the torch bit, raise EN */
		return lm3644_update_enable(priv, en_bit, torch_bit);
	}

	return lm3644_update_enable(priv, 0, en_bit);
}

static int lm3644_strobe_get(struct led_classdev_flash *fled, bool *state)
{
	struct lm3644_led *led = flash_to_lm3644_led(fled);
	struct lm3644 *priv = led->priv;
	bool is_led1 = led->is_led1;
	unsigned int en_bit = is_led1 ? LM3644_EN_LED1 : LM3644_EN_LED2;
	unsigned int torch_bit = is_led1 ? LM3644_EN_TORCH_LED1 :
					   LM3644_EN_TORCH_LED2;

	guard(mutex)(&priv->lock);

	*state = (priv->enable & en_bit) && !(priv->enable & torch_bit);

	return 0;
}

static int lm3644_timeout_set(struct led_classdev_flash *fled, u32 timeout_us)
{
	struct lm3644 *priv = flash_to_lm3644_led(fled)->priv;
	unsigned int code;
	int ret;

	/* Round up: the safety timeout must never be shorter than requested */
	code = DIV_ROUND_UP(timeout_us, priv->timeout_step_us);
	code = clamp_val(code, 1, priv->timeout_max);

	guard(mutex)(&priv->lock);

	ret = regmap_update_bits(priv->regmap, priv->timeout_reg,
				 LM3644_TIMEOUT_MASK, code);
	if (ret)
		return ret;

	fled->timeout.val = code * priv->timeout_step_us;

	return 0;
}

static int lm3644_duration_set(struct led_classdev_flash *fled,
			       u32 duration_us)
{
	/* There is no separate pulse duration register: strobe is a latch */
	return lm3644_timeout_set(fled, duration_us);
}

static int lm3644_fault_get(struct led_classdev_flash *fled, u32 *fault)
{
	struct lm3644 *priv = flash_to_lm3644_led(fled)->priv;
	unsigned int val;
	int ret;

	guard(mutex)(&priv->lock);

	ret = regmap_read(priv->regmap, LM3644_REG_FLAG1, &val);
	if (ret)
		return ret;

	*fault = 0;
	if (val & LM3644_FLAG1_OVP)
		*fault |= LM3644_FLASH_FAULT_OVER_VOLTAGE;
	if (val & LM3644_FLAG1_TIMEOUT)
		*fault |= LM3644_FLASH_FAULT_TIMEOUT;
	if (val & (LM3644_FLAG1_THERMAL | LM3644_FLAG1_LED1_THERMAL |
		   LM3644_FLAG1_LED2_THERMAL))
		*fault |= LM3644_FLASH_FAULT_OVER_TEMPERATURE;
	if (val & (LM3644_FLAG1_LED1_SHORT | LM3644_FLAG1_LED2_SHORT))
		*fault |= LM3644_FLASH_FAULT_SHORT_CIRCUIT;
	if (val & LM3644_FLAG1_UVLO)
		*fault |= LM3644_FLASH_FAULT_UNDER_VOLTAGE;

	return 0;
}

static const struct led_flash_ops lm3644_flash_ops = {
	.flash_brightness_set = lm3644_flash_brightness_set,
	.flash_brightness_get = lm3644_flash_brightness_get,
	.strobe_set = lm3644_strobe_set,
	.strobe_get = lm3644_strobe_get,
	.timeout_set = lm3644_timeout_set,
	.fault_get = lm3644_fault_get,
	.duration_set = lm3644_duration_set,
};

static const struct regmap_config lm3644_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = LM3644_REG_MAX,
	.cache_type = REGCACHE_NONE,
};

static void lm3644_init_flash(struct lm3644 *priv,
			      struct led_classdev_flash *fled,
			      const char *default_label,
			      unsigned int max_flash_ua)
{
	struct led_classdev *led = &fled->led_cdev;

	led->name = default_label;
	led->max_brightness = LM3644_CURRENT_MAX;
	led->brightness_set_blocking = lm3644_led_set;
	led->flags = LED_CORE_SUSPENDRESUME | LED_DEV_CAP_FLASH;
	fled->ops = &lm3644_flash_ops;

	/* Both LEDs are on the same die and share the module current budget */
	fled->brightness.min = LM3644_FLASH_MIN_UA;
	fled->brightness.max = max_flash_ua;
	fled->brightness.step = LM3644_FLASH_UA_PER_STEP;
	fled->brightness.val = fled->brightness.min;

	fled->timeout.min = priv->timeout_step_us;
	fled->timeout.max = priv->timeout_max * priv->timeout_step_us;
	fled->timeout.step = priv->timeout_step_us;
	fled->timeout.val = priv->timeout_max * priv->timeout_step_us;

	fled->duration.min = fled->timeout.min;
	fled->duration.max = fled->timeout.max;
	fled->duration.step = fled->timeout.step;
	fled->duration.val = fled->timeout.val;
}

/*
 * The LM3644 carries the flash current limit of the module in its EFUSE
 * programming; on the MX6 module the vendor driver drives 606 mA per LED.
 */
#define LM3644_DEFAULT_FLASH_UA		606000

static int lm3644_register_leds(struct lm3644 *priv)
{
	struct device *dev = priv->dev;
	/*
	 * Both LEDs live in the same node, so the fwnode is left unset and the
	 * class device names come from the labels the driver provides.
	 */
	struct led_init_data init_data = { };
	u32 max_ua, current_ua = LM3644_DEFAULT_FLASH_UA;
	int ret;

	if (!device_property_read_u32(dev, "led-max-microamp", &current_ua))
		current_ua = min(current_ua, (u32)LM3644_FLASH_MAX_UA);
	max_ua = min(current_ua, (u32)LM3644_FLASH_MAX_UA);

	priv->leds[0].priv = priv;
	priv->leds[0].is_led1 = true;
	priv->leds[1].priv = priv;
	priv->leds[1].is_led1 = false;

	lm3644_init_flash(priv, &priv->leds[0].fled, "white:flash", max_ua);
	lm3644_init_flash(priv, &priv->leds[1].fled, "yellow:flash", max_ua);

	init_data.devicename = "lm3644";
	init_data.default_label = "white:flash";
	ret = devm_led_classdev_flash_register_ext(dev, &priv->leds[0].fled,
						   &init_data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register LED1\n");

	init_data.default_label = "yellow:flash";
	ret = devm_led_classdev_flash_register_ext(dev, &priv->leds[1].fled,
						   &init_data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register LED2\n");

	return 0;
}

static int lm3644_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct lm3644 *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	mutex_init(&priv->lock);

	priv->regmap = devm_regmap_init_i2c(client, &lm3644_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "failed to init register map\n");

	if (of_device_is_compatible(dev->of_node, "ti,lm3643")) {
		priv->timeout_reg = LM3643_REG_TIMEOUT;
		priv->timeout_step_us = LM3643_TIMEOUT_LM3643_US;
		priv->timeout_max = LM3643_TIMEOUT_LM3643_MAX;
	} else {
		priv->timeout_reg = LM3644_REG_TIMEOUT;
		priv->timeout_step_us = LM3644_TIMEOUT_LM3644_US;
		priv->timeout_max = LM3644_TIMEOUT_LM3644_MAX;
	}

	priv->hw_en = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->hw_en))
		return dev_err_probe(dev, PTR_ERR(priv->hw_en),
				     "failed to get EN GPIO\n");

	/* The part answers I2C only once its EN pin has settled */
	usleep_range(1000, 2000);

	/* Program the safety timeout and start with both outputs off */
	ret = regmap_write(priv->regmap, priv->timeout_reg, priv->timeout_max);
	if (ret)
		return dev_err_probe(dev, ret, "failed to program timeout\n");

	priv->enable = priv->hw_en ? LM3644_EN_HW : 0;
	ret = regmap_write(priv->regmap, LM3644_REG_ENABLE, priv->enable);
	if (ret)
		return dev_err_probe(dev, ret, "failed to reset the outputs\n");

	ret = lm3644_register_leds(priv);
	if (ret)
		return ret;

	i2c_set_clientdata(client, priv);

	dev_info(dev, "LM3644 dual flash/torch controller at 0x%02x\n",
		 client->addr);

	return 0;
}

static void lm3644_remove(struct i2c_client *client)
{
	struct lm3644 *priv = i2c_get_clientdata(client);

	if (!priv)
		return;

	guard(mutex)(&priv->lock);
	regmap_write(priv->regmap, LM3644_REG_ENABLE, 0);
	gpiod_set_value_cansleep(priv->hw_en, 0);
}

static const struct of_device_id lm3644_of_match[] = {
	{ .compatible = "ti,lm3644" },
	{ .compatible = "ti,lm3643" },
	{ .compatible = "mediatek,strobe_main" },
	{ }
};
MODULE_DEVICE_TABLE(of, lm3644_of_match);

static const struct i2c_device_id lm3644_id_table[] = {
	{ "lm3644" },
	{ "lm3643" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, lm3644_id_table);

static struct i2c_driver lm3644_driver = {
	.driver = {
		.name = LM3644_NAME,
		.of_match_table = lm3644_of_match,
	},
	.probe = lm3644_probe,
	.remove = lm3644_remove,
	.id_table = lm3644_id_table,
};

module_i2c_driver(lm3644_driver);

MODULE_DESCRIPTION("TI LM3644/LM3643 dual flash LED controller driver");
MODULE_AUTHOR("Meizu fleet kernel port");
MODULE_LICENSE("GPL");
