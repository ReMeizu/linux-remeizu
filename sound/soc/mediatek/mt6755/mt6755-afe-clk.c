// SPDX-License-Identifier: GPL-2.0-only
/*
 * mt6755-afe-clk.c  --  MediaTek MT6755/MT6750 AFE clock control
 *
 * The MT6755 keeps the front-end's register-file and memory-interface
 * clocks behind the infra-controller "audio" gate and runs the module from
 * the 26 MHz crystal or a system PLL selected by the top-controller audio
 * mux. There is no separate audsys clock controller in the mainline clock
 * driver for this SoC, so the audio clocks are requested from topckgen and
 * infracfg exactly like the upstream MT6797 driver does.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>

#include "mt6755-afe-common.h"
#include "mt6755-afe-clk.h"

enum {
	CLK_INFRA_SYS_AUD,
	CLK_TOP_MUX_AUD,
	CLK_TOP_MUX_AUD_BUS,
	CLK_TOP_SYSPLL3_D4,
	CLK_TOP_SYSPLL1_D4,
	CLK_CLK26M,
	CLK_NUM
};

static const char * const aud_clks[CLK_NUM] = {
	[CLK_INFRA_SYS_AUD] = "infra_sys_audio_clk",
	[CLK_TOP_MUX_AUD] = "top_mux_audio",
	[CLK_TOP_MUX_AUD_BUS] = "top_mux_aud_intbus",
	[CLK_TOP_SYSPLL3_D4] = "top_sys_pll3_d4",
	[CLK_TOP_SYSPLL1_D4] = "top_sys_pll1_d4",
	[CLK_CLK26M] = "top_clk26m_clk",
};

int mt6755_init_clock(struct mtk_base_afe *afe)
{
	struct mt6755_afe_private *afe_priv = afe->platform_priv;
	int i;

	afe_priv->clk = devm_kcalloc(afe->dev, CLK_NUM, sizeof(*afe_priv->clk),
				     GFP_KERNEL);
	if (!afe_priv->clk)
		return -ENOMEM;

	for (i = 0; i < CLK_NUM; i++) {
		afe_priv->clk[i] = devm_clk_get(afe->dev, aud_clks[i]);
		if (IS_ERR(afe_priv->clk[i])) {
			int ret = PTR_ERR(afe_priv->clk[i]);

			dev_err(afe->dev, "%s(), devm_clk_get %s fail, ret %d\n",
				__func__, aud_clks[i], ret);
			return ret;
		}
	}

	return 0;
}

int mt6755_afe_enable_clock(struct mtk_base_afe *afe)
{
	struct mt6755_afe_private *afe_priv = afe->platform_priv;
	int ret;

	ret = clk_prepare_enable(afe_priv->clk[CLK_INFRA_SYS_AUD]);
	if (ret) {
		dev_err(afe->dev, "%s(), enable %s fail, ret %d\n", __func__,
			aud_clks[CLK_INFRA_SYS_AUD], ret);
		return ret;
	}

	ret = clk_prepare_enable(afe_priv->clk[CLK_TOP_MUX_AUD]);
	if (ret) {
		dev_err(afe->dev, "%s(), enable %s fail, ret %d\n", __func__,
			aud_clks[CLK_TOP_MUX_AUD], ret);
		goto err_infra;
	}

	/* run the AFE from the crystal unless APLL/APLL2 are programmed */
	ret = clk_set_parent(afe_priv->clk[CLK_TOP_MUX_AUD],
			     afe_priv->clk[CLK_CLK26M]);
	if (ret)
		dev_warn(afe->dev, "%s(), clk_set_parent %s-%s fail, ret %d\n",
			 __func__, aud_clks[CLK_TOP_MUX_AUD],
			 aud_clks[CLK_CLK26M], ret);

	ret = clk_prepare_enable(afe_priv->clk[CLK_TOP_MUX_AUD_BUS]);
	if (ret) {
		dev_err(afe->dev, "%s(), enable %s fail, ret %d\n", __func__,
			aud_clks[CLK_TOP_MUX_AUD_BUS], ret);
		goto err_mux;
	}

	return 0;

err_mux:
	clk_disable_unprepare(afe_priv->clk[CLK_TOP_MUX_AUD]);
err_infra:
	clk_disable_unprepare(afe_priv->clk[CLK_INFRA_SYS_AUD]);

	return ret;
}

int mt6755_afe_disable_clock(struct mtk_base_afe *afe)
{
	struct mt6755_afe_private *afe_priv = afe->platform_priv;

	clk_disable_unprepare(afe_priv->clk[CLK_TOP_MUX_AUD_BUS]);
	clk_disable_unprepare(afe_priv->clk[CLK_TOP_MUX_AUD]);
	clk_disable_unprepare(afe_priv->clk[CLK_INFRA_SYS_AUD]);

	return 0;
}
