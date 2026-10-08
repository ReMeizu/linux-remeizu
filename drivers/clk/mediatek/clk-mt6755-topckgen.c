// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Copyright (c) 2023 Arseniy Velikanov <adomerlee@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-mux.h"

#include <dt-bindings/clock/mediatek,mt6755-clk.h>


#define CLK_CFG_0			0x0040
#define CLK_CFG_0_SET		0x0044
#define CLK_CFG_0_CLR		0x0048

#define CLK_CFG_1			0x0050
#define CLK_CFG_1_SET		0x0054
#define CLK_CFG_1_CLR		0x0058

#define CLK_CFG_2			0x0060
#define CLK_CFG_2_SET		0x0064
#define CLK_CFG_2_CLR		0x0068

#define CLK_CFG_3			0x0070
#define CLK_CFG_3_SET		0x0074
#define CLK_CFG_3_CLR		0x0078

#define CLK_CFG_4			0x0080
#define CLK_CFG_4_SET		0x0084
#define CLK_CFG_4_CLR		0x0088

#define CLK_CFG_5			0x0090
#define CLK_CFG_5_SET		0x0094
#define CLK_CFG_5_CLR		0x0098

#define CLK_CFG_6			0x00a0
#define CLK_CFG_6_SET		0x00a4
#define CLK_CFG_6_CLR		0x00a8

#define CLK_CFG_7			0x00b0
#define CLK_CFG_7_SET		0x00b4
#define CLK_CFG_7_CLR		0x00b8

#define CLK_CFG_8			0x00c0
#define CLK_CFG_8_SET		0x00c4
#define CLK_CFG_8_CLR		0x00c8

static DEFINE_SPINLOCK(mt6755_clk_lock);

static const struct mtk_fixed_factor topckgen_factors[] = {
	FACTOR(CLK_TOP_AD_APLL1_CK, "apll1_ck", "apll1", 1, 1),
	FACTOR(CLK_TOP_AD_APLL2_CK, "apll2_ck", "apll2", 1, 1),
	FACTOR(CLK_TOP_MMPLL_CK, "mmpll_ck", "mmpll", 1, 1),
	FACTOR(CLK_TOP_MSDCPLL_CK, "msdcpll_ck", "msdcpll", 1, 1),
	FACTOR(CLK_TOP_MSDCPLL_D16, "msdcpll_d16", "msdcpll", 1, 16),
	FACTOR(CLK_TOP_MSDCPLL_D2, "msdcpll_d2", "msdcpll", 1, 2),
	FACTOR(CLK_TOP_MSDCPLL_D4, "msdcpll_d4", "msdcpll", 1, 4),
	FACTOR(CLK_TOP_MSDCPLL_D8, "msdcpll_d8", "msdcpll", 1, 8),
	FACTOR(CLK_TOP_SYSPLL_CK, "syspll_ck", "mainpll", 1, 1),
	FACTOR(CLK_TOP_SYSPLL_D3, "syspll_d3", "mainpll", 1, 3),
	FACTOR(CLK_TOP_SYSPLL_D3_D3, "syspll_d3_d3", "mainpll", 1, 9),
	FACTOR(CLK_TOP_SYSPLL_D5, "syspll_d5", "mainpll", 1, 5),
	FACTOR(CLK_TOP_SYSPLL_D7, "syspll_d7", "mainpll", 1, 7),
	FACTOR(CLK_TOP_SYSPLL1_CK, "syspll1_ck", "mainpll", 1, 2),
	FACTOR(CLK_TOP_SYSPLL1_D16, "syspll1_d16", "syspll1_ck", 1, 16),
	FACTOR(CLK_TOP_SYSPLL1_D2, "syspll1_d2", "syspll1_ck", 1, 2),
	FACTOR(CLK_TOP_SYSPLL1_D4, "syspll1_d4", "syspll1_ck", 1, 4),
	FACTOR(CLK_TOP_SYSPLL1_D8, "syspll1_d8", "syspll1_ck", 1, 8),
	FACTOR(CLK_TOP_SYSPLL2_CK, "syspll2_ck", "mainpll", 1, 3),
	FACTOR(CLK_TOP_SYSPLL2_D2, "syspll2_d2", "syspll2_ck", 1, 2),
	FACTOR(CLK_TOP_SYSPLL2_D4, "syspll2_d4", "syspll2_ck", 1, 4),
	FACTOR(CLK_TOP_SYSPLL3_CK, "syspll3_ck", "mainpll", 1, 5),
	FACTOR(CLK_TOP_SYSPLL3_D2, "syspll3_d2", "syspll3_ck", 1, 2),
	FACTOR(CLK_TOP_SYSPLL3_D4, "syspll3_d4", "syspll3_ck", 1, 4),
	FACTOR(CLK_TOP_SYSPLL4_CK, "syspll4_ck", "mainpll", 1, 7),
	FACTOR(CLK_TOP_SYSPLL4_D2, "syspll4_d2", "syspll4_ck", 1, 2),
	FACTOR(CLK_TOP_SYSPLL4_D4, "syspll4_d4", "syspll4_ck", 1, 4),
	FACTOR(CLK_TOP_TVDPLL_CK, "tvdpll_ck", "tvdpll", 1, 1),
	FACTOR(CLK_TOP_TVDPLL_D2, "tvdpll_d2", "tvdpll", 1, 2),
	FACTOR(CLK_TOP_TVDPLL_D4, "tvdpll_d4", "tvdpll", 1, 4),
	FACTOR(CLK_TOP_TVDPLL_D8, "tvdpll_d8", "tvdpll", 1, 8),
	FACTOR(CLK_TOP_TVDPLL_D16, "tvdpll_d16", "tvdpll", 1, 16),
	FACTOR(CLK_TOP_UNIVPLL_CK, "univpll_ck", "univpll", 1, 1),
	FACTOR(CLK_TOP_UNIVPLL_D26, "univpll_d26", "univpll", 1, 26),
	FACTOR(CLK_TOP_UNIVPLL_D7, "univpll_d7", "univpll", 1, 7),
	FACTOR(CLK_TOP_UNIVPLL_D5, "univpll_d5", "univpll", 1, 5),
	FACTOR(CLK_TOP_UNIVPLL_D3, "univpll_d3", "univpll", 1, 3),
	FACTOR(CLK_TOP_UNIVPLL_D2, "univpll_d2", "univpll", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL1_CK, "univpll1_ck", "univpll", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL1_D2, "univpll1_d2", "univpll1_ck", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL1_D4, "univpll1_d4", "univpll1_ck", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL1_D8, "univpll1_d8", "univpll1_ck", 1, 8),
	FACTOR(CLK_TOP_UNIVPLL2_CK, "univpll2_ck", "univpll", 1, 3),
	FACTOR(CLK_TOP_UNIVPLL2_D2, "univpll2_d2", "univpll2_ck", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL2_D4, "univpll2_d4", "univpll2_ck", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL2_D8, "univpll2_d8", "univpll2_ck", 1, 8),
	FACTOR(CLK_TOP_UNIVPLL3_CK, "univpll3_ck", "univpll", 1, 5),
	FACTOR(CLK_TOP_UNIVPLL3_D2, "univpll3_d2", "univpll3_ck", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL3_D4, "univpll3_d4", "univpll3_ck", 1, 4),
	FACTOR(CLK_TOP_VENCPLL_CK, "vencpll_ck", "vencpll", 1, 1),
};

