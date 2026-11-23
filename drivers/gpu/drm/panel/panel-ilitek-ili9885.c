// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ilitek ILI9885 1080x1920 DSI video-mode panel, TXD module ("txd1"),
 * as used in the Meizu M3 Note (m681).
 *
 * Everything marked VERIFIED-FROM-STOCK below is byte/number-exact from:
 *  [LCM-3.10] meizuosc-m681-stockdisplay-20260429/drivers/misc/mediatek/lcm/
 *             ili9885_fhd_dsi_vdo_txd1/ili9885_fhd_dsi_vdo_txd1.c
 *             (the 3.10.72 stock tree with VERIFIED working display)
 *  [LCM-3.18] kernel-m681-m6base-3.18.140/kernel-3.18/drivers/misc/mediatek/
 *             lcm/ili9885_fhd_dsi_vdo_txd1/ili9885_fhd_dsi_vdo_txd1.c
 *  [LK]       /srv/forge/android/m681/flyme7/lk.bin — init table found at
 *             0x55d30+ ({cmd u32}{count u8}{para[128]} records); confirms
 *             the 0xD6 payload starts 0x80 (LK and 3.10 agree; the 3.18
 *             tree's 0x00 first byte is the outlier).
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct ili9885 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;	/* GPIO158, dedicated LCM_RST pad */
	struct regulator *vsp;		/* TPS65132 OUTP, en = GPIO12 */
	struct regulator *vsn;		/* TPS65132 OUTN, en = GPIO7  */
	enum drm_panel_orientation orientation;	/* m681: mounted 180° */
};

static inline struct ili9885 *to_ili9885(struct drm_panel *panel)
{
	return container_of(panel, struct ili9885, panel);
}

/*
 * VERIFIED-FROM-STOCK init sequence — [LCM-3.10] init_setting[] and [LK]
 * 0x55d30.  Commands are sent exactly as the MTK LCM driver did
 * (dsi_set_cmdq_V2 => DCS short/long write with the listed parameters).
 * Note the stock table sends 0x11/0x29 as one-parameter (0x00) writes; we
 * replicate that byte-exactly rather than using the 0-parameter helpers.
 */
static void ili9885_init_sequence(struct mipi_dsi_multi_context *ctx)
{
	/*
	 * r77 PACKET-TYPE FIX: the stock transport (ddp_dsi.c DSI_set_cmdq_V2)
	 * sends every command >= 0xB0 as a GENERIC packet (long 0x29 / short
	 * 0x23) and only commands < 0xB0 as DCS. Our earlier all-DCS encoding
	 * meant the DDIC silently IGNORED every manufacturer register write
	 * below: on an LK-warm panel that was invisible (already configured),
	 * but a cold kernel-owned power-on ended up "display on, never
	 * configured" = colored/gray vertical bars.
	 */
	mipi_dsi_generic_write_seq_multi(ctx, 0xb0, 0x98, 0x85, 0x0a);
	/*
	 * D6[0] = 0x80 — VERIFIED-FROM-LK-BINARY (r84 audit): the flyme7
	 * lk.bin init table at 0x55d3c carries D6 payload 80 00 08 17 23 65
	 * 77 44 87 00 00 09; the 0x80-signature appears 6x in the binary
	 * (one per LCM variant) and the 3.18 tree's 0x00 variant appears
	 * NOWHERE in shipped firmware. LK == [LCM-3.10] == this table. The
	 * old r82 theory that a kernel-owned re-init needs 3.18's 0x00 is
	 * REJECTED.
	 */
	mipi_dsi_generic_write_seq_multi(ctx, 0xd6, 0x80, 0x00, 0x08, 0x17, 0x23,
					 0x65, 0x77, 0x44, 0x87, 0x00, 0x00, 0x09);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x35, 0x00);	/* TE on (DCS, <0xB0) */

	mipi_dsi_generic_write_seq_multi(ctx, 0xf5, 0x85);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb9, 0x02);

	mipi_dsi_generic_write_seq_multi(ctx, 0xf5, 0x8d);
	mipi_dsi_generic_write_seq_multi(ctx, 0xef, 0x02);

	mipi_dsi_generic_write_seq_multi(ctx, 0xf5, 0x9b);
	mipi_dsi_generic_write_seq_multi(ctx, 0xd7, 0xa0);

	mipi_dsi_generic_write_seq_multi(ctx, 0xf5, 0x9c);
	mipi_dsi_generic_write_seq_multi(ctx, 0xd7, 0x40);

	mipi_dsi_generic_write_seq_multi(ctx, 0xf5, 0x8b);
	mipi_dsi_generic_write_seq_multi(ctx, 0xd0, 0x0e);
	mipi_dsi_generic_write_seq_multi(ctx, 0xf5, 0x00);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x11, 0x00);	/* sleep out */
	mipi_dsi_msleep(ctx, 120);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x29, 0x00);	/* display on */
}

