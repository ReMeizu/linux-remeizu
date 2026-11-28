// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6755/MT6750 audio subsystem clock controller
 *
 * The audiosys block at 0x11220000 hosts both the audio front-end
 * (0x1000 register window, handled by the ASoC AFE driver) and the audio
 * clock gates. Gate registers have no set/clear alias (each bit is a simple
 * on/off latch), and the bits are shared with the AFE hardware, so the
 * driver uses the non-atomic read-modify-write gate ops.
 *
 * Gate offsets and bit positions follow the vendor clk-mt6755 driver
 * (drivers/clk/mediatek/clk-mt6755.c, audio_clks[]) and the published
 * MT6755 register map:
 *   AUDIO_TOP_CON0 (0x0000):
 *     2 AUD_AFE, 3 AUD_22M, 5 AUD_APLL2_TUNER, 6 AUD_APLL_TUNER,
 *     7 AUD_ADC, 8 AUD_DAC, 9 AUD_DAC_PREDIS, 10 AUD_TML,
 *     11 AUD_APLL1_DIV0, 12 AUD_APLL2_DIV0
 *   AUDIO_TOP_CON1 (0x0004):
 *     0 AUD_I2S, 2 AUD_24M
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/mediatek,mt6755-clk.h>

#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs audio0_cg_regs = {
	.set_ofs = 0x0000,
	.clr_ofs = 0x0000,
	.sta_ofs = 0x0000,
};

static const struct mtk_gate_regs audio1_cg_regs = {
	.set_ofs = 0x0004,
	.clr_ofs = 0x0004,
	.sta_ofs = 0x0004,
};

#define GATE_AUDIO0(_id, _name, _parent, _shift)		\
	GATE_MTK(_id, _name, _parent, &audio0_cg_regs, _shift,	\
		 &mtk_clk_gate_ops_no_setclr)

#define GATE_AUDIO1(_id, _name, _parent, _shift)		\
	GATE_MTK(_id, _name, _parent, &audio1_cg_regs, _shift,	\
		 &mtk_clk_gate_ops_no_setclr)

static const struct mtk_gate audio_clks[] = {
	/* AUDIO_TOP_CON0 */
	GATE_AUDIO0(CLK_AUDIO_AFE, "aud_afe", "audio_sel", 2),
	GATE_AUDIO0(CLK_AUDIO_22M, "aud_apll22m", "audio_sel", 3),
	GATE_AUDIO0(CLK_AUDIO_APLL2_TUNER, "aud_apll2_tuner",
		    "audio_sel", 5),
	GATE_AUDIO0(CLK_AUDIO_APLL_TUNER, "aud_apll1_tuner",
		    "audio_sel", 6),
	GATE_AUDIO0(CLK_AUDIO_ADC, "aud_adc", "aud_intbus_sel", 7),
	GATE_AUDIO0(CLK_AUDIO_DAC, "aud_dac", "aud_intbus_sel", 8),
	GATE_AUDIO0(CLK_AUDIO_DAC_PREDIS, "aud_dac_predis",
		    "aud_intbus_sel", 9),
	GATE_AUDIO0(CLK_AUDIO_TML, "aud_tml", "aud_intbus_sel", 10),
	GATE_AUDIO0(CLK_AUDIO_APLL1_DIV0, "aud_apll1_div0", "audio_sel", 11),
	GATE_AUDIO0(CLK_AUDIO_APLL2_DIV0, "aud_apll2_div0", "audio_sel", 12),
	/* AUDIO_TOP_CON1 */
	GATE_AUDIO1(CLK_AUDIO_I2S, "aud_i2s", "aud_intbus_sel", 0),
	GATE_AUDIO1(CLK_AUDIO_24M, "aud_apll24m", "audio_sel", 2),
};

static const struct mtk_clk_desc audio_desc = {
	.clks = audio_clks,
	.num_clks = ARRAY_SIZE(audio_clks),
};

static const struct of_device_id of_match_clk_mt6755_audio[] = {
	{ .compatible = "mediatek,mt6755-audsys", .data = &audio_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6755_audio);

static struct platform_driver clk_mt6755_audio_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6755-audio",
		.of_match_table = of_match_clk_mt6755_audio,
	},
};
module_platform_driver(clk_mt6755_audio_drv);

MODULE_DESCRIPTION("MediaTek MT6755/MT6750 audio subsystem clock driver");
MODULE_LICENSE("GPL");
