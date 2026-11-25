// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2024 - 2025 Dang Huynh <dang.huynh@mainlining.org>
 */

#include <clk.h>
#include <display.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <dw_hdmi.h>
#include <edid.h>
#include <asm/arch-rockchip/clock.h>
#include <asm/arch-rockchip/cru_rk3568.h>
#include <asm/arch-rockchip/hardware.h>
#include <asm/arch-rockchip/grf_rk3568.h>
#include <power/regulator.h>
#include "rk_hdmi.h"

/* Default HDMI PHY reference rate to bring HPLL up for hot-plug detect */
#define RK3568_HDMI_REF_DEFAULT		148500000

#define RK3568_IO_DDC_IN_MSK             ((3 << 14) | (3 << (14 + 16)))

/* CLKGATE_CON21: pclk_hdmi_host (b3), clk_hdmi_sfr (b4), clk_hdmi_cec (b5) */
#define RK3568_HDMI_CLKGATE_CON		21
#define RK3568_HDMI_CLKGATE_MSK		(0x7 << 3)

static int rk3568_hdmi_enable(struct udevice *dev, int panel_bpp,
			      const struct display_timing *edid)
{
	struct rk_hdmi_priv *priv = dev_get_priv(dev);
	struct clk ref_clk;

	/* Drive the PHY reference at the pixel clock for this mode */
	if (!clk_get_by_name(dev, "ref", &ref_clk))
		clk_set_rate(&ref_clk, edid->pixelclock.typ);

	return dw_hdmi_enable(&priv->hdmi, edid);
}

static int rk3568_hdmi_of_to_plat(struct udevice *dev)
{
	struct rk_hdmi_priv *priv = dev_get_priv(dev);
	struct dw_hdmi *hdmi = &priv->hdmi;

	hdmi->i2c_clk_high = 0x6c;
	hdmi->i2c_clk_low = 0x7d;

	return rk_hdmi_of_to_plat(dev);
}

static const char * const rk3568_hdmi_supplies[] = {
	"avdd-0v9-supply",
	"avdd-1v8-supply",
};

static void rk3568_hdmi_enable_supplies(struct udevice *dev)
{
	struct dm_regulator_uclass_plat *plat;
	struct udevice *reg;
	int ret;
	int i;

	for (i = 0; i < ARRAY_SIZE(rk3568_hdmi_supplies); i++) {
		ret = device_get_supply_regulator(dev, rk3568_hdmi_supplies[i],
						  &reg);
		if (ret) {
			dev_err(dev, "supply %s not found: %d\n",
				rk3568_hdmi_supplies[i], ret);
			continue;
		}

		plat = dev_get_uclass_plat(reg);
		if (plat->min_uV)
			regulator_set_value(reg, plat->min_uV);

		ret = regulator_set_enable(reg, true);
		if (ret)
			dev_err(dev, "failed to enable %s: %d\n",
				rk3568_hdmi_supplies[i], ret);
		else
			dev_info(dev, "enabled supply %s\n",
				 rk3568_hdmi_supplies[i]);
	}
}

static int rk3568_hdmi_read_edid(struct udevice *dev, u8 *buf, int buf_size)
{
	int ret = rk_hdmi_read_edid(dev, buf, buf_size);

	if (ret < 0)
		dev_err(dev, "failed to read EDID: %d\n", ret);

	return ret;
}

static int rk3568_hdmi_probe(struct udevice *dev)
{
	struct rk_hdmi_priv *priv = dev_get_priv(dev);
	struct rk3568_grf *grf = priv->grf;
	struct rk3568_cru *cru = rockchip_get_cru();
	struct clk ref_clk;
	int ret;

	rk3568_hdmi_enable_supplies(dev);

	/*
	 * Ungate the HDMI controller clocks. The RK3568 clock driver does
	 * not manage these gates.
	 */
	if (cru)
		rk_clrreg(&cru->clkgate_con[RK3568_HDMI_CLKGATE_CON],
			  RK3568_HDMI_CLKGATE_MSK);

	/*
	 * Bring up the PHY reference clock (clk_hdmi_ref off HPLL) before
	 * probing: the internal HDMI PHY needs its reference running for the
	 * hot-plug-detect status to be valid, otherwise the HPD poll times
	 * out and the controller is not probed. Mainline Linux likewise
	 * enables this clock at probe.
	 */
	if (!clk_get_by_name(dev, "ref", &ref_clk))
		clk_set_rate(&ref_clk, RK3568_HDMI_REF_DEFAULT);

	writel(RK3568_IO_DDC_IN_MSK, &grf->vo_con1);

	ret = rk_hdmi_probe(dev);
	if (ret)
		dev_err(dev, "HDMI probe failed (HPD/PHY): %d\n", ret);
	else
		dev_info(dev, "HDMI ready\n");

	return ret;
}

static const struct dm_display_ops rk3568_hdmi_ops = {
	.read_edid = rk3568_hdmi_read_edid,
	.enable = rk3568_hdmi_enable,
};

static const struct udevice_id rk3568_hdmi_ids[] = {
	{ .compatible = "rockchip,rk3568-dw-hdmi" },
	{ }
};

U_BOOT_DRIVER(rk3568_hdmi_rockchip) = {
	.name = "rk3568_hdmi_rockchip",
	.id = UCLASS_DISPLAY,
	.of_match = rk3568_hdmi_ids,
	.ops = &rk3568_hdmi_ops,
	.of_to_plat = rk3568_hdmi_of_to_plat,
	.probe = rk3568_hdmi_probe,
	.priv_auto = sizeof(struct rk_hdmi_priv),
};
