// SPDX-License-Identifier: GPL-2.0+
/*
 * Raspberry Pi 7" touchscreen panel (tc358762 DSI->DPI bridge)
 *
 * Ported from the Linux driver:
 *   drivers/gpu/drm/panel/panel-raspberrypi-touchscreen.c
 *
 * The panel is a 800x480 DSI display using a single data lane.  The bridge
 * chip (tc358762) is configured over DSI generic writes, and the backlight is
 * driven by an ATtiny on the panel board through I2C (address 0x45).
 *
 * Touch is intentionally not implemented; it is not needed before the OS.
 */

#include <dm.h>
#include <dm/device_compat.h>
#include <i2c.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <mipi_dsi.h>
#include <panel.h>

/* ATtiny I2C registers */
#define RPI_REG_ID		0x80
#define RPI_REG_PORTA		0x81
#define RPI_REG_PORTB		0x82
#define RPI_REG_POWERON		0x85
#define RPI_REG_PWM		0x86

/* Known ATtiny firmware revisions */
#define RPI_ID_V1		0xde
#define RPI_ID_V2		0xc3

/* DSI PPI layer registers of the tc358762 */
#define PPI_STARTPPI		0x0104
#define PPI_LPTXTIMECNT		0x0114
#define PPI_D0S_ATMR		0x0144
#define PPI_D1S_ATMR		0x0148
#define PPI_D0S_CLRSIPOCOUNT	0x0164
#define PPI_D1S_CLRSIPOCOUNT	0x0168

/* DSI protocol layer registers */
#define DSI_STARTDSI		0x0204
#define DSI_LANEENABLE		0x0210

/* LCDC/DPI host registers */
#define LCDCTRL			0x0420

/* SPI master registers */
#define SPICMR			0x0450

/* System controller registers */
#define SYSCTRL			0x0464

#define DSI_LANEENABLE_CLOCK	BIT(0)
#define DSI_LANEENABLE_D0	BIT(1)

/* Matches the mode used by the Linux driver and the Raspberry Pi firmware */
static const struct display_timing rpi_panel_timing = {
	.pixelclock.typ		= 25979400,
	.hactive.typ		= 800,
	.hfront_porch.typ	= 1,
	.hback_porch.typ	= 1,
	.hsync_len.typ		= 2,
	.vactive.typ		= 480,
	.vfront_porch.typ	= 7,
	.vback_porch.typ	= 21,
	.vsync_len.typ		= 2,
};

static int rpi_panel_i2c_read(struct udevice *dev, u8 reg, u8 *val)
{
	return dm_i2c_read(dev, reg, val, 1);
}

static int rpi_panel_i2c_write(struct udevice *dev, u8 reg, u8 val)
{
	return dm_i2c_write(dev, reg, &val, 1);
}

/*
 * The tc358762 takes 16-bit register addresses with a 32-bit value.  Use the
 * regular mipi_dsi_generic_write() path so the transfer picks up the device's
 * mode flags exactly like the Linux panel driver does: this panel advertises
 * MIPI_DSI_MODE_LPM, so its register writes have to go out as low-power
 * command packets.
 */
/*
 * The tc358762 takes 16-bit register addresses with a 32-bit value.  Use the
 * same generic write path as the Linux panel driver, which sends these in LP
 * mode (the device advertises MIPI_DSI_MODE_LPM).  The bridge only accepts its
 * bring-up commands on the low-power path.
 */
static int rpi_panel_dsi_write(struct mipi_dsi_device *dsi, u16 reg, u32 val)
{
	u8 payload[] = {
		reg, reg >> 8,
		val, val >> 8, val >> 16, val >> 24,
	};
	ssize_t ret;

	ret = mipi_dsi_generic_write(dsi, payload, sizeof(payload));
	if (ret < 0) {
		dev_err(dsi->dev, "DSI write of register 0x%04x failed: %d\n",
			reg, (int)ret);
		return ret;
	}

	return 0;
}

static int rpi_panel_get_display_timing(struct udevice *dev,
					struct display_timing *timing)
{
	*timing = rpi_panel_timing;

	return 0;
}

static int rpi_panel_set_backlight(struct udevice *dev, int percent)
{
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;

	return rpi_panel_i2c_write(dev, RPI_REG_PWM, percent * 255 / 100);
}

static int rpi_panel_bridge_init(struct udevice *dev,
				  struct mipi_dsi_device *dsi)
{
	int ret;

	ret = rpi_panel_dsi_write(dsi, DSI_LANEENABLE,
				  DSI_LANEENABLE_CLOCK | DSI_LANEENABLE_D0);
	if (ret)
		return ret;

