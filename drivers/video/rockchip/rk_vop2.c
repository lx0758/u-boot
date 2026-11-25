// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 - 2025 Dang Huynh <dang.huynh@mainlining.org>
 *
 * Based on rk_vop.c:
 *   Copyright (c) 2015 Google, Inc
 *   Copyright 2014 Rockchip Inc.
 */

#include <clk.h>
#include <display.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <edid.h>
#include <log.h>
#include <panel.h>
#include <regmap.h>
#include <reset.h>
#include <syscon.h>
#include <video.h>
#include <video_bridge.h>
#include <asm/global_data.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <asm/arch-rockchip/clock.h>
#include <asm/arch-rockchip/vop_rk3568.h>
#include <dm/device-internal.h>
#include <dm/uclass-internal.h>
#include <dt-bindings/soc/rockchip,vop2.h>
#include <efi.h>
#include <efi_loader.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <power/regulator.h>

#include "rk_vop2.h"

DECLARE_GLOBAL_DATA_PTR;

enum vop_pol {
	HSYNC_POSITIVE = 0,
	VSYNC_POSITIVE = 1,
	DEN_NEGATIVE   = 2,
	DCLK_INVERT    = 3
};

static void rkvop2_cfg_regdone(struct rk3568_vop_sysctrl *sysctrl, int port)
{
	u32 reg;

	reg = M_GLOBAL_REGDONE;

	/*
	 * For RK3588, changes will only take effect when the same bit is
	 * leftshifted by 16.
	 */
	reg |= M_LOAD_GLOBAL(port) | M_LOAD_GLOBAL(port) << 16;

	writel(reg, &sysctrl->reg_cfg_done);
}

static int rkvop2_enable(struct udevice *dev, ulong fbbase,
			 int fb_bits_per_pixel, const struct display_timing *edid,
			 int port, int win_id, struct rkvop2_platdata *platdata)
{
	struct rk_vop2_priv *priv = dev_get_priv(dev);
	struct rk3568_vop_overlay *overlay = priv->regs + VOP2_OVERLAY_OFFSET;
	struct rk3568_vop_esmart *esmart;
	bool is_cluster = false;
	u8 layer;
	u32 reg;
	u32 rgb_mode;
	u32 hactive = edid->hactive.typ;
	u32 vactive = edid->vactive.typ;

	if (platdata->layers[win_id] < 0)
		return -EINVAL;

	switch (platdata->layers[win_id]) {
	case ROCKCHIP_VOP2_CLUSTER0:
	case ROCKCHIP_VOP2_CLUSTER1:
	case ROCKCHIP_VOP2_CLUSTER2:
	case ROCKCHIP_VOP2_CLUSTER3:
		is_cluster = true;
		break;
	default:
		break;
	}

	layer = platdata->layers[win_id];

	debug("(%s, %s): win_id = %d - layer = %d - cluster: %d\n",
	      dev_read_name(dev),  __func__, win_id, layer, is_cluster);

	/* TODO: Support VOP2 CLUSTER */
	if (is_cluster) {
		dev_err(dev, "win_id is a cluster, not supported.\n");
		return -ENOSYS;
	}

	esmart = priv->regs + VOP2_ESMART_OFFSET(layer - 4);

	debug("(%s, %s): esmart addr: 0x%p\n", dev_read_name(dev), __func__, esmart);

	writel(V_ACT_WIDTH(hactive - 1) | V_ACT_HEIGHT(vactive - 1),
	       &esmart->esmart_region0_act_info);

	/* Set offset to 0,0 */
	writel(0, &esmart->esmart_region0_dsp_offset);

	writel(V_DSP_WIDTH(hactive - 1) |
			V_DSP_HEIGHT(vactive - 1),
			&esmart->esmart_region0_dsp_info);

	switch (fb_bits_per_pixel) {
	case 16:
		rgb_mode = RGB565;
		writel(V_RGB565_VIRWIDTH(hactive), &esmart->esmart_region0_vir);
		break;
	case 24:
		rgb_mode = RGB888;
		writel(V_RGB888_VIRWIDTH(hactive), &esmart->esmart_region0_vir);
		break;
	case 32:
	default:
		rgb_mode = ARGB8888;
		writel(V_ARGB888_VIRWIDTH(hactive), &esmart->esmart_region0_vir);
		break;
	}

	writel(fbbase, &esmart->esmart_region0_mst_yrgb);

	writel(V_ESMART_REGION0_DATA_FMT(rgb_mode) | M_ESMART_REGION0_MST_EN,
	       &esmart->esmart_region0_mst_ctl);

	/* Set esmart to the destination video port */
	reg = V_ESMART_SEL_PORT(layer - 4, port);

	/*
	 * VOP2 requires every port mux to be configured.
	 *
	 * As U-Boot only supports singledisplay, we'll set all
	 * unused ports to set layer to 8 (disabled).
	 */
	for (int i = 0; i < 4; i++) {
		if (i != port)
			reg |= V_PORT_MUX(8, i);
	}

	writel(reg, &overlay->port_sel);

	/* Set layer 0 to win_id */
	writel(V_LAYER_SEL(0, win_id), &overlay->layer_sel);

	reg = readl(&overlay->overlay_ctrl) | M_LAYER_SEL_REGDONE_EN;
	writel(reg, &overlay->overlay_ctrl);

	priv->layer = layer;

	return 0;
}

