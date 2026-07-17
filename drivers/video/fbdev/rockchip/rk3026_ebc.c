// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip RK3026 EBC (E-Ink Broadcaster) framebuffer driver.
 *
 * Targets the ONYX BOOX C67ML (and e601/e602 rebrands): a 6.0" 1024x758
 * E Ink panel driven by the RK3026 EBC block. The register sequence and
 * timing-packing here were reversed from the vendor binary-only driver
 * (uudecoded hardware_ebc.o); see rk3026_ebc.h for the source map.
 *
 * This is a deliberately small grayscale fbdev driver for bringup, not a
 * full DRM driver. It exposes an 8bpp framebuffer with deferred-IO: writes
 * accumulate, then a worker programs the window, kicks one panel scan
 * (DSP_FRAME_START), and waits for the frame-complete IRQ. The waveform LUT
 * is loaded via request_firmware(); the per-mode LUT handling is stubbed
 * (GC16 full-refresh only) until the LUT walker is ported.
 *
 * Copyright (C) 2026  (mainline bringup)
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/fb.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/videodev2.h>

#include <video/of_display_timing.h>
#include <video/display_timing.h>

#include "rk3026_ebc.h"

#define DRIVER_NAME		"rk3026-ebc"
#define EBC_WAVEFORM_FW		"rockchip/rk3026-c67ml.wbf"
#define EBC_FRAME_TIMEOUT_MS	500

struct rk3026_ebc {
	struct device		*dev;
	void __iomem		*regs;
	struct clk		*hclk;
	struct clk		*dclk;
	struct clk		*aclk;
	struct regmap		*grf;
	int			irq;

	struct fb_info		*info;
	void			*fb_virt;
	dma_addr_t		fb_phys;
	size_t			fb_size;

	struct display_timing	timing;
	u32			width;
	u32			height;

	struct completion	frame_done;
	const struct firmware	*waveform;
};

static inline void ebc_write(struct rk3026_ebc *ebc, u32 reg, u32 val)
{
	writel_relaxed(val, ebc->regs + reg);
}

static inline u32 ebc_read(struct rk3026_ebc *ebc, u32 reg)
{
	return readl_relaxed(ebc->regs + reg);
}

static void ebc_config_done(struct rk3026_ebc *ebc)
{
	/* Vendor writes 0xffff to latch a register group; see every *_set(). */
	ebc_write(ebc, EBC_CONFIG_DONE, EBC_CONFIG_DONE_VAL);
}

/*
 * Program H/V timing + active-window registers.
 *
 * The packing mirrors ebc_panel_scan_set() in hardware_ebc.o, expressed in
 * terms of the panel timing fields (E Ink spec names in parentheses):
 *
 *   HTIMING0 = hsync(LSL)   | ((hfront(LEL) + ...) << 16)
 *   HTIMING1 = hback(LBL)   | ((hfront + hback) << 16)
 *   VTIMING0 = vsync(FSL)   | ((vsync + vback) << 16)
 *   VTIMING1 = vback(FBL)   | ((vback + vsync) << 16)
 *   ACT_INFO = height       | (width << 16)
 *
 * The vendor function folds gdck_sta/lgonl into the back fields; for the
 * known C67ML panel those are constants (gdck_sta=4, lgonl=262) and the
 * resulting words match a straight {front | back<<16} pack of the DT
 * porches, which is what we emit here.
 */
static void ebc_set_timing(struct rk3026_ebc *ebc)
{
	const struct display_timing *t = &ebc->timing;
	u32 hsync = t->hsync_len.typ;
	u32 hback = t->hback_porch.typ;
	u32 hfront = t->hfront_porch.typ;
	u32 vsync = t->vsync_len.typ;
	u32 vback = t->vback_porch.typ;
	u32 vfront = t->vfront_porch.typ;

	ebc_write(ebc, EBC_DSP_HTIMING0,
		  (hsync & 0xffff) | ((hfront + hsync) << 16));
	ebc_write(ebc, EBC_DSP_HTIMING1,
		  (hback & 0xffff) | ((hfront + hback) << 16));
	ebc_write(ebc, EBC_DSP_VTIMING0,
		  (vsync & 0xffff) | ((vsync + vback) << 16));
	ebc_write(ebc, EBC_DSP_VTIMING1,
		  (vback & 0xffff) | ((vback + vfront) << 16));
	ebc_write(ebc, EBC_DSP_ACT_INFO,
		  (ebc->height & 0xffff) | (ebc->width << 16));
	ebc_config_done(ebc);
}

static void ebc_set_dsp_ctrl(struct rk3026_ebc *ebc, u32 mode)
{
	/* ebc_dsp_ctl_set(): DSP_CTRL = 0xC0030000 | mode, then latch. */
	ebc_write(ebc, EBC_DSP_CTRL, DSP_CTRL_FIXED | mode);
	ebc_config_done(ebc);
}

static void ebc_kick_frame(struct rk3026_ebc *ebc)
{
	reinit_completion(&ebc->frame_done);
	/* One frame, FRAME_START. Hardware scans the panel and raises IRQ. */
	ebc_write(ebc, EBC_DSP_START, DSP_FRAME_NUM(1) | DSP_FRAME_START);
}