	ret = rpi_panel_dsi_write(dsi, PPI_D0S_CLRSIPOCOUNT, 0x05);
	if (ret)
		return ret;
	ret = rpi_panel_dsi_write(dsi, PPI_D1S_CLRSIPOCOUNT, 0x05);
	if (ret)
		return ret;
	ret = rpi_panel_dsi_write(dsi, PPI_D0S_ATMR, 0x00);
	if (ret)
		return ret;
	ret = rpi_panel_dsi_write(dsi, PPI_D1S_ATMR, 0x00);
	if (ret)
		return ret;
	ret = rpi_panel_dsi_write(dsi, PPI_LPTXTIMECNT, 0x03);
	if (ret)
		return ret;

	ret = rpi_panel_dsi_write(dsi, SPICMR, 0x00);
	if (ret)
		return ret;
	ret = rpi_panel_dsi_write(dsi, LCDCTRL, 0x00100150);
	if (ret)
		return ret;
	ret = rpi_panel_dsi_write(dsi, SYSCTRL, 0x040f);
	if (ret)
		return ret;
	mdelay(100);

	ret = rpi_panel_dsi_write(dsi, PPI_STARTPPI, 0x01);
	if (ret)
		return ret;
	ret = rpi_panel_dsi_write(dsi, DSI_STARTDSI, 0x01);
	if (ret)
		return ret;
	mdelay(100);

	return 0;
}

static int rpi_panel_enable_backlight(struct udevice *dev)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct mipi_dsi_device *dsi = plat->device;
	u8 portb = 0;
	int ret, i;

	if (!dsi)
		return -ENODEV;

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		dev_err(dev, "failed to attach DSI device: %d\n", ret);
		return ret;
	}

	ret = rpi_panel_i2c_write(dev, RPI_REG_POWERON, 1);
	if (ret) {
		dev_err(dev, "failed to power on panel: %d\n", ret);
		goto err_power_off;
	}

	/* Wait for nPWRDWN to go low to indicate power-on is done */
	for (i = 0; i < 100; i++) {
		ret = rpi_panel_i2c_read(dev, RPI_REG_PORTB, &portb);
		if (!ret && (portb & BIT(0)))
			break;
		mdelay(10);
	}
	if (i == 100) {
		dev_err(dev, "panel power-on timed out (PORTB=0x%02x)\n", portb);
		ret = -ETIMEDOUT;
		goto err_power_off;
	}

	ret = rpi_panel_bridge_init(dev, dsi);
	if (ret) {
		dev_err(dev, "tc358762 init failed: %d\n", ret);
		goto err_power_off;
	}

	/* Same orientation as the Raspberry Pi firmware / Linux driver */
	ret = rpi_panel_i2c_write(dev, RPI_REG_PORTA, BIT(2));
	if (ret)
		goto err_power_off;

	ret = rpi_panel_i2c_write(dev, RPI_REG_PWM, 0xff);
	if (ret)
		goto err_power_off;

	return 0;

err_power_off:
	rpi_panel_i2c_write(dev, RPI_REG_PWM, 0);
	rpi_panel_i2c_write(dev, RPI_REG_POWERON, 0);
	/* POWERON=0 powers the ATtiny/tc358762 down, which also resets the bridge */
	mdelay(10);

	return ret;
}

static int rpi_panel_probe(struct udevice *dev)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	u8 id;
	int ret;

	ret = rpi_panel_i2c_read(dev, RPI_REG_ID, &id);
	if (ret) {
		dev_err(dev, "ATtiny I2C read failed: %d\n", ret);
		return ret;
	}

	if (id != RPI_ID_V1 && id != RPI_ID_V2) {
		dev_err(dev, "unknown ATtiny firmware revision: 0x%02x\n", id);
		return -ENODEV;
	}

	plat->lanes = 1;
	plat->format = MIPI_DSI_FMT_RGB888;
	plat->mode_flags = MIPI_DSI_MODE_VIDEO |
			   MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			   MIPI_DSI_MODE_LPM;

	/* Keep the panel powered down until the display pipeline is ready */
	rpi_panel_i2c_write(dev, RPI_REG_POWERON, 0);

	return 0;
}

static const struct panel_ops rpi_panel_ops = {
	.enable_backlight = rpi_panel_enable_backlight,
	.set_backlight = rpi_panel_set_backlight,
	.get_display_timing = rpi_panel_get_display_timing,
};

static const struct udevice_id rpi_panel_ids[] = {
	{ .compatible = "raspberrypi,7inch-touchscreen-panel" },
	{ }
};

U_BOOT_DRIVER(rpi_7inch_panel) = {
	.name		= "rpi_7inch_panel",
	.id		= UCLASS_PANEL,
	.of_match	= rpi_panel_ids,
	.ops		= &rpi_panel_ops,
	.probe		= rpi_panel_probe,
	.plat_auto	= sizeof(struct mipi_dsi_panel_plat),
};