static void rkvop2_set_pin_polarity(struct udevice *dev,
				    enum vop_modes mode, u32 polarity)
{
	struct rkvop2_driverdata *ops =
		(struct rkvop2_driverdata *)dev_get_driver_data(dev);

	if (ops->set_pin_polarity)
		ops->set_pin_polarity(dev, mode, polarity);
}

static void rkvop2_enable_output(struct udevice *dev, enum vop_modes mode, u32 port)
{
	struct rkvop2_driverdata *ops =
		(struct rkvop2_driverdata *)dev_get_driver_data(dev);

	if (ops->enable_output)
		ops->enable_output(dev, mode, port);
}

static void rkvop2_mode_set(struct udevice *dev,
			    const struct display_timing *edid,
		enum vop_modes mode, int port,
		struct rkvop2_platdata *platdata)
{
	struct rk_vop2_priv *priv = dev_get_priv(dev);
	struct rk3568_vop_sysctrl *sysctrl = priv->regs + VOP2_SYSREG_OFFSET;
	struct rk3568_vop_post *post = priv->regs + VOP2_POST_OFFSET(port);
	struct rkvop2_driverdata *data =
		(struct rkvop2_driverdata *)dev_get_driver_data(dev);

	debug("(%s, %s): port addr: 0x%p\n", dev_read_name(dev), __func__, post);

	u32 hactive = edid->hactive.typ;
	u32 vactive = edid->vactive.typ;
	u32 hsync_len = edid->hsync_len.typ;
	u32 hback_porch = edid->hback_porch.typ;
	u32 vsync_len = edid->vsync_len.typ;
	u32 vback_porch = edid->vback_porch.typ;
	u32 hfront_porch = edid->hfront_porch.typ;
	u32 vfront_porch = edid->vfront_porch.typ;
	int mode_flags;
	u32 pin_polarity;
	u32 reg;

	pin_polarity = 0;
	/* Linux only inverts DCLK when the display bus asks for it (never for DSI) */
	if (mode != VOP_MODE_MIPI && mode != VOP_MODE_MIPI1)
		pin_polarity |= BIT(DCLK_INVERT);
	if (edid->flags & DISPLAY_FLAGS_HSYNC_HIGH)
		pin_polarity |= BIT(HSYNC_POSITIVE);
	if (edid->flags & DISPLAY_FLAGS_VSYNC_HIGH)
		pin_polarity |= BIT(VSYNC_POSITIVE);

	rkvop2_enable_output(dev, mode, port);
	rkvop2_set_pin_polarity(dev, mode, pin_polarity);

	mode_flags = 0;  /* RGB888 */
	if ((data->features & VOP_FEATURE_OUTPUT_10BIT) &&
	    mode == VOP_MODE_HDMI)
		mode_flags = 15;  /* RGBaaa */

	reg = V_DSP_OUT_MODE(mode_flags);