static const char * const axi_parents[] = {
	"clk26m",
	"syspll1_d4",
	"syspll2_d2",
	"osc_d8",
};

static const char * const mem_parents[] = {
	"dmpll_ck",
};

static const char * const ddrphycfg_parents[] = {
	"clk26m",
	"syspll1_d8",
};

static const char * const mm_parents[] = {
	"clk26m",
	"syspll_d3",
	"vencpll_ck",
	"syspll1_d2",
	"syspll2_d2",
	"syspll_d7",
	"syspll2_d4",
	"syspll4_d2",
};

static const char * const pwm_parents[] = {
	"clk26m",
	"univpll2_d2",
	"univpll2_d4",
};

static const char * const vdec_parents[] = {
	"clk26m",
	"univpll1_d2",
	"vencpll_ck",
	"univpll_d5",
	"syspll1_d4",
};

static const char * const mfg_parents[] = {
	"clk26m",
	"mmpll_ck",
	"univpll_d3",
	"syspll_d3",
};

static const char * const camtg_parents[] = {
	"clk26m",
	"univpll_d26",
	"univpll2_d2",
};

static const char * const uart_parents[] = {
	"clk26m",
	"univpll2_d8",
};

static const char * const spi_parents[] = {
	"clk26m",
	"syspll3_d2",
	"syspll2_d4",
	"msdcpll_d4",
};

static const char * const msdc50_0_hclk_parents[] = {
	"clk26m",
	"syspll1_d2",
	"syspll2_d2",
	"syspll4_d2",
};

static const char * const msdc50_0_parents[] = {
	"clk26m",
	"msdcpll_ck",
	"msdcpll_d2",
	"univpll1_d4",
	"syspll2_d2",
	"syspll_d7",
	"msdcpll_d4",
	"univpll_d2",
	"univpll1_d2",
};

