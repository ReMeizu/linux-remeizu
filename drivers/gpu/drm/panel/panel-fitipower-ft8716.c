// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fitipower FT8716 based 1080x1920 DSI panel (Meizu MX6 "m95").
 *
 * Panel parameters and the initialisation sequence are transcribed from the
 * vendor driver of the same panel:
 *   meizu_mx6_m95 kernel/m685
 *     drivers/misc/mediatek/lcm/ft8716_fhd_vdo_sharp/ft8716_fhd_vdo_sharp.c
 *
 *   resolution   1080x1920, 4 lanes, packed RGB888, burst video mode
 *                (lcm_get_params(): LANE_NUM = LCM_FOUR_LANE,
 *                 PS = LCM_PACKED_PS_24BIT_RGB888, mode = BURST_VDO_MODE)
 *   porches      VSA 2 / VBP 16 / VFP 16, HSA 4 / HBP 32 / HFP 32
 *                (lcm_get_params()); vertical_frontporch_for_low_power 600
 *                has no mainline equivalent and is not modelled.
 *   DSI PLL      430 MHz (lcm_get_params(), LCM_DSI_CMD_MODE == 0 branch).
 *                That is the byte clock, i.e. 860 Mbit/s per lane, which for
 *                4 lanes at 24 bpp is a 143.333 MHz pixel clock:
 *                430 MHz * 2 * 4 / 24. The resulting vrefresh is ~63.9 Hz,
 *                which is what the stock kernel programs; the vendor value is
 *                a rounded-up entry of its own PLL table.
 *   reset        GPIO180 (vendor mtkfb_pins_lcm_reset uses
 *                PINMUX_GPIO180__FUNC_LCM_RST), active low, pulsed twice
 *                (lcm_power_switch()).
 *
 * The vendor init table (lcm_slpout/lcm_dispon/lcm_dispoff) is 16 entries; the
 * large register dump in the same file is inside "#if 0" and is not part of
 * the sequence.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <video/mipi_display.h>

struct ft8716 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[3];
	bool prepared;
};

static inline struct ft8716 *panel_to_ft8716(struct drm_panel *panel)
{
	return container_of(panel, struct ft8716, panel);
}

struct ft8716_cmd {
	u8 cmd;
	u8 len;
	u8 data[4];
};

/* lcm_slpout[] + lcm_dispon[] of the vendor driver, in order. */
static const struct ft8716_cmd ft8716_init_seq[] = {
	{ 0x00, 1, { 0x00 } },			/* page select */
	{ 0x99, 2, { 0x95, 0x27 } },
	{ 0x35, 0, {} },			/* TE on (ESD_CHECK) */
	{ 0x11, 0, {} },			/* sleep out */
	{ 0x29, 0, {} },			/* display on */
	{ 0x00, 1, { 0x00 } },
	{ 0x99, 2, { 0x00, 0x00 } },
};

static int ft8716_write(struct ft8716 *ctx, const struct ft8716_cmd *c)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	ssize_t ret;

	if (c->len)
		ret = mipi_dsi_dcs_write_buffer(dsi, &c->cmd, c->len + 1);
	else
		ret = mipi_dsi_dcs_write(dsi, c->cmd, NULL, 0);

	if (ret < 0) {
		dev_err(&dsi->dev, "failed to write command %#x: %zd\n",
			c->cmd, ret);
		return ret;
	}

	return 0;
}

static int ft8716_power_on(struct ft8716 *ctx)
{
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		return ret;

	/* lcm_power_switch(1): two reset pulses around the bias rails. */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);

	return 0;
}

static void ft8716_power_off(struct ft8716 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
}

static int ft8716_prepare(struct drm_panel *panel)
{
	struct ft8716 *ctx = panel_to_ft8716(panel);
	int ret;

	if (ctx->prepared)
		return 0;

	ret = ft8716_power_on(ctx);
	if (ret < 0)
		return ret;

	ctx->prepared = true;

	return 0;
}

static int ft8716_enable(struct drm_panel *panel)
{
	struct ft8716 *ctx = panel_to_ft8716(panel);
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(ft8716_init_seq); i++) {
		ret = ft8716_write(ctx, &ft8716_init_seq[i]);
		if (ret < 0)
			return ret;

		/* REGFLAG_DELAY 120 after sleep out. */
		if (ft8716_init_seq[i].cmd == MIPI_DCS_EXIT_SLEEP_MODE)
			msleep(120);
	}

	return 0;
}

static int ft8716_disable(struct drm_panel *panel)
{
	struct ft8716 *ctx = panel_to_ft8716(panel);
	int ret;

	/* lcm_dispoff[] */
	ret = mipi_dsi_dcs_set_display_off(ctx->dsi);
	if (ret < 0)
		return ret;

	msleep(20);

	ret = mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
	if (ret < 0)
		return ret;

	msleep(120);

	return 0;
}

static int ft8716_unprepare(struct drm_panel *panel)
{
	struct ft8716 *ctx = panel_to_ft8716(panel);

	if (!ctx->prepared)
		return 0;

	ft8716_power_off(ctx);
	ctx->prepared = false;

	return 0;
}

static const struct drm_display_mode ft8716_mode = {
	.clock = 143333,
	.hdisplay = 1080,
	.hsync_start = 1080 + 32,
	.hsync_end = 1080 + 32 + 4,
	.htotal = 1080 + 32 + 4 + 32,
	.vdisplay = 1920,
	.vsync_start = 1920 + 16,
	.vsync_end = 1920 + 16 + 2,
	.vtotal = 1920 + 16 + 2 + 16,
};

static int ft8716_get_modes(struct drm_panel *panel,
			    struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &ft8716_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs ft8716_panel_funcs = {
	.prepare = ft8716_prepare,
	.enable = ft8716_enable,
	.disable = ft8716_disable,
	.unprepare = ft8716_unprepare,
	.get_modes = ft8716_get_modes,
};

static int ft8716_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ft8716 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->supplies[0].supply = "vddi";
	ctx->supplies[1].supply = "avdd";
	ctx->supplies[2].supply = "avee";

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset gpio\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	drm_panel_init(&ctx->panel, dev, &ft8716_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void ft8716_remove(struct mipi_dsi_device *dsi)
{
	struct ft8716 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ft8716_of_match[] = {
	{ .compatible = "fitipower,ft8716" },
	{ }
};
MODULE_DEVICE_TABLE(of, ft8716_of_match);

static struct mipi_dsi_driver ft8716_driver = {
	.probe = ft8716_probe,
	.remove = ft8716_remove,
	.driver = {
		.name = "panel-fitipower-ft8716",
		.of_match_table = ft8716_of_match,
	},
};
module_mipi_dsi_driver(ft8716_driver);

MODULE_AUTHOR("ReMeizu");
MODULE_DESCRIPTION("Fitipower FT8716 1080x1920 DSI panel");
MODULE_LICENSE("GPL");