	/* MIPI: enable the 10->8 post-processing stage (DSP_CTRL bit16), as Linux does */
	if (mode == VOP_MODE_MIPI || mode == VOP_MODE_MIPI1)
		reg |= BIT(16);

	debug("(%s, %s): bg_dly: %d\n",
	      dev_read_name(dev), __func__, platdata->bg_dly[port]);

	if (platdata->bg_dly[port] < 0) {
		dev_err(dev, "bg_dly is zero for vp%d\n", port);
		return;
	}

	writel(((platdata->bg_dly[port] + (hactive >> 1) - 1) << 16) | hsync_len,
	       &post->prescan_htimings);

	writel(V_HSYNC(hsync_len) |
			V_HORPRD(hsync_len + hback_porch + hactive + hfront_porch),
			&post->dsp_htotal_hs_end);

	writel(V_HEAP(hsync_len + hback_porch + hactive) |
			V_HASP(hsync_len + hback_porch),
			&post->dsp_hact_st_end);

	writel(V_VAEP(vsync_len + vback_porch + vactive) |
			V_VASP(vsync_len + vback_porch),
			&post->dsp_vact_st_end);

	writel(V_VSYNC(vsync_len) |
			V_VERPRD(vsync_len + vback_porch + vactive + vfront_porch),
			&post->dsp_vtotal_vs_end);

	writel(V_HEAP(hsync_len + hback_porch + hactive) |
			V_HASP(hsync_len + hback_porch),
			&post->dsp_hact_info);

	writel(V_VAEP(vsync_len + vback_porch + vactive) |
			V_VASP(vsync_len + vback_porch),
			&post->dsp_vact_info);

	/* No scaling */
	writel(0x10001000, &post->scl_factor_yrgb);

	/* Linux programs this to 0 for every output; the reset value is not 0 */
	writel(0, &post->mipi_ctrl);

	/* Background colour of the video port: match Linux (black) */
	writel(0, &post->dsp_bg);

	/* Colour-bar generator off: match Linux, which programs 0 */
	writel(0, &post->color_ctrl);

	writel(reg, &post->dsp_ctrl);

	rkvop2_cfg_regdone(sysctrl, port);
}

/*
 * The framebuffer is reserved from the top of DRAM with a fixed maximum size
 * (CONFIG_VIDEO_ROCKCHIP_MAX_*). A monitor whose EDID preferred mode is larger
 * than that would write past the framebuffer, so fall back to a safe VESA mode
 * that every HDMI sink accepts.
 */
static const struct display_timing rk_vop2_fallback_timing = {
	.pixelclock.typ		= 65000000,
	.hactive.typ		= 1024,
	.hfront_porch.typ	= 24,
	.hback_porch.typ	= 160,
	.hsync_len.typ		= 136,
	.vactive.typ		= 768,
	.vfront_porch.typ	= 3,
	.vback_porch.typ	= 29,
	.vsync_len.typ		= 6,
	.flags			= DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW,
};

static void rk_vop2_clamp_timing(struct udevice *dev,
				 struct display_timing *timing)
{
	if (timing->hactive.typ <= CONFIG_VIDEO_ROCKCHIP_MAX_XRES &&
	    timing->vactive.typ <= CONFIG_VIDEO_ROCKCHIP_MAX_YRES)
		return;

	dev_warn(dev, "mode %ux%u exceeds framebuffer limit %dx%d, using 1024x768\n",
		 timing->hactive.typ, timing->vactive.typ,
		 CONFIG_VIDEO_ROCKCHIP_MAX_XRES, CONFIG_VIDEO_ROCKCHIP_MAX_YRES);
	*timing = rk_vop2_fallback_timing;
}

/**
 * rk_display_init() - Try to enable the given display device
 *
 * This function performs many steps:
 * - Finds the display device being referenced by @ep_node
 * - Puts the VOP's ID into its uclass platform data
 * - Probes the device to set it up
 * - Reads the timing information (from EDID or panel)
 * - Sets up the VOP clocks, etc. for the selected pixel clock and display mode
 * - Enables the display (the display device handles this and will do different
 *     things depending on the display type)
 * - Tells the uclass about the display resolution so that the console will
 *     appear correctly
 *
 * @dev:	VOP device that we want to connect to the display
 * @fbbase:	Frame buffer address
 * @vp_node:	Device tree node to process
 * Return: 0 if OK, -ve if something went wrong
 */