static int ili9885_prepare(struct drm_panel *panel)
{
	struct ili9885 *ctx = to_ili9885(panel);
	struct mipi_dsi_multi_context mctx = { .dsi = ctx->dsi };
	int ret;

	/*
	 * Full stock power-on ([LCM-3.10] lcm_init_power + lcm_init), kernel-
	 * owned from a cold panel: reset low, bias up (ENP 3ms, then ENN
	 * 10ms — stock MDELAYs), stock reset waveform, byte-exact init table.
	 * mtk_dsi_poweron() forces command mode before this runs so the init
	 * DCS goes out cleanly. vsp/vsn are the real TPS65132 outputs since
	 * r84: enabling them programs VPOS/VNEG to 5.5 V (what stock's
	 * tps65132_write_bytes(0x00/0x01, 0x0F) did) and raises the EN
	 * GPIOs, so a cold prepare gets a true bias ramp like stock resume.
	 */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(10);

	ret = regulator_enable(ctx->vsp);
	if (ret < 0)
		return ret;
	msleep(3);
	ret = regulator_enable(ctx->vsn);
	if (ret < 0) {
		regulator_disable(ctx->vsp);
		return ret;
	}
	msleep(10);

	/*
	 * VERIFIED-FROM-STOCK reset waveform ([LCM-3.10]/[LCM-3.18] lcm_init):
	 * 1 (5ms) -> 0 (10ms) -> 1 (120ms), physical levels.
	 */
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(5);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(10);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(120);

	ili9885_init_sequence(&mctx);
	if (mctx.accum_err) {
		regulator_disable(ctx->vsn);
		regulator_disable(ctx->vsp);
		return mctx.accum_err;
	}

	/*
	 * NO DCS reads here — BTA wedges this DSI IP hard regardless of the
	 * clock-lane state (r75/r76 with continuous HS, r81 with the stock LP
	 * command phase: the first read stalls the bus for minutes with hung-
	 * task traces in mtk_dsi_wait_for_irq_done). The RX path needs the
	 * vendor BTA/RACK sequence we don't replicate; tracked in the journal.
	 */

	return 0;
}

static int ili9885_unprepare(struct drm_panel *panel)
{
	struct ili9885 *ctx = to_ili9885(panel);
	struct mipi_dsi_multi_context mctx = { .dsi = ctx->dsi };

	/*
	 * VERIFIED-FROM-STOCK sleep-in ([LCM-3.10] lcm_sleep_in_setting +
	 * lcm_suspend): 0x28, 20ms, 0x10, 120ms; then reset low 5ms,
	 * ENN off 2ms, ENP off 1ms. Since r84 vsp/vsn are the real TPS65132
	 * outputs (not always-on), so this genuinely cuts AVDD/AVEE like
	 * stock suspend does — the DDIC is fully unpowered between cycles
	 * and the next prepare re-inits it from a clean cold state.
	 */
	mipi_dsi_dcs_write_seq_multi(&mctx, 0x28, 0x00);
	mipi_dsi_msleep(&mctx, 20);
	mipi_dsi_dcs_write_seq_multi(&mctx, 0x10, 0x00);
	mipi_dsi_msleep(&mctx, 120);

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(5);
	regulator_disable(ctx->vsn);
	msleep(2);
	regulator_disable(ctx->vsp);
	msleep(1);

	return mctx.accum_err;
}

/*
 * VERIFIED-FROM-STOCK timing ([LCM-3.10] lcm_get_params, lines 207-215):
 *   HSA 8, HBP 70, HFP 70; VSA 8, VBP 16, VFP 24; 1080x1920.
 *   htotal 1228, vtotal 1968 -> 145.0 MHz @60 Hz.
 *   physical 68 x 121 mm (lines 190-191).
 * ([LCM-3.18] flight-proven deltas, if RDMA underruns appear on mainline:
 *   HBP/HFP 80/80 and VFP 150 — see the long v251 comment in that file.)
 *
 * Stock link rate: PLL_CLOCK = 480 MHz -> 960 Mbps/lane, BURST_VDO_MODE,
 * 4 lanes ([LCM-3.10] lines 192-256).  mtk_dsi computes the PHY rate as
 * pclk*bpp/lanes = 870 Mbps instead; HYPOTHESIS: the panel tolerates this
 * (burst-mode panels accept a link-rate range).  If not, inflate hporches
 * to raise the computed rate towards 960.
 */