static const char * const msdc30_1_parents[] = {
	"clk26m",
	"univpll2_d2",
	"msdcpll_d4",
	"univpll1_d4",
	"syspll2_d2",
	"syspll_d7",
	"univpll_d7",
	"msdcpll_d2",
};

static const char * const msdc30_2_parents[] = {
	"clk26m",
	"univpll2_d2",
	"msdcpll_d4",
	"univpll1_d4",
	"syspll2_d2",
	"syspll_d7",
	"univpll_d7",
	"msdcpll_d2",
};

static const char * const msdc30_3_parents[] = {
	"clk26m",
	"msdcpll_d8",
	"msdcpll_d4",
	"univpll1_d4",
	"univpll_d26",
	"syspll_d7",
	"univpll_d7",
	"syspll3_d4",
	"msdcpll_d16",
};

static const char * const audio_parents[] = {
	"clk26m",
	"syspll3_d4",
	"syspll4_d4",
	"syspll1_d16",
};

static const char * const aud_intbus_parents[] = {
	"clk26m",
	"syspll1_d4",
	"syspll4_d2",
};

static const char * const pmicspi_parents[] = {
	"clk26m",
	"syspll1_d8",
	"osc_d4",
};

static const char * const atb_parents[] = {
	"clk26m",
	"syspll1_d2",
	"syspll_d5",
};

static const char * const dpi0_parents[] = {
	"clk26m",
	"tvdpll_d2",
	"tvdpll_d4",
	"tvdpll_d8",
	"tvdpll_d16",
};

static const char * const scam_parents[] = {
	"clk26m",
	"syspll3_d2",
};

static const char * const aud_1_parents[] = {
	"clk26m",
	"apll1_ck",
};

static const char * const aud_2_parents[] = {
	"clk26m",
	"apll2_ck",
};

static const char * const disppwm_parents[] = {
	"clk26m",
	"univpll2_d4",
	"osc_d2",
	"osc_d8",
};

static const char * const ssusb_top_sys_parents[] = {
	"clk26m",
	"univpll3_d2",
};

static const char * const usb_top_parents[] = {
	"clk26m",
	"univpll3_d4",
};

static const char * const spm_parents[] = {
	"clk26m",
	"syspll1_d8",
};

static const char * const bsi_spi_parents[] = {
	"clk26m",
	"syspll_d3_d3",
	"syspll1_d4",
	"syspll_d7",
};

static const char * const i2c_parents[] = {
	"clk26m",
	"syspll1_d8",
	"univpll3_d4",
};

static const char * const dvfsp_parents[] = {
	"clk26m",
	"syspll1_d8",
};