static int rk_display_init(struct udevice *dev, ulong fbbase, ofnode vp_node)
{
	struct rk_vop2_priv *priv = dev_get_priv(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct rkvop2_driverdata *drvdata =
		(struct rkvop2_driverdata *)dev_get_driver_data(dev);
	struct rkvop2_platdata *platdata =
		(struct rkvop2_platdata *)drvdata->platdata;
	ofnode ep_node;
	int vop_id, port_id, win_id;
	u32 ep_id;
	struct display_timing timing;
	struct udevice *disp = NULL;
	struct udevice *bridge = NULL;
	int ret;
	u32 remote_phandle;
	struct display_plat *disp_uc_plat;
	enum video_log2_bpp l2bpp;
	ofnode remote;
	const char *compat;
	char dclk_name[9];
	struct clk dclk;

	debug("%s(%s, 0x%lx, %s)\n", __func__,
	      dev_read_name(dev), fbbase, ofnode_get_name(vp_node));

	port_id = ofnode_read_u32_default(vp_node, "reg", -1);
	if (port_id < 0) {
		dev_err(dev, "%s: no video port id\n", ofnode_get_name(vp_node));
		return port_id;
	}

	dev_info(dev, "vp%d: probing\n", port_id);

	ep_node = ofnode_first_subnode(vp_node);
	if (!ofnode_valid(ep_node)) {
		dev_err(dev, "vp%d: no valid subnode\n", port_id);
		return -EINVAL;
	}

	/* Endpoint id selects between the two MIPI outputs (DSI0/DSI1) */
	ep_id = ofnode_read_u32_default(ep_node, "reg", 0);

	ret = ofnode_read_u32(ep_node, "remote-endpoint", &remote_phandle);
	if (ret) {
		dev_err(dev, "vp%d: no remote-endpoint\n", port_id);
		return ret;
	}

	remote = ofnode_get_by_phandle(remote_phandle);
	if (!ofnode_valid(remote)) {
		dev_err(dev, "vp%d: invalid remote-endpoint phandle\n", port_id);
		return -EINVAL;
	}

	remote = ofnode_get_parent(remote);
	if (!ofnode_valid(remote)) {
		dev_err(dev, "vp%d: remote-endpoint has no parent\n", port_id);
		return -EINVAL;
	}

	/*
	 * The remote-endpoint references into a subnode of the encoder
	 * (i.e. HDMI, MIPI, etc.) with the DTS looking something like
	 * the following:
	 *
	 * hdmi: hdmi@fe0a0000 {
	 *   ports {
	 *     hdmi_in: port {
	 *       hdmi_in_vp0: endpoint { ... };
	 *     }
	 *   }
	 * }
	 *
	 * This isn't any different from how VOP1 works, so we'll adapt
	 * the same method of finding the display from the original code
	 * (find the enclosing device of "UCLASS_DISPLAY")
	 *
	 * We also look for UCLASS_VIDEO_BRIDGE so we can use the existing
	 * DW MIPI DSI driver for Rockchip.
	 */
	while (ofnode_valid(remote)) {
		remote = ofnode_get_parent(remote);
		if (!ofnode_valid(remote)) {
			dev_err(dev, "vp%d: no display/bridge device for endpoint\n",
				port_id);
			return -EINVAL;
		}

		if (IS_ENABLED(CONFIG_VIDEO_BRIDGE))
			uclass_find_device_by_ofnode(UCLASS_VIDEO_BRIDGE, remote, &bridge);

		uclass_find_device_by_ofnode(UCLASS_DISPLAY, remote, &disp);
		if (disp || bridge)
			break;
	};
	compat = ofnode_get_property(remote, "compatible", NULL);
	if (!compat) {
		dev_err(dev, "vp%d: remote endpoint has no compatible\n", port_id);
		return -EINVAL;
	}
	if (strstr(compat, "edp")) {
		vop_id = VOP_MODE_EDP;
	} else if (strstr(compat, "mipi")) {
		vop_id = ep_id == ROCKCHIP_VOP2_EP_MIPI1 ?
			 VOP_MODE_MIPI1 : VOP_MODE_MIPI;
	} else if (strstr(compat, "hdmi")) {
		vop_id = VOP_MODE_HDMI;
	} else if (strstr(compat, "rk3588-dp")) {
		vop_id = VOP_MODE_DP;
	} else if (strstr(compat, "lvds")) {
		vop_id = VOP_MODE_LVDS;
	} else {
		dev_err(dev, "vp%d: unsupported encoder %s\n", port_id, compat);
		return -EINVAL;
	}
	debug("vop_id=%d - port=%d\n", vop_id, port_id);

	/* Get the video port clock and enable it */
	snprintf(dclk_name, sizeof(dclk_name), "dclk_vp%d", port_id);
	ret = clk_get_by_name(dev, dclk_name, &dclk);
	if (ret < 0)
		return ret;

	if (bridge) {
		/* video bridge detected, probe it */
		ret = device_probe(bridge);
		if (ret) {
			dev_err(dev, "Failed to probe video bridge: %d\n", ret);
			return ret;
		}

		/* Attach the DSI controller and the display to the bridge. */
		ret = video_bridge_attach(bridge);
		if (ret) {
			dev_err(dev, "Failed to attach video bridge: %d\n", ret);
			return ret;
		}

		ret = video_bridge_get_display_timing(bridge, &timing);
		if (ret) {
			dev_err(dev, "Failed to read timings: %d\n", ret);
			return ret;
		}
	} else {
		disp_uc_plat = dev_get_uclass_plat(disp);
		debug("Found device '%s', disp_uc_priv=%p\n", disp->name, disp_uc_plat);
		if (display_in_use(disp)) {
			dev_warn(dev, "vp%d: %s already in use\n", port_id,
				 disp->name);
			return -EBUSY;
		}

		disp_uc_plat->source_id = vop_id;
		disp_uc_plat->src_dev = dev;

		ret = device_probe(disp);
		if (ret) {
			dev_err(dev, "vp%d: %s probe failed: %d\n", port_id,
				disp->name, ret);
			return ret;
		}

		ret = display_read_timing(disp, &timing);
		if (ret) {
			dev_err(dev, "vp%d: failed to read timings: %d\n",
				port_id, ret);
			return ret;
		}
	}

	rk_vop2_clamp_timing(dev, &timing);

	dev_info(dev, "vp%d: %ux%u @ %u Hz (%s)\n", port_id,
		 timing.hactive.typ, timing.vactive.typ,
		 timing.pixelclock.typ, compat);

	/* Set clock rate on video port to display timings */
	ret = clk_set_rate(&dclk, timing.pixelclock.typ);
	if (ret < 0) {
		dev_err(dev, "Failed to set clock rate: %d\n", ret);
		return ret;
	}

	dev_info(dev, "vp%d: %s = %lu Hz (requested %u)\n", port_id,
		 dclk_name, clk_get_rate(&dclk), timing.pixelclock.typ);

	/* Set bitwidth for vop display according to vop mode */
	switch (vop_id) {
	case VOP_MODE_EDP:
	case VOP_MODE_MIPI:
	case VOP_MODE_MIPI1:
	case VOP_MODE_HDMI:
	case VOP_MODE_DP:
	case VOP_MODE_LVDS:
		l2bpp = VIDEO_BPP32;
		break;
	default:
		l2bpp = VIDEO_BPP16;
	}

	/*
	 * We'll use the default platform-specific win_id from Linux
	 * so that Linux can take over U-Boot plane when Linux reconfigures
	 * VOP2.
	 */
	win_id = platdata->vp_lyr[port_id];
	if (win_id < 0) {
		dev_err(dev, "win_id is null, don't setup\n");
		return -EINVAL;
	}

	ret = rkvop2_enable(dev, fbbase, 1 << l2bpp, &timing, port_id, win_id, platdata);
	if (ret < 0)
		return ret;

	rkvop2_mode_set(dev, &timing, vop_id, port_id, platdata);

	if (bridge) {
		/* Attach the DSI controller and the display to the bridge. */
		ret = video_bridge_set_backlight(bridge, 60);
		if (ret) {
			dev_err(dev, "Failed to start the video bridge: %d\n", ret);
			return ret;
		}
	} else {
		ret = display_enable(disp, 1 << l2bpp, &timing);
		if (ret)
			return ret;
	}

	uc_priv->xsize = timing.hactive.typ;
	uc_priv->ysize = timing.vactive.typ;
	uc_priv->bpix = l2bpp;

	priv->vp = port_id;

	debug("fb=%lx, size=%d %d\n", fbbase,
	      uc_priv->xsize, uc_priv->ysize);

	return 0;
}

static ofnode rk_vop2_find_vp_node(struct udevice *dev, u32 vp)
{
	ofnode ports = dev_read_subnode(dev, "ports");
	ofnode node;

	for (node = ofnode_first_subnode(ports); ofnode_valid(node);
	     node = dev_read_next_subnode(node)) {
		if (ofnode_read_u32_default(node, "reg", -1) == vp)
			return node;
	}

	return ofnode_null();
}

int rk_vop2_probe(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct rk_vop2_priv *priv = dev_get_priv(dev);
	int prio[4];
	int nprio = 0, psize, i;
	int ret = 0;
	ofnode port, node;

	/* Before relocation we don't need to do anything */
	if (!(gd->flags & GD_FLG_RELOC))
		return 0;

	if (IS_ENABLED(CONFIG_EFI_LOADER)) {
		debug("Adding to EFI map %d @ %lx\n", plat->size, plat->base);
		efi_add_memory_map(plat->base, plat->size, EFI_RESERVED_MEMORY_TYPE);
	}

	priv->regs = dev_read_addr_ptr(dev);
	dev_info(dev, "framebuffer 0x%lx-0x%lx (%u KiB)\n", plat->base,
		 plat->base + plat->size, plat->size >> 10);

	/* Try all the ports until we find one that works. */
	port = dev_read_subnode(dev, "ports");
	if (!ofnode_valid(port)) {
		debug("%s(%s): 'port' subnode not found\n",
		      __func__, dev_read_name(dev));
		return -EINVAL;
	}

	/*
	 * Optional board policy: rockchip,vp-priority = <vp_index ...>;
	 * When present, try the listed video ports in that order instead of
	 * the device-tree order. Absent property keeps the upstream behavior.
	 */
	psize = dev_read_size(dev, "rockchip,vp-priority");
	if (psize > 0) {
		nprio = psize / sizeof(u32);
		if (nprio < 1 || nprio > (int)ARRAY_SIZE(prio) ||
		    dev_read_u32_array(dev, "rockchip,vp-priority", prio, nprio)) {
			debug("%s(%s): invalid rockchip,vp-priority\n",
			      __func__, dev_read_name(dev));
			nprio = 0;
		}
	}

	if (nprio) {
		for (i = 0; i < nprio; i++) {
			node = rk_vop2_find_vp_node(dev, prio[i]);
			if (!ofnode_valid(node)) {
				debug("%s(%s): vp%d not found\n", __func__,
				      dev_read_name(dev), prio[i]);
				continue;
			}
			ret = rk_display_init(dev, plat->base, node);
			if (ret)
				debug("Device failed: ret=%d\n", ret);
			if (!ret)
				break;
		}
	} else {
		for (node = ofnode_first_subnode(port);
		     ofnode_valid(node);
		     node = dev_read_next_subnode(node)) {
			ret = rk_display_init(dev, plat->base, node);
			if (ret)
				debug("Device failed: ret=%d\n", ret);
			if (!ret)
				break;
		}
	}
	video_set_flush_dcache(dev, true);

	return ret;
}

int rk_vop2_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);

	plat->size = 4 * (CONFIG_VIDEO_ROCKCHIP_MAX_XRES *
			CONFIG_VIDEO_ROCKCHIP_MAX_YRES);

	return 0;
}
