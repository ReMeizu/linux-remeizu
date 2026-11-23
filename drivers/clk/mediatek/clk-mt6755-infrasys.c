// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Copyright (c) 2023 Arseniy Velikanov <adomerlee@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"
#include "reset.h"

#include <dt-bindings/clock/mediatek,mt6755-clk.h>
#include <dt-bindings/reset/mt6755-resets.h>

static const struct mtk_gate_regs infra0_cg_regs = {
	.set_ofs = 0x80,
	.clr_ofs = 0x84,
	.sta_ofs = 0x90,
};

static const struct mtk_gate_regs infra1_cg_regs = {
	.set_ofs = 0x88,
	.clr_ofs = 0x8c,
	.sta_ofs = 0x94,
};

static const struct mtk_gate_regs infra2_cg_regs = {
	.set_ofs = 0xa4,
	.clr_ofs = 0xa8,
	.sta_ofs = 0xac,
};

static const struct mtk_gate infra_gates[] = {
	GATE_MTK(CLK_INFRA_PMIC_TMR, "infra_pmictmr", "pmicspi_sel", &infra0_cg_regs, 0,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_PMIC_AP, "infra_pmicap", "pmicspi_sel", &infra0_cg_regs, 1,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_PMIC_MD, "infra_pmicmd", "pmicspi_sel", &infra0_cg_regs, 2,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_PMIC_CONN, "infra_pmicconn", "pmicspi_sel", &infra0_cg_regs, 3,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_APXGPT, "infra_apxgpt", "axi_sel", &infra0_cg_regs, 6,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_ICUSB, "infra_icusb", "axi_sel", &infra0_cg_regs, 8,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_GCE, "infra_gce", "axi_sel", &infra0_cg_regs, 9,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_THERM, "infra_therm", "axi_sel", &infra0_cg_regs, 10,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_I2C0, "infra_i2c0", "i2c_sel", &infra0_cg_regs, 11,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_I2C1, "infra_i2c1", "i2c_sel", &infra0_cg_regs, 12,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_I2C2, "infra_i2c2", "i2c_sel", &infra0_cg_regs, 13,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_I2C3, "infra_i2c3", "i2c_sel", &infra0_cg_regs, 14,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_PWM_HCLK, "infra_pwmhclk", "axi_sel", &infra0_cg_regs, 15,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_PWM1, "infra_pwm1", "i2c_sel", &infra0_cg_regs, 16,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_PWM2, "infra_pwm2", "i2c_sel", &infra0_cg_regs, 17,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_PWM3, "infra_pwm3", "i2c_sel", &infra0_cg_regs, 18,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_PWM4, "infra_pwm4", "i2c_sel", &infra0_cg_regs, 19,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_PWM, "infra_pwm", "i2c_sel", &infra0_cg_regs, 21,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_UART0, "infra_uart0", "uart_sel", &infra0_cg_regs, 22,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_UART1, "infra_uart1", "uart_sel", &infra0_cg_regs, 23,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_UART2, "infra_uart2", "uart_sel", &infra0_cg_regs, 24,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_UART3, "infra_uart3", "uart_sel", &infra0_cg_regs, 25,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_MD2MD_CCIF0, "infra_md2mdccif0", "axi_sel", &infra0_cg_regs, 27,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_MD2MD_CCIF1, "infra_md2mdccif1", "axi_sel", &infra0_cg_regs, 28,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_MD2MD_CCIF2, "infra_md2mdccif2", "axi_sel", &infra0_cg_regs, 29,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_BTIF, "infra_btif", "axi_sel", &infra0_cg_regs, 31,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_MD2MD_CCIF3, "infra_md2mdccif3", "axi_sel", &infra1_cg_regs, 0,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_SPI0, "infra_spi0", "spi_sel", &infra1_cg_regs, 1,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_MSDC0, "infra_msdc0", "msdc50_0_sel", &infra1_cg_regs, 2,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_MD2MD_CCIF4, "infra_md2mdccif4", "axi_sel", &infra1_cg_regs, 3,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_MSDC1, "infra_msdc1", "msdc30_1_sel", &infra1_cg_regs, 4,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_MSDC2, "infra_msdc2", "msdc30_2_sel", &infra1_cg_regs, 5,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_MSDC3, "infra_msdc3", "msdc30_3_sel", &infra1_cg_regs, 6,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_MD2MD_CCIF5, "infra_md2mdccif5", "axi_sel", &infra1_cg_regs, 7,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_GCPU, "infra_gcpu", "axi_sel", &infra1_cg_regs, 8,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_TRNG, "infra_trng", "axi_sel", &infra1_cg_regs, 9,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_CPUM, "infra_cpum", "axi_sel", &infra1_cg_regs, 11,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_CCIF1_AP, "infra_ccif1ap", "axi_sel", &infra1_cg_regs, 12,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_CCIF1_MD, "infra_ccif1md", "axi_sel", &infra1_cg_regs, 13,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_AP_DMA, "infra_apdma", "axi_sel", &infra1_cg_regs, 18,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_XIU, "infra_xiu", "axi_sel", &infra1_cg_regs, 19,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_DEVICE_APC, "infra_deviceapc", "axi_sel", &infra1_cg_regs, 20,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_XIU2AHB, "infra_xiu2ahb", "axi_sel", &infra1_cg_regs, 21,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_CCIF_AP, "infra_ccifap", "axi_sel", &infra1_cg_regs, 23,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_DEBUGSYS, "infra_debugsys", "axi_sel", &infra1_cg_regs, 24,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_AUDIO, "infra_audio", "axi_sel", &infra1_cg_regs, 25,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_CCIF_MD, "infra_ccifmd", "axi_sel", &infra1_cg_regs, 26,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_SSUSB_TOP, "infra_ssusbtop", "ssusb_top_sys_sel", &infra2_cg_regs, 1,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_DISP_PWM, "infra_disppwm", "axi_sel", &infra2_cg_regs, 2,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_SPI1, "infra_spi1", "spi_sel", &infra2_cg_regs, 6,
		 &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_INFRA_I2C4, "infra_i2c4", "i2c_sel", &infra2_cg_regs, 7,
		 &mtk_clk_gate_ops_setclr),
};