static const struct mtk_mux topckgen_muxes[] = {
	MUX_CLR_SET_UPD(CLK_TOP_MUX_AXI, "axi_sel", axi_parents, 0x40, 0x44, 0x48,
			0, 2, 0x0004, 0),
	MUX_CLR_SET_UPD(CLK_TOP_MUX_MEM, "mem_sel", mem_parents, 0x40, 0x44, 0x48,
			8, 2, 0x0004, 1),
	MUX_CLR_SET_UPD(CLK_TOP_MUX_DDRPHY, "ddrphycfg_sel", ddrphycfg_parents, 0x40, 0x44, 0x48,
			16, 1, 0x0004, 2),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_MM, "mm_sel", mm_parents, 0x40, 0x44, 0x48,
			     24, 3, 31, 0x0004, 3),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_PWM, "pwm_sel", pwm_parents, 0x50, 0x54, 0x58,
			     0, 2, 7, 0x0004, 4),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_VDEC, "vdec_sel", vdec_parents, 0x50, 0x54, 0x58,
			     8, 3, 15, 0x0004, 5),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_MFG, "mfg_sel", mfg_parents, 0x50, 0x54, 0x58,
			     24, 2, 31, 0x0004, 7),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_CAMTG, "camtg_sel", camtg_parents, 0x60, 0x64, 0x68,
			     0, 2, 7, 0x0004, 8),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_UART, "uart_sel", uart_parents, 0x60, 0x64, 0x68,
			     8, 1, 15, 0x0004, 9),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_SPI, "spi_sel", spi_parents, 0x60, 0x64, 0x68,
			     16, 2, 23, 0x0004, 10),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_MSDC50_0_HCLK, "msdc50_0_hclk_sel", msdc50_0_hclk_parents, 0x70, 0x74, 0x78,
			     8, 2, 15, 0x0004, 12),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_MSDC50_0, "msdc50_0_sel", msdc50_0_parents, 0x70, 0x74, 0x78,
			     16, 4, 23, 0x0004, 13),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_MSDC30_1, "msdc30_1_sel", msdc30_1_parents, 0x70, 0x74, 0x78,
			     24, 4, 31, 0x0004, 14),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_MSDC30_2, "msdc30_2_sel", msdc30_2_parents, 0x80, 0x84, 0x88,
			     0, 4, 7, 0x0004, 15),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_MSDC30_3, "msdc30_3_sel", msdc30_3_parents, 0x80, 0x84, 0x88,
			     8, 4, 15, 0x0004, 16),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_AUDIO, "audio_sel", audio_parents, 0x80, 0x84, 0x88,
			     16, 2, 23, 0x0004, 17),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_AUDINTBUS, "aud_intbus_sel", aud_intbus_parents, 0x80, 0x84, 0x88,
			     24, 2, 31, 0x0004, 18),
	MUX_CLR_SET_UPD(CLK_TOP_MUX_PMICSPI, "pmicspi_sel", pmicspi_parents, 0x90, 0x94, 0x98,
			0, 2, 0x0004, 19),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_ATB, "atb_sel", atb_parents, 0x90, 0x94, 0x98,
			     16, 2, 23, 0x0004, 21),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_DPI0, "dpi0_sel", dpi0_parents, 0xa0, 0xa4, 0xa8,
			     0, 3, 7, 0x0004, 23),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_SCAM, "scam_sel", scam_parents, 0xa0, 0xa4, 0xa8,
			     8, 1, 15, 0x0004, 24),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_AUD1, "aud_1_sel", aud_1_parents, 0xa0, 0xa4, 0xa8,
			     16, 1, 23, 0x0004, 25),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_AUD2, "aud_2_sel", aud_2_parents, 0xa0, 0xa4, 0xa8,
			     24, 1, 31, 0x0004, 26),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_DISPPWM, "disppwm_sel", disppwm_parents, 0xb0, 0xb4, 0xb8,
			     0, 2, 7, 0x0004, 27),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_SSUSBTOPSYS, "ssusb_top_sys_sel", ssusb_top_sys_parents, 0xb0, 0xb4, 0xb8,
			     8, 1, 15, 0x0004, 28),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_USBTOP, "usb_top_sel", usb_top_parents, 0xb0, 0xb4, 0xb8,
			     24, 1, 31, 0x0004, 30),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_SPM, "spm_sel", spm_parents, 0xc0, 0xc4, 0xc8,
			     0, 1, 7, 0x0004, 31),
	MUX_GATE_CLR_SET_UPD(CLK_TOP_MUX_BSISPI, "bsi_spi_sel", bsi_spi_parents, 0xc0, 0xc4, 0xc8,
			     8, 2, 15, 0x0008, 0),


};

static void clk_mt6755_topckgen_unregister(void *arg)
{
	struct clk_hw_onecell_data *data = arg;

	mtk_clk_unregister_muxes(topckgen_muxes, ARRAY_SIZE(topckgen_muxes), data);
	mtk_clk_unregister_factors(topckgen_factors, ARRAY_SIZE(topckgen_factors), data);
	mtk_free_clk_data(data);
}

static int clk_mt6755_topckgen_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *data;
	int ret;

	/* Binding IDs contain reserved holes; array length is not the ID range. */
	data = mtk_alloc_clk_data(CLK_TOP_NR_CLK);
	if (!data)
		return -ENOMEM;

	ret = mtk_clk_register_factors(topckgen_factors,
				       ARRAY_SIZE(topckgen_factors), data);
	if (ret)
		goto free_data;
	ret = mtk_clk_register_muxes(&pdev->dev, topckgen_muxes,
				     ARRAY_SIZE(topckgen_muxes), pdev->dev.of_node,
				     &mt6755_clk_lock, data);
	if (ret) {
		mtk_clk_unregister_factors(topckgen_factors,
					   ARRAY_SIZE(topckgen_factors), data);
		goto free_data;
	}

	ret = devm_add_action_or_reset(&pdev->dev, clk_mt6755_topckgen_unregister, data);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_onecell_get, data);

free_data:
	mtk_free_clk_data(data);
	return ret;
}

static const struct of_device_id of_match_mt6755_topckgen[] = {
	{ .compatible = "mediatek,mt6755-topckgen" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, of_match_mt6755_topckgen);

static struct platform_driver clk_mt6755_topckgen = {
	.probe = clk_mt6755_topckgen_probe,
	.driver = {
		.name = "clk-mt6755-topckgen",
		.of_match_table = of_match_mt6755_topckgen,
	},
};
module_platform_driver(clk_mt6755_topckgen);

MODULE_AUTHOR("Arseniy Velikanov <adomerlee@gmail.com>");
MODULE_DESCRIPTION("Mediatek MT6755 topckgen clock driver");
MODULE_LICENSE("GPL");