static irqreturn_t ebc_irq(int irq, void *data)
{
	struct rk3026_ebc *ebc = data;
	u32 status = ebc_read(ebc, EBC_INT);

	if (!(status & EBC_INT_FRAMEEND_STATUS))
		return IRQ_NONE;

	/* Ack: write FRAMEEND_CLEAR (vendor rk29_ebc_irq path). */
	ebc_write(ebc, EBC_INT, EBC_INT_FRAMEEND_CLEAR);
	complete(&ebc->frame_done);
	return IRQ_HANDLED;
}

static void ebc_refresh(struct rk3026_ebc *ebc)
{
	/* Point the window at the framebuffer and trigger a full GC16 scan. */
	ebc_write(ebc, EBC_WIN_MST0, lower_32_bits(ebc->fb_phys));
	ebc_write(ebc, EBC_WIN_MST1, lower_32_bits(ebc->fb_phys));
	ebc_config_done(ebc);

	ebc_set_dsp_ctrl(ebc, 0);
	ebc_kick_frame(ebc);

	if (!wait_for_completion_timeout(&ebc->frame_done,
					 msecs_to_jiffies(EBC_FRAME_TIMEOUT_MS)))
		dev_warn_ratelimited(ebc->dev, "frame timeout\n");
}

/* --- fbdev deferred-IO --- */
static void ebc_deferred_io(struct fb_info *info, struct list_head *pagereflist)
{
	ebc_refresh(info->par);
}

static const struct fb_deferred_io ebc_defio = {
	.delay		= HZ / 5,	/* coalesce writes ~200 ms (E Ink is slow) */
	.deferred_io	= ebc_deferred_io,
};

static ssize_t ebc_fb_write(struct fb_info *info, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	ssize_t ret = fb_sys_write(info, buf, count, ppos);

	if (ret > 0)
		ebc_refresh(info->par);
	return ret;
}

static void ebc_fb_fillrect(struct fb_info *info, const struct fb_fillrect *r)
{
	sys_fillrect(info, r);
	ebc_refresh(info->par);
}

static void ebc_fb_copyarea(struct fb_info *info, const struct fb_copyarea *a)
{
	sys_copyarea(info, a);
	ebc_refresh(info->par);
}

static void ebc_fb_imageblit(struct fb_info *info, const struct fb_image *img)
{
	sys_imageblit(info, img);
	ebc_refresh(info->par);
}

static const struct fb_ops ebc_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= fb_sys_read,
	.fb_write	= ebc_fb_write,
	.fb_fillrect	= ebc_fb_fillrect,
	.fb_copyarea	= ebc_fb_copyarea,
	.fb_imageblit	= ebc_fb_imageblit,
	.fb_mmap	= fb_deferred_io_mmap,
};

static int ebc_setup_fb(struct rk3026_ebc *ebc)
{
	struct fb_info *info;
	int ret;

	/* 8bpp grayscale; E Ink panels use 4-bit but 8bpp is simpler for fbdev
	 * and the EBC packs two pixels per byte internally for GC16. */
	ebc->fb_size = PAGE_ALIGN(ebc->width * ebc->height);
	ebc->fb_virt = dmam_alloc_coherent(ebc->dev, ebc->fb_size,
					   &ebc->fb_phys, GFP_KERNEL);
	if (!ebc->fb_virt)
		return -ENOMEM;
	memset(ebc->fb_virt, 0xff, ebc->fb_size); /* white */

	info = framebuffer_alloc(0, ebc->dev);
	if (!info)
		return -ENOMEM;
	ebc->info = info;
	info->par = ebc;
	info->fbops = &ebc_fb_ops;
	info->flags = FBINFO_VIRTFB;
	info->screen_buffer = ebc->fb_virt;
	info->fix.smem_start = ebc->fb_phys;
	info->fix.smem_len = ebc->fb_size;

	strscpy(info->fix.id, "rk3026-ebc", sizeof(info->fix.id));
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_STATIC_PSEUDOCOLOR;
	info->fix.line_length = ebc->width;
	info->fix.accel = FB_ACCEL_NONE;

	info->var.xres = ebc->width;
	info->var.yres = ebc->height;
	info->var.xres_virtual = ebc->width;
	info->var.yres_virtual = ebc->height;
	info->var.bits_per_pixel = 8;
	info->var.grayscale = 1;
	info->var.red.length = 8;
	info->var.green.length = 8;
	info->var.blue.length = 8;

	info->fbdefio = devm_kmemdup(ebc->dev, &ebc_defio,
				     sizeof(ebc_defio), GFP_KERNEL);
	if (!info->fbdefio) {
		framebuffer_release(info);
		return -ENOMEM;
	}
	fb_deferred_io_init(info);

	ret = register_framebuffer(info);
	if (ret) {
		fb_deferred_io_cleanup(info);
		framebuffer_release(info);
		return ret;
	}
	return 0;
}