static const struct drm_display_mode ili9885_mode = {
	.clock = (1080 + 70 + 8 + 70) * (1920 + 24 + 8 + 16) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 70,		/* + HFP */
	.hsync_end = 1080 + 70 + 8,		/* + HSA */
	.htotal = 1080 + 70 + 8 + 70,		/* + HBP */
	.vdisplay = 1920,
	.vsync_start = 1920 + 24,		/* + VFP */
	.vsync_end = 1920 + 24 + 8,		/* + VSA */
	.vtotal = 1920 + 24 + 8 + 16,		/* + VBP */
	.width_mm = 68,
	.height_mm = 121,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int ili9885_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct ili9885 *ctx = to_ili9885(panel);

	/*
	 * Property set through .get_orientation; still needs to be attached to
	 * the connector for fbdev/compositors that read it from there.
	 */
	drm_connector_set_panel_orientation(connector, ctx->orientation);

	return drm_connector_helper_get_modes_fixed(connector, &ili9885_mode);
}

static enum drm_panel_orientation ili9885_get_orientation(struct drm_panel *panel)
{
	struct ili9885 *ctx = to_ili9885(panel);

	return ctx->orientation;
}

static const struct drm_panel_funcs ili9885_funcs = {
	.prepare = ili9885_prepare,
	.unprepare = ili9885_unprepare,
	.get_modes = ili9885_get_modes,
	.get_orientation = ili9885_get_orientation,
};

static int ili9885_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ili9885 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct ili9885, panel, &ili9885_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	/*
	 * r74: reset is REAL again. The old "never drive LCM_RST" rule existed
	 * only because the DSI command path was broken (cmd-mode switch -62) and
	 * a reset pulse was unrecoverable. With the DSI host fixed (force cmd
	 * mode on poweron, polled DSI_INTSTA waits, true PLL rates) prepare()
	 * runs the full stock power-on — reset waveform + init table — so the
	 * panel is kernel-owned from a cold state and survives unprepare/prepare
	 * cycles (blank/wake, compositor restarts).
	 * GPIO_ACTIVE_HIGH on purpose — the waveform uses physical levels.
	 */
	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "cannot get reset GPIO\n");

	ctx->vsp = devm_regulator_get(dev, "vsp");
	if (IS_ERR(ctx->vsp))
		return dev_err_probe(dev, PTR_ERR(ctx->vsp), "cannot get vsp\n");
	ctx->vsn = devm_regulator_get(dev, "vsn");
	if (IS_ERR(ctx->vsn))
		return dev_err_probe(dev, PTR_ERR(ctx->vsn), "cannot get vsn\n");

	dsi->lanes = 4;				/* [LCM-3.10] LCM_FOUR_LANE */
	dsi->format = MIPI_DSI_FMT_RGB888;	/* RGB888 */
	/*
	 * BURST_VDO_MODE ([LCM-3.10] line 192) + continuous HS clock.
	 * r84 audit correction: cont_clock=1 is a [LCM-3.18] property
	 * (its :593); [LCM-3.10] leaves it 0 (non-continuous). We run
	 * continuous — i.e. the 3.18 flavour — because the r83 silicon
	 * match (HSTX_CKLP_EN + stock blanking WCs) was captured against
	 * the live TWRP/stock state and works with it. LPM for the init
	 * commands (LK sends init in LP mode).
	 */
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	/* m681: panel is mounted upside down — rotation = <180> in DT */
	ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get orientation\n");

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "mipi_dsi_attach failed\n");
	}

	return 0;
}

static void ili9885_remove(struct mipi_dsi_device *dsi)
{
	struct ili9885 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ili9885_of_match[] = {
	{ .compatible = "txd,ili9885" },
	{ .compatible = "ilitek,ili9885" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ili9885_of_match);

static struct mipi_dsi_driver ili9885_driver = {
	.probe = ili9885_probe,
	.remove = ili9885_remove,
	.driver = {
		.name = "panel-ilitek-ili9885",
		.of_match_table = ili9885_of_match,
	},
};
module_mipi_dsi_driver(ili9885_driver);

MODULE_DESCRIPTION("Ilitek ILI9885 FHD DSI panel driver (Meizu m681 TXD module)");
MODULE_LICENSE("GPL");
