// SPDX-License-Identifier: GPL-2.0
/*
 * mxcfb ioctl shim over the RK3026 EBC's fbdev emulation.
 *
 * DRM fbdev emulation delivers damage rectangles but has no way to express a
 * waveform mode, a temperature, an update marker or a completion wait. Every
 * shipping e-reader userspace - KOReader included - talks the i.MX EPDC ioctl
 * dialect instead, so this file bolts that dialect onto the emulated fbdev and
 * translates it into the driver's internal update API.
 *
 * It is deliberately a leaf: nothing in the DRM driver depends on it, and
 * building without it leaves a working single-mode partial-refresh display.
 *
 * Copyright (C) 2026
 */

#include <linux/fb.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <drm/drm_fb_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_rect.h>

#include "rk3026_ebc.h"
#include "rk3026_ebc_mxcfb.h"

/*
 * Waveform mode mapping.
 *
 * The rkf container this panel ships carries five LUT families: reset,
 * gray16, "mode 6", gray2 and grayA2. The EBC work modes below select them
 * through the controller's own dispatch, which also decides whether the
 * refresh is full-class or partial-class.
 *
 *   INIT    -> EBC_MODE_RESET       reset LUT, full class
 *   DU      -> EBC_MODE_BLACK_WHITE gray2 LUT, partial class
 *   A2      -> EBC_MODE_A2          grayA2 LUT, partial class
 *   GC16    -> EBC_MODE_FULL/PART   gray16 LUT, by update_mode
 *   GL16    -> EBC_MODE_PART        gray16 LUT, partial class
 *   REAGL   -> EBC_MODE_OED_PART    "mode 6" LUT, full class
 *   REAGLD  -> EBC_MODE_FULL        gray16 LUT, full class
 *   GC4     -> EBC_MODE_PART        gray16 LUT; the panel has no 4-level LUT
 *   AUTO    -> EBC_MODE_FULL/PART   gray16 LUT, by update_mode
 *
 * GC4 and REAGLD have no distinct waveform on this panel and are folded into
 * the gray16 family; GL16 likewise, the container has no separate 16-level
 * "no-flash" table. REAGL is mapped onto the container's mode-6 family, which
 * is the only other 16-level table present - that pairing is this driver's
 * choice, not something the vendor code states.
 *
 * AUTO is deliberately not routed to EBC_MODE_AUTO: that work mode is one of
 * the controller's direct modes, driven line by line with no LUT upload and
 * with DISPLAY_MODE clear, which is a different data path entirely. Treating
 * AUTO as GC16 keeps every update on the LUT path.
 */
static u32 mxcfb_to_ebc_mode(u32 waveform_mode, u32 update_mode)
{
	switch (waveform_mode) {
	case MXCFB_WAVEFORM_MODE_INIT:
		return EBC_MODE_RESET;
	case MXCFB_WAVEFORM_MODE_DU:
		return EBC_MODE_BLACK_WHITE;
	case MXCFB_WAVEFORM_MODE_A2:
		return EBC_MODE_A2;
	case MXCFB_WAVEFORM_MODE_REAGL:
		return EBC_MODE_OED_PART;
	case MXCFB_WAVEFORM_MODE_REAGLD:
		return EBC_MODE_FULL;
	case MXCFB_WAVEFORM_MODE_GL16:
	case MXCFB_WAVEFORM_MODE_GC4:
		return EBC_MODE_PART;
	case MXCFB_WAVEFORM_MODE_GC16:
	case MXCFB_WAVEFORM_MODE_AUTO:
	default:
		return update_mode == MXCFB_UPDATE_MODE_FULL ?
			EBC_MODE_FULL : EBC_MODE_PART;
	}
}

