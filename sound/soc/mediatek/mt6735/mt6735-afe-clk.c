// SPDX-License-Identifier: GPL-2.0-only
/*
 * mt6735-afe-clk.c  --  MediaTek MT6735/MT6737/MT6753 AFE clock control
 *
 * The audio subsystem of MT6735 lives behind the "audio" gate of the
 * infra-controller (CLK_INFRA_AUDIO) and runs from either the 26 MHz
 * crystal or the system PLL through the top-controller audio mux. The
 * front-end register file clocks are additionally gated in AUDIO_TOP_CON0
 * and are handled by the ADDA/DAI widgets.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>

#include "mt6735-afe-common.h"
#include "mt6735-afe-clk.h"

enum {
	CLK_INFRA_SYS_AUD,
	CLK_TOP_MUX_AUD,
	CLK_TOP_MUX_AUD_BUS,
	CLK_TOP_SYSPLL1_D4,
	CLK_CLK26M,
	CLK_NUM
};

static const char * const aud_clks[CLK_NUM] = {
	[CLK_INFRA_SYS_AUD] = "infra_sys_audio_clk",
	[CLK_TOP_MUX_AUD] = "top_mux_audio",
	[CLK_TOP_MUX_AUD_BUS] = "top_mux_aud_intbus",
	[CLK_TOP_SYSPLL1_D4] = "top_sys_pll1_d4",
	[CLK_CLK26M] = "top_clk26m_clk",
};

int mt6735_init_clock(struct mtk_base_afe *afe)
{
	struct mt6735_afe_private *afe_priv = afe->platform_priv;
	int i;

	afe_priv->clk = devm_kcalloc(afe->dev, CLK_NUM, sizeof(*afe_priv->clk),
				     GFP_KERNEL);
	if (!afe_priv->clk)
		return -ENOMEM;

	for (i = 0; i < CLK_NUM; i++) {
		afe_priv->clk[i] = devm_clk_get(afe->dev, aud_clks[i]);
		if (IS_ERR(afe_priv->clk[i])) {
			int ret = PTR_ERR(afe_priv->clk[i]);

			/* Optional clocks may be absent (incomplete DT). */
			if (ret == -ENOENT &&
			    (i == CLK_TOP_SYSPLL1_D4 || i == CLK_CLK26M)) {
				afe_priv->clk[i] = NULL;
				continue;
			}

			dev_err(afe->dev, "%s(), devm_clk_get %s failed: %d\n",
				__func__, aud_clks[i], ret);
			return ret;
		}
	}

	return 0;
}

int mt6735_afe_enable_clock(struct mtk_base_afe *afe)
{
	struct mt6735_afe_private *afe_priv = afe->platform_priv;
	int ret;

	ret = clk_prepare_enable(afe_priv->clk[CLK_INFRA_SYS_AUD]);
	if (ret) {
		dev_err(afe->dev, "%s(), enable %s failed: %d\n", __func__,
			aud_clks[CLK_INFRA_SYS_AUD], ret);
		return ret;
	}

	ret = clk_prepare_enable(afe_priv->clk[CLK_TOP_MUX_AUD]);
	if (ret) {
		dev_err(afe->dev, "%s(), enable %s failed: %d\n", __func__,
			aud_clks[CLK_TOP_MUX_AUD], ret);
		goto err_infra;
	}

	/* The AFE runs the 26 MHz crystal unless APLL is programmed. */
	if (afe_priv->clk[CLK_CLK26M]) {
		ret = clk_set_parent(afe_priv->clk[CLK_TOP_MUX_AUD],
				     afe_priv->clk[CLK_CLK26M]);
		if (ret)
			dev_warn(afe->dev, "%s(), muxing audio to clk26m failed: %d\n",
				 __func__, ret);
	}

	ret = clk_prepare_enable(afe_priv->clk[CLK_TOP_MUX_AUD_BUS]);
	if (ret) {
		dev_err(afe->dev, "%s(), enable %s failed: %d\n", __func__,
			aud_clks[CLK_TOP_MUX_AUD_BUS], ret);
		goto err_mux;
	}

	if (afe_priv->clk[CLK_TOP_SYSPLL1_D4]) {
		ret = clk_prepare_enable(afe_priv->clk[CLK_TOP_SYSPLL1_D4]);
		if (ret)
			dev_warn(afe->dev, "%s(), enable %s failed: %d\n",
				 __func__, aud_clks[CLK_TOP_SYSPLL1_D4], ret);
	}

	return 0;

err_mux:
	clk_disable_unprepare(afe_priv->clk[CLK_TOP_MUX_AUD]);
err_infra:
	clk_disable_unprepare(afe_priv->clk[CLK_INFRA_SYS_AUD]);

	return ret;
}

int mt6735_afe_disable_clock(struct mtk_base_afe *afe)
{
	struct mt6735_afe_private *afe_priv = afe->platform_priv;

	if (afe_priv->clk[CLK_TOP_SYSPLL1_D4])
		clk_disable_unprepare(afe_priv->clk[CLK_TOP_SYSPLL1_D4]);

	clk_disable_unprepare(afe_priv->clk[CLK_TOP_MUX_AUD_BUS]);
	clk_disable_unprepare(afe_priv->clk[CLK_TOP_MUX_AUD]);
	clk_disable_unprepare(afe_priv->clk[CLK_INFRA_SYS_AUD]);

	return 0;
}