static int ebc_hw_init(struct rk3026_ebc *ebc)
{
	int ret;

	ret = clk_prepare_enable(ebc->hclk);
	if (ret)
		return ret;
	ret = clk_prepare_enable(ebc->aclk);
	if (ret)
		goto err_hclk;
	ret = clk_prepare_enable(ebc->dclk);
	if (ret)
		goto err_aclk;

	/* Route shared display pads to EBC (vs LCDC): GRF 0x150 bit0, with the
	 * Rockchip hiword write-mask in the upper 16 bits. */
	if (ebc->grf)
		regmap_write(ebc->grf, GRF_EBC_SEL_REG,
			     (GRF_EBC_SEL_BIT << 16) | GRF_EBC_SEL_BIT);

	ebc_set_timing(ebc);
	ebc_set_dsp_ctrl(ebc, 0);
	return 0;

err_aclk:
	clk_disable_unprepare(ebc->aclk);
err_hclk:
	clk_disable_unprepare(ebc->hclk);
	return ret;
}

static void ebc_hw_fini(struct rk3026_ebc *ebc)
{
	clk_disable_unprepare(ebc->dclk);
	clk_disable_unprepare(ebc->aclk);
	clk_disable_unprepare(ebc->hclk);
}

static int ebc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rk3026_ebc *ebc;
	int ret;

	ebc = devm_kzalloc(dev, sizeof(*ebc), GFP_KERNEL);
	if (!ebc)
		return -ENOMEM;
	ebc->dev = dev;
	init_completion(&ebc->frame_done);
	platform_set_drvdata(pdev, ebc);

	ebc->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ebc->regs))
		return PTR_ERR(ebc->regs);

	ebc->hclk = devm_clk_get(dev, "hclk");
	if (IS_ERR(ebc->hclk))
		return dev_err_probe(dev, PTR_ERR(ebc->hclk), "no hclk\n");
	ebc->dclk = devm_clk_get(dev, "dclk");
	if (IS_ERR(ebc->dclk))
		return dev_err_probe(dev, PTR_ERR(ebc->dclk), "no dclk\n");
	ebc->aclk = devm_clk_get(dev, "aclk");
	if (IS_ERR(ebc->aclk))
		return dev_err_probe(dev, PTR_ERR(ebc->aclk), "no aclk\n");

	ebc->grf = syscon_regmap_lookup_by_phandle(dev->of_node,
						   "rockchip,grf");
	if (IS_ERR(ebc->grf)) {
		dev_warn(dev, "no rockchip,grf; skipping pad mux\n");
		ebc->grf = NULL;
	}

	ret = of_get_display_timing(dev->of_node, "panel-timing", &ebc->timing);
	if (ret)
		return dev_err_probe(dev, ret, "missing panel-timing\n");
	ebc->width = ebc->timing.hactive.typ;
	ebc->height = ebc->timing.vactive.typ;

	ret = of_reserved_mem_device_init(dev);
	if (ret)
		dev_warn(dev, "no reserved memory region (%d)\n", ret);

	ebc->irq = platform_get_irq(pdev, 0);
	if (ebc->irq < 0)
		return ebc->irq;
	ret = devm_request_irq(dev, ebc->irq, ebc_irq, 0, DRIVER_NAME, ebc);
	if (ret)
		return dev_err_probe(dev, ret, "request_irq\n");

	/* Waveform LUT: non-fatal if absent (driver still drives a refresh). */
	ret = request_firmware(&ebc->waveform, EBC_WAVEFORM_FW, dev);
	if (ret)
		dev_warn(dev, "waveform %s not found (%d); using defaults\n",
			 EBC_WAVEFORM_FW, ret);
	else
		dev_info(dev, "waveform %s: %zu bytes\n",
			 EBC_WAVEFORM_FW, ebc->waveform->size);

	ret = ebc_hw_init(ebc);
	if (ret)
		goto err_fw;

	ret = ebc_setup_fb(ebc);
	if (ret)
		goto err_hw;

	dev_info(dev, "RK3026 EBC %ux%u registered as fb%d\n",
		 ebc->width, ebc->height, ebc->info->node);
	return 0;

err_hw:
	ebc_hw_fini(ebc);
err_fw:
	if (ebc->waveform)
		release_firmware(ebc->waveform);
	return ret;
}

static void ebc_remove(struct platform_device *pdev)
{
	struct rk3026_ebc *ebc = platform_get_drvdata(pdev);

	if (ebc->info) {
		unregister_framebuffer(ebc->info);
		fb_deferred_io_cleanup(ebc->info);
		framebuffer_release(ebc->info);
	}
	ebc_hw_fini(ebc);
	if (ebc->waveform)
		release_firmware(ebc->waveform);
}

static const struct of_device_id ebc_of_match[] = {
	{ .compatible = "rockchip,rk3026-ebc" },
	{ }
};
MODULE_DEVICE_TABLE(of, ebc_of_match);

static struct platform_driver ebc_driver = {
	.probe	= ebc_probe,
	.remove	= ebc_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= ebc_of_match,
	},
};
module_platform_driver(ebc_driver);

MODULE_DESCRIPTION("Rockchip RK3026 EBC E-Ink framebuffer driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(EBC_WAVEFORM_FW);