static int mxcfb_send_update(struct rk3026_ebc *ebc, struct fb_info *info,
			     unsigned long arg)
{
	struct mxcfb_update_data upd;
	struct drm_rect clip;
	bool temp_valid;
	u32 mode;

	if (copy_from_user(&upd, (void __user *)arg, sizeof(upd)))
		return -EFAULT;

	clip.x1 = min_t(u32, upd.update_region.left, ebc->width);
	clip.y1 = min_t(u32, upd.update_region.top, ebc->height);
	clip.x2 = min_t(u32, clip.x1 + upd.update_region.width, ebc->width);
	clip.y2 = min_t(u32, clip.y1 + upd.update_region.height, ebc->height);
	if (clip.x1 >= clip.x2 || clip.y1 >= clip.y2)
		return -EINVAL;

	mode = mxcfb_to_ebc_mode(upd.waveform_mode, upd.update_mode);
	temp_valid = upd.temp != MXCFB_TEMP_USE_AMBIENT &&
		     upd.temp > -128 && upd.temp < 128;

	/*
	 * From here on this client owns the pixels: it writes them through the
	 * mmap and tells us when they are ready, so the deferred-IO path must
	 * not refresh the same region again with the default waveform.
	 */
	ebc->mxcfb_active = true;

	mutex_lock(&ebc->blit_lock);
	rk3026_ebc_blit_xrgb8888(ebc, info->screen_buffer,
				 info->fix.line_length, &clip);
	mutex_unlock(&ebc->blit_lock);

	rk3026_ebc_queue_update(ebc, &clip, mode, upd.temp, temp_valid,
				upd.update_marker);
	return 0;
}

static int mxcfb_wait_for_update(struct rk3026_ebc *ebc, unsigned long arg)
{
	u32 marker;

	if (copy_from_user(&marker, (void __user *)arg, sizeof(marker)))
		return -EFAULT;

	return rk3026_ebc_wait_marker(ebc, marker);
}

static int ebc_fb_ioctl(struct fb_info *info, unsigned int cmd,
			unsigned long arg)
{
	struct drm_fb_helper *helper = info->par;
	struct rk3026_ebc *ebc = drm_to_ebc(helper->dev);

	switch (cmd) {
	case MXCFB_SEND_UPDATE:
		return mxcfb_send_update(ebc, info, arg);
	case MXCFB_WAIT_FOR_UPDATE_COMPLETE:
		return mxcfb_wait_for_update(ebc, arg);
	}

	if (ebc->orig_fbops->fb_ioctl)
		return ebc->orig_fbops->fb_ioctl(info, cmd, arg);
	return -ENOTTY;
}

static int ebc_fb_open(struct fb_info *info, int user)
{
	struct drm_fb_helper *helper = info->par;
	struct rk3026_ebc *ebc = drm_to_ebc(helper->dev);
	int ret = 0;

	if (ebc->orig_fbops->fb_open)
		ret = ebc->orig_fbops->fb_open(info, user);
	if (!ret && user)
		atomic_inc(&ebc->fb_users);
	return ret;
}

static int ebc_fb_release(struct fb_info *info, int user)
{
	struct drm_fb_helper *helper = info->par;
	struct rk3026_ebc *ebc = drm_to_ebc(helper->dev);

	/* Hand the panel back to fbcon once the last mxcfb client is gone. */
	if (user && atomic_dec_and_test(&ebc->fb_users))
		ebc->mxcfb_active = false;

	if (ebc->orig_fbops->fb_release)
		return ebc->orig_fbops->fb_release(info, user);
	return 0;
}

/**
 * rk3026_ebc_mxcfb_install - graft the mxcfb ioctls onto an emulated fbdev
 * @ebc: controller
 * @info: fb_info the DRM fbdev client has just set up
 *
 * The generic fb_ops is const and shared, so it is copied once and the three
 * entry points this shim needs are replaced in the copy.
 */
void rk3026_ebc_mxcfb_install(struct rk3026_ebc *ebc, struct fb_info *info)
{
	struct fb_ops *ops;

	if (!info || !info->fbops)
		return;

	ops = devm_kmemdup(ebc->dev, info->fbops, sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return;

	ebc->orig_fbops = info->fbops;
	ops->fb_ioctl = ebc_fb_ioctl;
	ops->fb_open = ebc_fb_open;
	ops->fb_release = ebc_fb_release;
	ebc->fbops = ops;
	info->fbops = ops;

	drm_info(&ebc->drm, "mxcfb V1 ioctl shim on fb%d\n", info->node);
}
