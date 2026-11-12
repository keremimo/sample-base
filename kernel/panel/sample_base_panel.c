// SPDX-License-Identifier: GPL-2.0+
/*
 * sample-base's driver for the MPC Sample's panel: a Vitek VT024K-ZN01 on a
 * Sitronix ST7796S over SPI. After inMusic's st7796s.c (John Keeping, in the
 * MPC's kernel), with the same power-up and init, but in the panel's own
 * portrait orientation: 240 x 320, the address mode without the row/column
 * exchange (MV). The panel refreshes from its memory a gate line at a time;
 * here a write runs along the gate lines too, so the refresh can never meet a
 * line of the picture half written across it (with MV, as the stock driver
 * runs it turned to landscape, a write crosses every gate line it touches).
 *
 * It never binds by itself: no device table. sample-base loads it and binds
 * spi3.0 to it by driver_override while it runs (src/drivers/panel_driver.h),
 * turning its landscape picture into this portrait one, and gives the device
 * back to st7796s afterwards. No fbdev.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>
#include <video/mipi_display.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mipi_dbi.h>

#define ST7796S_FRMCTR2		0xb2
#define ST7796S_EM		0xb7
#define ST7796S_PWCTR3		0xc2
#define ST7796S_GAMCTRP1	0xe0
#define ST7796S_GAMCTRN1	0xe1

#define ST7796S_MY	BIT(7)
#define ST7796S_MX	BIT(6)
#define ST7796S_MV	BIT(5)

/* The stock driver's landscape mode is MY | MV; without MV the picture is the panel's portrait one. */
static ushort madctl = ST7796S_MY;
module_param(madctl, ushort, 0644);
MODULE_PARM_DESC(madctl, "Address mode, MV always clear (default 0x80: MY)");

static const struct drm_display_mode panel_mode = { DRM_SIMPLE_MODE(240, 320, 37, 49) };

static void panel_pipe_enable(struct drm_simple_display_pipe *pipe,
			      struct drm_crtc_state *crtc_state,
			      struct drm_plane_state *plane_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct mipi_dbi *dbi = &dbidev->dbi;
	int ret, idx;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	ret = mipi_dbi_poweron_reset(dbidev);
	if (ret)
		goto out_exit;

	msleep(150);

	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(120);

	mipi_dbi_command(dbi, 0xe7, 0x00);
	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, (u8)(madctl & ~ST7796S_MV & 0xfc));
	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT,
			 MIPI_DCS_PIXEL_FMT_16BIT);

	mipi_dbi_command(dbi, ST7796S_FRMCTR2, 0x0c, 0x0c, 0x00, 0x33, 0x33);
	mipi_dbi_command(dbi, ST7796S_EM, 0x35);
	mipi_dbi_command(dbi, 0xbb, 0x32);
	mipi_dbi_command(dbi, ST7796S_PWCTR3, 0x01);
	mipi_dbi_command(dbi, 0xc3, 0x19);
	mipi_dbi_command(dbi, 0xc4, 0x20);
	mipi_dbi_command(dbi, 0xc6, 0x0f);
	mipi_dbi_command(dbi, 0xd0, 0xa4, 0xa1);

	mipi_dbi_command(dbi, ST7796S_GAMCTRP1, 0xd0, 0x08, 0x0e, 0x09, 0x09,
			 0x05, 0x31, 0x33, 0x48, 0x17, 0x14, 0x15, 0x31, 0x34);
	mipi_dbi_command(dbi, ST7796S_GAMCTRN1, 0xd0, 0x08, 0x0e, 0x09, 0x09,
			 0x15, 0x31, 0x33, 0x48, 0x17, 0x14, 0x15, 0x31, 0x34);
	mipi_dbi_command(dbi, MIPI_DCS_ENTER_INVERT_MODE);

	mipi_dbi_command(dbi, MIPI_DCS_ENTER_NORMAL_MODE);

	mipi_dbi_enable_tail(dbidev, crtc_state, plane_state, 10);
out_exit:
	drm_dev_exit(idx);
}

static const struct drm_simple_display_pipe_funcs panel_pipe_funcs = {
	DRM_MIPI_DBI_SIMPLE_DISPLAY_PIPE_FUNCS(panel_pipe_enable),
};

DEFINE_DRM_GEM_DMA_FOPS(panel_fops);

static const struct drm_driver panel_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &panel_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "sample-base-panel",
	.desc			= "MPC Sample panel, portrait",
	.date			= "20261112",
	.major			= 1,
	.minor			= 0,
};

static int panel_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct mipi_dbi_dev *dbidev;
	struct drm_device *drm;
	struct mipi_dbi *dbi;
	struct gpio_desc *dc;
	int ret;

	dbidev = devm_drm_dev_alloc(dev, &panel_driver, struct mipi_dbi_dev, drm);
	if (IS_ERR(dbidev))
		return PTR_ERR(dbidev);

	dbi = &dbidev->dbi;
	drm = &dbidev->drm;

	dbi->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(dbi->reset))
		return dev_err_probe(dev, PTR_ERR(dbi->reset), "Failed to get GPIO 'reset'\n");

	dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc))
		return dev_err_probe(dev, PTR_ERR(dc), "Failed to get GPIO 'dc'\n");

	dbidev->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(dbidev->backlight))
		return PTR_ERR(dbidev->backlight);

	ret = mipi_dbi_spi_init(spi, dbi, dc);
	if (ret)
		return ret;

	ret = mipi_dbi_dev_init(dbidev, &panel_pipe_funcs, &panel_mode, 0);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	spi_set_drvdata(spi, drm);
	return 0;
}

static void panel_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static void panel_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver panel_spi_driver = {
	.driver = {
		.name = "sample-base-panel",
	},
	.probe = panel_probe,
	.remove = panel_remove,
	.shutdown = panel_shutdown,
};
module_spi_driver(panel_spi_driver);

MODULE_DESCRIPTION("MPC Sample panel (ST7796S) in its own portrait orientation, for sample-base");
MODULE_LICENSE("GPL");