static void mt6755_infrasys_unregister(void *arg)
{
	struct clk_hw_onecell_data *data = arg;

	mtk_clk_unregister_gates(infra_gates, ARRAY_SIZE(infra_gates), data);
	mtk_free_clk_data(data);
}

static u16 mt6755_infra_rst_ofs[] = { 0x140 };
static u16 mt6755_infra_rst_map[] = {
	[MT6755_INFRA_PMIC_WRAP_RST] = 0,
};

static const struct mtk_clk_rst_desc mt6755_infra_rst_desc = {
	.version = MTK_RST_SET_CLR,
	.rst_bank_ofs = mt6755_infra_rst_ofs,
	.rst_bank_nr = ARRAY_SIZE(mt6755_infra_rst_ofs),
	.rst_idx_map = mt6755_infra_rst_map,
	.rst_idx_map_nr = ARRAY_SIZE(mt6755_infra_rst_map),
};

static int mt6755_infrasys_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *data;
	int ret;

	ret = mtk_register_reset_controller_with_dev(&pdev->dev,
						   &mt6755_infra_rst_desc);
	if (ret)
		return ret;

	/* Binding IDs contain reserved holes; array length is not the ID range. */
	data = mtk_alloc_clk_data(CLK_INFRA_NR_CLK);
	if (!data)
		return -ENOMEM;

	ret = mtk_clk_register_gates(&pdev->dev, pdev->dev.of_node,
				     infra_gates, ARRAY_SIZE(infra_gates), data);
	if (ret)
		goto free_data;

	ret = devm_add_action_or_reset(&pdev->dev, mt6755_infrasys_unregister, data);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_onecell_get, data);

free_data:
	mtk_free_clk_data(data);
	return ret;
}

static const struct of_device_id mt6755_infrasys_match[] = {
	{ .compatible = "mediatek,mt6755-infrasys" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6755_infrasys_match);

static struct platform_driver mt6755_infrasys_driver = {
	.probe = mt6755_infrasys_probe,
	.driver = {
		.name = "clk-mt6755-infrasys",
		.of_match_table = mt6755_infrasys_match,
	},
};
module_platform_driver(mt6755_infrasys_driver);

MODULE_AUTHOR("Arseniy Velikanov <adomerlee@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6755 infrastructure clocks");
MODULE_LICENSE("GPL");
