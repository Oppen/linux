// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip RK3026 EBC (E-Ink Broadcaster) DRM driver.
 *
 * Targets the ONYX BOOX C67ML and its rebrands: a 6.0" 1024x758 E Ink panel
 * (ED060XC3T1) driven by the RK3026 EBC block. Register offsets, timing
 * packing and the frame sequence were reversed from the vendor binary-only
 * driver; see rk3026_ebc.h for the map.
 *
 * The EBC scans a pair of packed 4bpp images - what is on the panel and what
 * is being driven towards - through a waveform LUT indexed by (old grey, new
 * grey). A refresh is tens to hundreds of frames at 85 Hz, so all of it runs
 * on a dedicated kthread: atomic commits and the frame-end interrupt only
 * hand work to it.
 *
 * Copyright (C) 2026
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/thermal.h>
#include <linux/unaligned.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include <video/of_display_timing.h>

#include "rk3026_ebc.h"

#define DRIVER_NAME		"rk3026-ebc"
#define EBC_WAVEFORM_FW		"rockchip/rk3026-c67ml.rkf"
/* A cold GC16 waveform is ~130 frames at 85 Hz; the vendor arms 3 s. */
/* DIAGNOSTIC, REVERT ME: was 3000. Splits "the frame never completes" from
 * "the frame is too slow for the budget" -- aclk_lcdc1 is 24 MHz off a
 * bypassed CPLL, and dclk_ebc landed on 66 MHz instead of the intended 80. */
#define EBC_FRAME_TIMEOUT_MS	60000
/* Longest a client may block in MXCFB_WAIT_FOR_UPDATE_COMPLETE. */
#define EBC_MARKER_TIMEOUT_MS	10000

/* Refresh mode used when nothing annotates the update. */
#define EBC_MODE_DEFAULT	EBC_MODE_PART

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
	ebc_write(ebc, EBC_CONFIG_DONE, EBC_CONFIG_DONE_VAL);
}

/*
 * DIAGNOSTIC, REVERT ME. The scan completes -- frame-end interrupts arrive and
 * no chunk times out -- while the glass does not move, so every stage between
 * the source image and the panel is reported as a positive marker rather than
 * inferred from the absence of an error. Reports the next N scans; defaults to
 * the first few so the fbcon updates at boot are covered without a shell, and
 * re-arms with
 *
 *	echo 4 > /sys/module/rk3026_ebc/parameters/debug_scan
 */
static unsigned int debug_scan = 6;
module_param(debug_scan, uint, 0644);
MODULE_PARM_DESC(debug_scan, "report every stage of the next N waveform scans (0 = off)");

static atomic_t dbg_irqs = ATOMIC_INIT(0);
static u32 dbg_last_status;

/*
 * DIAGNOSTIC, REVERT ME. Runtime knobs so one card can answer several questions
 * without a rebuild, because judging a weak panel response by eye is expensive
 * and ambiguous.
 *
 * force_mode: drive every update in this EBC_MODE_* instead of the one the
 * update carries. Modes 11 and 12 are the DIRECT modes -- they assert neither
 * DISPLAY_MODE nor UPDATE_MODE, so the controller drives from the image without
 * the waveform LUT at all. A hard black/white there proves the pad, clock, rail
 * and image-fetch chain and isolates the fault to the LUT; mush there exonerates
 * the LUT and points at the data path.
 *
 * swap_mst: which buffer goes in WIN_MST0. The vendor's attribution says the
 * target frame does (docs/PORT-FIXES.md §1.0), but a transposed LUT packing and
 * a swapped pair cancel, so both orders need to be reachable without a rebuild.
 */
static int force_mode = -1;
module_param(force_mode, int, 0644);
MODULE_PARM_DESC(force_mode, "override the refresh mode (0-12; -1 = use the update's own)");

static bool swap_mst;
module_param(swap_mst, bool, 0644);
MODULE_PARM_DESC(swap_mst, "put next in WIN_MST0 instead of prev (the vendor doc's order, which does NOT work)");

/*
 * DIAGNOSTIC, REVERT ME. Full-screen white and full-screen black both render
 * as VERTICAL STRIPES spanning the whole panel (2026-08-03, video). A uniform
 * source image can only come out non-uniform if something varies along the
 * source-driver axis, so these are the three fields on that axis, made
 * settable so the axis can be bisected without a rebuild.
 *
 * sdck: source-driver clocks per line. Computed as width/4, but the field is
 * 8 bits and 1024/4 = 256 does not fit -- the driver's `width < 0x400` test
 * silently yields 0xff instead, which is either exactly right (if the field
 * encodes count-1) or one short (if it does not). Nothing establishes which.
 *
 * win_vir: the window's virtual width. Written as width | height << 16 and
 * reads back as 1024; whether the field wants pixels, bytes or 4-pixel groups
 * is not established either.
 */
static int sdck = -1;
module_param(sdck, int, 0644);
MODULE_PARM_DESC(sdck, "override EPD_CTRL SDCK (-1 = width/4 saturated to 0xff)");

static uint win_vir;
module_param(win_vir, uint, 0644);
MODULE_PARM_DESC(win_vir, "override WIN_VIR's low half in pixels (0 = panel width)");

/*
 * Bytes of the target image that differ from what the panel is believed to
 * show. A waveform whose old and new greys are equal everywhere is *correct*
 * to leave the glass alone, so a zero here puts the fault upstream of the scan.
 */
static void ebc_report_image(struct rk3026_ebc *ebc)
{
	const u8 *p = ebc->prev_virt, *n = ebc->next_virt;
	size_t i, diff = 0, black = 0, white = 0;

	for (i = 0; i < ebc->img_size; i++) {
		if (p[i] != n[i])
			diff++;
		if (n[i] == 0x00)
			black++;
		else if (n[i] == 0xff)
			white++;
	}

	dev_info(ebc->dev,
		 "scan: image %zu bytes, %zu differ from prev, next %zu x00 %zu xff\n",
		 ebc->img_size, diff, black, white);
}

/*
 * What the clocks and the EPD rail actually are at the moment of the scan.
 *
 * The GRF and CRU reads are here because a good boot and a bad boot have so
 * far produced dmesg that is identical line for line -- same registers, same
 * LUT, same frame counts, same 4260 ms -- while the panel images on one and
 * not the other. Whatever differs is therefore something the driver sets up
 * but never reads back, and pad mux, the EBC/LCDC pad select and the dclk mux
 * are exactly that: written once at probe, believed forever after.
 */
static void ebc_report_env(struct rk3026_ebc *ebc)
{
	u32 iomux2b = 0, iomux2c = 0, iomux2d = 0, ebc_sel = 0;

	dev_info(ebc->dev, "scan: aclk %lu dclk %lu hclk %lu epd_supply %d\n",
		 clk_get_rate(ebc->aclk), clk_get_rate(ebc->dclk),
		 clk_get_rate(ebc->hclk),
		 ebc->epd_supply ? regulator_is_enabled(ebc->epd_supply) : -1);

	if (ebc->grf) {
		regmap_read(ebc->grf, GRF_IOMUX_GPIO2B, &iomux2b);
		regmap_read(ebc->grf, GRF_IOMUX_GPIO2C, &iomux2c);
		regmap_read(ebc->grf, GRF_IOMUX_GPIO2D, &iomux2d);
		regmap_read(ebc->grf, GRF_EBC_SEL_REG, &ebc_sel);
	}
	dev_info(ebc->dev,
		 "scan: grf iomux2b %08x 2c %08x 2d %08x ebc_sel %08x (want aa/ff28/0a/bit0)\n",
		 iomux2b, iomux2c, iomux2d, ebc_sel);
}

/*
 * Registers the controller reads back, after the group has been latched by
 * CONFIG_DONE. DSP_START, DSP_CTRL and EBC_INT are write-only, so the values
 * printed for those are the driver's own shadow state, not the hardware's.
 */
static void ebc_report_regs(struct rk3026_ebc *ebc, const char *when)
{
	dev_info(ebc->dev,
		 "scan: %s win_ctrl %08x mst0 %08x mst1 %08x vir %08x act %08x dsp %08x dsp_st %08x\n",
		 when, ebc_read(ebc, EBC_WIN_CTRL), ebc_read(ebc, EBC_WIN_MST0),
		 ebc_read(ebc, EBC_WIN_MST1), ebc_read(ebc, EBC_WIN_VIR),
		 ebc_read(ebc, EBC_WIN_ACT), ebc_read(ebc, EBC_WIN_DSP),
		 ebc_read(ebc, EBC_WIN_DSP_ST));
	dev_info(ebc->dev,
		 "scan: %s epd_ctrl %08x act_info %08x ht %08x %08x vt %08x %08x\n",
		 when, ebc_read(ebc, EBC_EPD_CTRL),
		 ebc_read(ebc, EBC_DSP_ACT_INFO),
		 ebc_read(ebc, EBC_DSP_HTIMING0), ebc_read(ebc, EBC_DSP_HTIMING1),
		 ebc_read(ebc, EBC_DSP_VTIMING0), ebc_read(ebc, EBC_DSP_VTIMING1));
	dev_info(ebc->dev,
		 "scan: %s int(hw) %08x; shadow dsp_start %08x dsp_ctrl %08x int %08x\n",
		 when, ebc_read(ebc, EBC_INT), ebc->dsp_start, ebc->dsp_ctrl,
		 ebc->int_reg);
}

/*
 * Read the LUT SRAM window back. A scan against a zero LUT drives no pigment
 * and still ends normally, which is indistinguishable from success by every
 * signal we have today.
 *
 * A readback that is uniformly zero is ambiguous: either the writes did not
 * land, or the window is write-only. Compare against @nonzero_src -- if the
 * source LUT itself is mostly zeros, the waveform selection is the suspect,
 * not the SRAM.
 */
static void ebc_report_lut(struct rk3026_ebc *ebc, const u8 *src, u32 words,
			   u32 chunk)
{
	u32 i, want, got, bad = 0, nonzero_hw = 0, nonzero_src = 0;
	u32 first = 0, first_want = 0, first_got = 0;

	for (i = 0; i < words; i++) {
		want = get_unaligned_le32(src + i * 4);
		got = ebc_read(ebc, EBC_LUT_BASE + i * 4);
		if (want)
			nonzero_src++;
		if (got)
			nonzero_hw++;
		if (got != want && !bad++) {
			first = i;
			first_want = want;
			first_got = got;
		}
	}

	dev_info(ebc->dev,
		 "scan: chunk %u lut %u words, nonzero src %u hw %u, %u mismatched\n",
		 chunk, words, nonzero_src, nonzero_hw, bad);
	if (bad)
		dev_info(ebc->dev, "scan: chunk %u first mismatch word %u: wrote %08x read %08x\n",
			 chunk, first, first_want, first_got);
}

/*
 * H/V timing and active window. The H registers count source-driver clocks,
 * four pixels each; the V registers count lines. E Ink spec names in comments.
 */
static void ebc_set_timing(struct rk3026_ebc *ebc)
{
	const struct display_timing *t = &ebc->timing;
	u32 lsl = t->hsync_len.typ;
	u32 lbl = t->hback_porch.typ;
	u32 lel = t->hfront_porch.typ;
	u32 fsl = t->vsync_len.typ;
	u32 fbl = t->vback_porch.typ;
	u32 fel = t->vfront_porch.typ;
	u32 line = ebc->width / 4;

	ebc_write(ebc, EBC_DSP_HTIMING0,
		  lsl | ((lsl + line + lbl + lel) << 16));
	ebc_write(ebc, EBC_DSP_HTIMING1,
		  (lsl + lbl) | ((lsl + line + lbl) << 16));
	ebc_write(ebc, EBC_DSP_VTIMING0,
		  fsl | ((fsl + ebc->height + fel + fbl) << 16));
	ebc_write(ebc, EBC_DSP_VTIMING1,
		  (fsl + fbl) | ((fsl + fbl + ebc->height) << 16));
	ebc_write(ebc, EBC_DSP_ACT_INFO,
		  ebc->width | (ebc->height << 16));
	ebc_config_done(ebc);
}

/* Full-panel window, 4bpp, origin at the first active pixel. */
static void ebc_set_window(struct rk3026_ebc *ebc)
{
	const struct display_timing *t = &ebc->timing;
	u32 size = ebc->width | (ebc->height << 16);

	ebc_write(ebc, EBC_WIN_CTRL, WIN_CTRL_FIXED);
	/* Both halves are written; only the width half reads back. */
	ebc_write(ebc, EBC_WIN_VIR,
		  win_vir ? (win_vir | (ebc->height << 16)) : size);
	ebc_write(ebc, EBC_WIN_ACT, size);
	ebc_write(ebc, EBC_WIN_DSP, size);
	ebc_write(ebc, EBC_WIN_DSP_ST,
		  (t->hsync_len.typ + t->hback_porch.typ + 1) |
		  ((t->vsync_len.typ + 4) << 16));
	ebc_config_done(ebc);
}

/* VCOM0..3 stay zero on this panel. */
static void ebc_set_border(struct rk3026_ebc *ebc)
{
	ebc_write(ebc, EBC_VCOM0, 0);
	ebc_write(ebc, EBC_VCOM1, 0);
	ebc_write(ebc, EBC_VCOM2, 0);
	ebc_write(ebc, EBC_VCOM3, 0);
}

static void ebc_set_epd_ctrl(struct rk3026_ebc *ebc)
{
	u32 gdck = ebc->timing.hsync_len.typ + ebc->gdck_sta;
	u32 n = ebc->width < 0x400 ? ebc->width / 4 : 0xff;

	if (sdck >= 0)
		n = sdck;

	ebc_write(ebc, EBC_EPD_CTRL,
		  EPD_GDCK_STA(gdck) | EPD_LGONL(gdck + ebc->lgonl) |
		  EPD_SDCK(n) | EPD_GATE_SCAN_DIR | EPD_SOURCE_SCAN_DIR);
}

/*
 * DSP_CTRL mode class. Modes split into three groups by the mask tests in the
 * controller's mode dispatch: LUT-driven full refreshes assert DISPLAY_MODE,
 * partial refreshes assert UPDATE_MODE as well, and the direct (non-LUT)
 * modes assert neither.
 */
static u32 ebc_dsp_ctrl_mode(u32 mode)
{
	u32 bit;

	if (mode > EBC_MODE_MAX)
		return DSP_DISPLAY_MODE;

	bit = BIT(mode);
	if (bit & 0x0522)
		return DSP_DISPLAY_MODE;
	if (bit & 0x1a01)
		return 0;
	if (bit & 0x00cc)
		return DSP_UPDATE_MODE | DSP_DISPLAY_MODE;
	return DSP_DISPLAY_MODE;
}

static void ebc_set_dsp_ctrl(struct rk3026_ebc *ebc, u32 mode)
{
	ebc->dsp_ctrl = DSP_CTRL_FIXED | DSP_VCOM_MODE | ebc_dsp_ctrl_mode(mode);
	ebc_write(ebc, EBC_DSP_CTRL, ebc->dsp_ctrl);
	ebc_config_done(ebc);
}

/*
 * MST0 is the image on the panel, MST1 the one being driven towards.
 *
 * ⚠️ This order is established ON HARDWARE (2026-08-03): full-screen white and
 * black render correctly this way round and do not render the other way. It
 * CONTRADICTS docs/VENDOR-BLOBS.md §5, which reads `ebc_image_addr_set` as
 * MST0 = current and MST1 = previous. Either that attribution is wrong or our
 * waveform LUT is packed transposed and the two cancel — unresolved, and the
 * swap_mst knob exists to keep the losing order reachable. Do not "fix" this
 * back to match the doc without a card that images.
 *
 * Both hold absolute physical addresses, biased by the window origin at two
 * pixels per byte over a fixed 1024-pixel row stride.
 */
static void ebc_set_image_addr(struct rk3026_ebc *ebc, u32 x, u32 y)
{
	u32 off = (x + y * EBC_ROW_STRIDE) / 2;
	dma_addr_t m0 = swap_mst ? ebc->next_phys : ebc->prev_phys;
	dma_addr_t m1 = swap_mst ? ebc->prev_phys : ebc->next_phys;

	ebc_write(ebc, EBC_WIN_MST0, lower_32_bits(m0) + off);
	ebc_write(ebc, EBC_WIN_MST1, lower_32_bits(m1) + off);
	ebc_config_done(ebc);
}

/*
 * Kick one waveform chunk: at most EBC_LUT_MAX_FRAMES frames of LUT into the
 * SRAM window, then the window geometry, the per-chunk frame count and the
 * scan itself.
 *
 * Everything up to CONFIG_DONE is a register group latched by that write, and
 * FRAME_START is only asserted afterwards. VCOM_MODE stays set while further
 * chunks follow so the rails are not dropped mid-waveform.
 */
static void ebc_frame_kick(struct rk3026_ebc *ebc, const u8 *lut,
			   u32 chunk, u32 frames, bool last)
{
	const u8 *src = lut + chunk * EBC_LUT_MAX_FRAMES * EBC_LUT_FRAME_BYTES;
	u32 size = ebc->width | (ebc->height << 16);
	const struct display_timing *t = &ebc->timing;
	u32 words = frames * (EBC_LUT_FRAME_BYTES / 4);
	u32 i;

	for (i = 0; i < words; i++)
		ebc_write(ebc, EBC_LUT_BASE + i * 4, get_unaligned_le32(src + i * 4));

	if (debug_scan)
		ebc_report_lut(ebc, src, words, chunk);

	ebc_write(ebc, EBC_WIN_DSP_ST,
		  (t->hsync_len.typ + t->hback_porch.typ + 1) |
		  ((t->vsync_len.typ + 4) << 16));
	ebc_write(ebc, EBC_WIN_DSP, size);
	ebc_write(ebc, EBC_WIN_ACT, size);

	ebc->int_reg = EBC_INT_FRAME_NUM(frames) | EBC_INT_ENABLE;
	ebc_write(ebc, EBC_INT, ebc->int_reg);

	ebc->dsp_ctrl &= ~DSP_VCOM_MODE;
	if (!last)
		ebc->dsp_ctrl |= DSP_VCOM_MODE;
	ebc_write(ebc, EBC_DSP_CTRL, ebc->dsp_ctrl);

	ebc->dsp_start &= ~DSP_FRAME_NUM_MASK;
	ebc->dsp_start |= DSP_FRAME_NUM(frames - 1);
	ebc_write(ebc, EBC_DSP_START, ebc->dsp_start);

	ebc_config_done(ebc);

	ebc->dsp_start |= DSP_FRAME_START;
	ebc_write(ebc, EBC_DSP_START, ebc->dsp_start);
}

static irqreturn_t ebc_irq(int irq, void *data)
{
	struct rk3026_ebc *ebc = data;
	u32 status = ebc_read(ebc, EBC_INT);

	dbg_last_status = status;
	atomic_inc(&dbg_irqs);

	if (!(status & EBC_INT_FRAME_END))
		return IRQ_NONE;

	/*
	 * Acknowledge order: the three write-one-to-clear bits are asserted one
	 * at a time, then display-end is masked and unmasked again.
	 */
	ebc->int_reg |= EBC_INT_FRAMEEND_CLEAR;
	ebc_write(ebc, EBC_INT, ebc->int_reg);
	ebc->int_reg |= EBC_INT_DISPLAYEND_CLEAR;
	ebc_write(ebc, EBC_INT, ebc->int_reg);
	ebc->int_reg |= EBC_INT_FRAMEFLAG_CLEAR;
	ebc_write(ebc, EBC_INT, ebc->int_reg);
	ebc->int_reg |= EBC_INT_DISPLAYEND_MASK;
	ebc_write(ebc, EBC_INT, ebc->int_reg);

	ebc->int_reg &= ~EBC_INT_DISPLAYEND_MASK;
	ebc_write(ebc, EBC_INT, ebc->int_reg);
	ebc_config_done(ebc);

	complete(&ebc->frame_done);
	return IRQ_HANDLED;
}

/* Panel temperature, in degrees Celsius. */
static int ebc_read_temp(struct rk3026_ebc *ebc)
{
	int temp;

	if (!ebc->tzd || thermal_zone_get_temp(ebc->tzd, &temp))
		return EBC_TEMP_FALLBACK;

	temp /= 1000;
	/* A reading above 50 degrees is not trusted; fall back to 25. */
	if (temp > EBC_TEMP_MAX) {
		dev_warn_ratelimited(ebc->dev,
				     "panel temperature %d out of range, using %d\n",
				     temp, EBC_TEMP_FALLBACK);
		temp = EBC_TEMP_FALLBACK;
	}
	return temp;
}

/**
 * rk3026_ebc_blit_xrgb8888 - pack a damaged region into the target image
 * @ebc: controller
 * @src: XRGB8888 source, top-left of the whole framebuffer
 * @src_pitch: source stride in bytes
 * @clip: damaged region, in pixels
 *
 * The panel is 4bpp with two pixels per byte, low nibble first. The clip is
 * widened to an even pixel boundary so byte writes stay whole.
 */
void rk3026_ebc_blit_xrgb8888(struct rk3026_ebc *ebc, const void *src,
			      unsigned int src_pitch, const struct drm_rect *clip)
{
	int x1 = max(clip->x1, 0) & ~1;
	int x2 = min_t(int, round_up(clip->x2, 2), (int)ebc->width);
	int y1 = max(clip->y1, 0);
	int y2 = min_t(int, clip->y2, (int)ebc->height);
	int x, y;

	if (x1 >= x2 || y1 >= y2)
		return;

	for (y = y1; y < y2; y++) {
		const u8 *s = (const u8 *)src + (size_t)y * src_pitch + x1 * 4;
		u8 *d = (u8 *)ebc->next_virt + ((size_t)y * ebc->width + x1) / 2;

		for (x = x1; x < x2; x += 2, s += 8, d++) {
			u32 p0 = get_unaligned_le32(s);
			u32 p1 = get_unaligned_le32(s + 4);
			u32 g0 = (77 * ((p0 >> 16) & 0xff) +
				  150 * ((p0 >> 8) & 0xff) +
				  29 * (p0 & 0xff)) / 256;
			u32 g1 = (77 * ((p1 >> 16) & 0xff) +
				  150 * ((p1 >> 8) & 0xff) +
				  29 * (p1 & 0xff)) / 256;

			*d = (g0 >> 4) | (g1 & 0xf0);
		}
	}
}

/* Drive one queued update onto the panel. Runs only on the refresh thread. */
static void ebc_do_refresh(struct rk3026_ebc *ebc,
			   const struct rk3026_ebc_update *upd)
{
	u32 frames, done = 0, chunk = 0;
	const u8 *lut;
	int temp;
	u32 mode = upd->mode;

	if (!ebc->waveform)
		return;

	/* DIAGNOSTIC: drive a different mode class with the same data and scan. */
	if (force_mode >= 0 && force_mode <= EBC_MODE_MAX)
		mode = force_mode;

	temp = upd->temp_valid ? upd->temp : ebc_read_temp(ebc);
	if (temp > EBC_TEMP_MAX)
		temp = EBC_TEMP_FALLBACK;

	/*
	 * A direct mode has no LUT of its own; fall back to the update's own mode
	 * for the scan data so the DSP_CTRL class is the only thing that changed.
	 */
	lut = rk3026_ebc_waveform_lut(ebc->waveform, mode, temp, &frames);
	if (!lut && mode != upd->mode)
		lut = rk3026_ebc_waveform_lut(ebc->waveform, upd->mode, temp, &frames);
	if (!lut) {
		if (debug_scan)
			dev_info(ebc->dev, "scan: no LUT for mode %u at %d C\n",
				 mode, temp);
		return;
	}

	if (debug_scan) {
		dev_info(ebc->dev,
			 "scan: mode %u (update asked %u) temp %d C, %u frames, clip %d,%d-%d,%d\n",
			 mode, upd->mode, temp, frames, upd->clip.x1, upd->clip.y1,
			 upd->clip.x2, upd->clip.y2);
		ebc_report_env(ebc);
		ebc_report_image(ebc);
	}

	/*
	 * The target image must not move while the controller is scanning it,
	 * so writers are held off for the whole waveform.
	 */
	mutex_lock(&ebc->blit_lock);

	ebc_set_dsp_ctrl(ebc, mode);
	ebc_set_image_addr(ebc, 0, 0);

	if (debug_scan)
		ebc_report_regs(ebc, "armed");

	while (done < frames) {
		u32 n = min_t(u32, frames - done, EBC_LUT_MAX_FRAMES);
		int irqs = atomic_read(&dbg_irqs);
		ktime_t t0 = ktime_get();
		bool ok;

		reinit_completion(&ebc->frame_done);
		ebc_frame_kick(ebc, lut, chunk, n, done + n >= frames);
		ok = wait_for_completion_timeout(&ebc->frame_done,
						msecs_to_jiffies(EBC_FRAME_TIMEOUT_MS));
		if (debug_scan)
			dev_info(ebc->dev,
				 "scan: chunk %u %u frames %s in %lld ms (expect %u), %d irqs, status %08x\n",
				 chunk, n, ok ? "ended" : "TIMED OUT",
				 ktime_ms_delta(ktime_get(), t0),
				 n * 1000 / 85,
				 atomic_read(&dbg_irqs) - irqs, dbg_last_status);
		if (!ok) {
			dev_warn_ratelimited(ebc->dev, "frame timeout\n");
			break;
		}
		done += n;
		chunk++;
	}

	if (debug_scan) {
		ebc_report_regs(ebc, "done");
		dev_info(ebc->dev, "scan: %u of %u frames driven\n", done, frames);
		debug_scan--;
	}

	/* What was driven in is now what the panel shows. */
	memcpy(ebc->prev_virt, ebc->next_virt, ebc->img_size);
	mutex_unlock(&ebc->blit_lock);
}

static bool ebc_queue_empty(struct rk3026_ebc *ebc)
{
	bool empty;

	spin_lock(&ebc->queue_lock);
	empty = ebc->queue_head == ebc->queue_tail;
	spin_unlock(&ebc->queue_lock);
	return empty;
}

static int ebc_refresh_thread(void *data)
{
	struct rk3026_ebc *ebc = data;

	while (!kthread_should_stop()) {
		struct rk3026_ebc_update upd;

		wait_event_interruptible(ebc->update_wq,
					 !ebc_queue_empty(ebc) ||
					 kthread_should_stop());
		if (kthread_should_stop())
			break;

		spin_lock(&ebc->queue_lock);
		if (ebc->queue_head == ebc->queue_tail) {
			spin_unlock(&ebc->queue_lock);
			continue;
		}
		upd = ebc->queue[ebc->queue_head % EBC_UPDATE_QUEUE_LEN];
		ebc->queue_head++;
		ebc->marker_active = upd.marker;
		spin_unlock(&ebc->queue_lock);

		ebc_do_refresh(ebc, &upd);

		spin_lock(&ebc->queue_lock);
		ebc->marker_active = 0;
		spin_unlock(&ebc->queue_lock);
		wake_up_all(&ebc->marker_wq);
	}
	return 0;
}

/**
 * rk3026_ebc_queue_update - hand a panel update to the refresh thread
 * @ebc: controller
 * @clip: damaged region; recorded but the scan is always full-panel
 * @mode: EBC work mode
 * @temp: caller-supplied panel temperature, in degrees Celsius
 * @temp_valid: whether @temp should be used instead of the sensor
 * @marker: caller-owned completion tag, or 0 for fire-and-forget
 *
 * A full queue is coalesced into its newest entry: the clips are merged and
 * the newest mode wins, which is what a burst of small damage rectangles
 * wants anyway.
 */
/*
 * Block until nothing is queued and no waveform is in flight. The thread holds
 * @blit_lock for the whole of an update, so an empty queue plus an uncontended
 * lock means the panel is idle.
 */
void rk3026_ebc_wait_idle(struct rk3026_ebc *ebc)
{
	bool empty;

	for (;;) {
		spin_lock(&ebc->queue_lock);
		empty = ebc->queue_head == ebc->queue_tail;
		spin_unlock(&ebc->queue_lock);
		if (empty)
			break;
		msleep(20);
	}

	mutex_lock(&ebc->blit_lock);
	mutex_unlock(&ebc->blit_lock);
}

void rk3026_ebc_queue_update(struct rk3026_ebc *ebc, const struct drm_rect *clip,
			     u32 mode, int temp, bool temp_valid, u32 marker)
{
	struct rk3026_ebc_update *upd;
	unsigned int used;

	spin_lock(&ebc->queue_lock);

	used = ebc->queue_tail - ebc->queue_head;
	if (used >= EBC_UPDATE_QUEUE_LEN) {
		upd = &ebc->queue[(ebc->queue_tail - 1) % EBC_UPDATE_QUEUE_LEN];
		upd->clip.x1 = min(upd->clip.x1, clip->x1);
		upd->clip.y1 = min(upd->clip.y1, clip->y1);
		upd->clip.x2 = max(upd->clip.x2, clip->x2);
		upd->clip.y2 = max(upd->clip.y2, clip->y2);
		upd->mode = mode;
		upd->marker = marker;
		upd->temp = temp;
		upd->temp_valid = temp_valid;
	} else {
		upd = &ebc->queue[ebc->queue_tail % EBC_UPDATE_QUEUE_LEN];
		upd->clip = *clip;
		upd->mode = mode;
		upd->marker = marker;
		upd->temp = temp;
		upd->temp_valid = temp_valid;
		ebc->queue_tail++;
	}

	spin_unlock(&ebc->queue_lock);
	wake_up(&ebc->update_wq);
}

static bool ebc_marker_pending(struct rk3026_ebc *ebc, u32 marker)
{
	bool pending = false;
	unsigned int i;

	spin_lock(&ebc->queue_lock);
	if (ebc->marker_active == marker) {
		pending = true;
	} else {
		for (i = ebc->queue_head; i != ebc->queue_tail; i++) {
			if (ebc->queue[i % EBC_UPDATE_QUEUE_LEN].marker == marker) {
				pending = true;
				break;
			}
		}
	}
	spin_unlock(&ebc->queue_lock);
	return pending;
}

/**
 * rk3026_ebc_wait_marker - block until a queued update has been driven out
 * @ebc: controller
 * @marker: value returned by rk3026_ebc_queue_update()
 */
int rk3026_ebc_wait_marker(struct rk3026_ebc *ebc, u32 marker)
{
	long ret;

	if (!marker)
		return 0;

	ret = wait_event_interruptible_timeout(ebc->marker_wq,
					       !ebc_marker_pending(ebc, marker),
					       msecs_to_jiffies(EBC_MARKER_TIMEOUT_MS));
	if (ret < 0)
		return ret;
	return ret ? 0 : -ETIMEDOUT;
}

/* --- DRM --- */

static const u32 ebc_plane_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const u64 ebc_plane_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static int ebc_plane_atomic_check(struct drm_plane *plane,
				  struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;

	if (!new_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, false);
}

static void ebc_plane_atomic_update(struct drm_plane *plane,
				    struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old_state =
		drm_atomic_get_old_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow =
		to_drm_shadow_plane_state(new_state);
	struct rk3026_ebc *ebc = drm_to_ebc(plane->dev);
	struct drm_rect clip;
	int idx;

	if (!new_state->fb || !new_state->visible)
		return;
	if (!drm_atomic_helper_damage_merged(old_state, new_state, &clip))
		return;
	if (!drm_dev_enter(plane->dev, &idx))
		return;

	/*
	 * A mxcfb client drives its own updates and annotates them with a
	 * waveform mode; leave its pixels alone rather than refreshing twice.
	 */
	if (!ebc->mxcfb_active) {
		mutex_lock(&ebc->blit_lock);
		rk3026_ebc_blit_xrgb8888(ebc, shadow->data[0].vaddr,
					 new_state->fb->pitches[0], &clip);
		mutex_unlock(&ebc->blit_lock);

		rk3026_ebc_queue_update(ebc, &clip, EBC_MODE_DEFAULT, 0, false, 0);
	}

	drm_dev_exit(idx);
}

static const struct drm_plane_funcs ebc_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static const struct drm_plane_helper_funcs ebc_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check		= ebc_plane_atomic_check,
	.atomic_update		= ebc_plane_atomic_update,
};

static void ebc_crtc_atomic_enable(struct drm_crtc *crtc,
				   struct drm_atomic_state *state)
{
	struct rk3026_ebc *ebc = drm_to_ebc(crtc->dev);
	struct drm_rect clip = DRM_RECT_INIT(0, 0, ebc->width, ebc->height);
	int ret;

	/*
	 * The high-voltage rails must be up before the panel is driven; the PMIC
	 * sequences them internally once ENABLE is written.
	 */
	ret = regulator_enable(ebc->epd_supply);
	if (ret) {
		dev_err(ebc->dev, "failed to enable EPD rails: %d\n", ret);
		return;
	}

	/* Drive the panel to a known white before anything is composed on it. */
	memset(ebc->next_virt, 0xff, ebc->img_size);
	rk3026_ebc_queue_update(ebc, &clip, EBC_MODE_RESET, 0, false, 0);
}

static void ebc_crtc_atomic_disable(struct drm_crtc *crtc,
				    struct drm_atomic_state *state)
{
	struct rk3026_ebc *ebc = drm_to_ebc(crtc->dev);

	/* Let any waveform in flight finish before the rails drop. */
	rk3026_ebc_wait_idle(ebc);
	regulator_disable(ebc->epd_supply);
}

static const struct drm_crtc_funcs ebc_crtc_funcs = {
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.reset			= drm_atomic_helper_crtc_reset,
	.destroy		= drm_crtc_cleanup,
	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_crtc_helper_funcs ebc_crtc_helper_funcs = {
	.atomic_enable		= ebc_crtc_atomic_enable,
	.atomic_disable		= ebc_crtc_atomic_disable,
};

static const struct drm_encoder_funcs ebc_encoder_funcs = {
	.destroy		= drm_encoder_cleanup,
};

static int ebc_connector_get_modes(struct drm_connector *connector)
{
	struct rk3026_ebc *ebc = drm_to_ebc(connector->dev);
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);
	if (!mode)
		return 0;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	mode->clock = EBC_PIXEL_RATE / 1000;
	mode->hdisplay = ebc->width;
	mode->hsync_start = ebc->width;
	mode->hsync_end = ebc->width;
	mode->htotal = ebc->width;
	mode->vdisplay = ebc->height;
	mode->vsync_start = ebc->height;
	mode->vsync_end = ebc->height;
	mode->vtotal = ebc->height;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_connector_funcs ebc_connector_funcs = {
	.reset			= drm_atomic_helper_connector_reset,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs ebc_connector_helper_funcs = {
	.get_modes		= ebc_connector_get_modes,
};

static const struct drm_mode_config_funcs ebc_mode_config_funcs = {
	.fb_create		= drm_gem_fb_create_with_dirty,
	.atomic_check		= drm_atomic_helper_check,
	.atomic_commit		= drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_FOPS(ebc_fops);

#ifdef CONFIG_DRM_FBDEV_EMULATION
static int ebc_fbdev_probe(struct drm_fb_helper *helper,
			   struct drm_fb_helper_surface_size *sizes)
{
	int ret = drm_fbdev_shmem_driver_fbdev_probe(helper, sizes);

	if (!ret)
		rk3026_ebc_mxcfb_install(drm_to_ebc(helper->dev), helper->info);
	return ret;
}
#endif

static const struct drm_driver ebc_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &ebc_fops,
	.name			= DRIVER_NAME,
	.desc			= "Rockchip RK3026 EBC",
	.major			= 1,
	.minor			= 0,
	DRM_GEM_SHMEM_DRIVER_OPS,
#ifdef CONFIG_DRM_FBDEV_EMULATION
	.fbdev_probe		= ebc_fbdev_probe,
#endif
};

/* --- bring-up --- */

static int ebc_setup_buffers(struct rk3026_ebc *ebc)
{
	ebc->img_size = ebc->width * ebc->height / 2;

	ebc->prev_virt = dmam_alloc_attrs(ebc->dev, ebc->img_size,
					  &ebc->prev_phys, GFP_KERNEL,
					  DMA_ATTR_WRITE_COMBINE);
	if (!ebc->prev_virt)
		return -ENOMEM;
	ebc->next_virt = dmam_alloc_attrs(ebc->dev, ebc->img_size,
					  &ebc->next_phys, GFP_KERNEL,
					  DMA_ATTR_WRITE_COMBINE);
	if (!ebc->next_virt)
		return -ENOMEM;

	/* The panel is assumed white after power-up. */
	memset(ebc->prev_virt, 0xff, ebc->img_size);
	memset(ebc->next_virt, 0xff, ebc->img_size);
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

	/*
	 * ⚠️ BISECT, 2026-08-03: enabling mclk (ACLK_EBC) and asserting dclk's
	 * parent here is DISABLED, because the two cards that did it are the two
	 * whose U-Boot console then hung at misc_init_r on the following boot,
	 * while the four cards that made the identical register writes by hand
	 * from a running Linux were fine. The U-Boot binary is byte-identical
	 * across all six (loader checksum 3b8cdcfc), so the suspect is the
	 * CCF-mediated path, not the register values.
	 *
	 * Prime suspect: clk-rk3026.c models aclk_vio1_niu and aclk_ebc on the
	 * SAME gate bit (CLKGATE_CON9 b10), so refcounting can close it for one
	 * consumer while the other still needs it -- and ebc_hw_fini() disables
	 * mclk on shutdown.
	 *
	 * Until that is understood, 66 MHz comes from devmem at the shell:
	 *   0x200000a0 = 0x00000101   dclk_ebc -> GPLL/2
	 *   0x200000c0 = 0x00000081   aclk_lcdc0_src -> GPLL/2
	 *   0x200000dc = 0x00010000   ungate aclk_lcdc0_src (CON3 b0)
	 */
	if (0 && ebc->mclk) {
		ret = clk_prepare_enable(ebc->mclk);
		if (ret)
			goto err_aclk;
	}

	/*
	 * dclk boots muxed to CPLL, which sits in slow mode at 24 MHz -- 12 MHz
	 * after the divider, against the panel's 20 MHz source-clock spec, and a
	 * 54-frame scan then takes 4260 ms instead of 790.
	 *
	 * ⚠️ Order matters and cost a session. clk_set_rate() alone returns
	 * SUCCESS and leaves the hardware at 12 MHz: CCF reparents, writes the
	 * mux, then writes the divider -- and CLKSEL_CON23 reads back 0xff000100,
	 * which is exactly a hiword-style divider write applied as a plain 32-bit
	 * store, clearing the mux bit the same call had just set. clk_get_rate()
	 * then reports the rate CCF believes, not the one the panel gets. So the
	 * parent is asserted LAST, and the register is logged rather than trusted.
	 */
	ret = clk_set_rate(ebc->dclk, EBC_DCLK_RATE);
	if (ret)
		goto err_mclk;
	/*
	 * Re-enabled 2026-08-03, alone: mclk above stays disabled, so this card
	 * tests the reparent by itself. With it compiled out a scan measured
	 * 4260 ms for 54 frames against the driver's own 635 ms expectation --
	 * i.e. 12 MHz -- while clk_get_rate() reported 66 MHz. Frame time is the
	 * instrument; the CCF view is not.
	 */
	if (ebc->dclk_parent) {
		ret = clk_set_parent(ebc->dclk, ebc->dclk_parent);
		if (ret)
			goto err_mclk;
	}
	ret = clk_prepare_enable(ebc->dclk);
	if (ret)
		goto err_mclk;

	dev_info(ebc->dev, "dclk %lu Hz (CCF's view; check a scan's frame time)\n",
		 clk_get_rate(ebc->dclk));

	/* Route the shared display pads to EBC rather than LCDC. */
	if (ebc->grf)
		regmap_write(ebc->grf, GRF_EBC_SEL_REG,
			     (GRF_EBC_SEL_BIT << 16) | GRF_EBC_SEL_BIT);

	ebc_set_timing(ebc);
	ebc_set_window(ebc);
	ebc_set_border(ebc);
	ebc_set_epd_ctrl(ebc);

	/* Idle DSP_CTRL: the full-refresh class value. */
	ebc->dsp_ctrl = DSP_CTRL_FIXED | DSP_DISPLAY_MODE;
	ebc_write(ebc, EBC_DSP_CTRL, ebc->dsp_ctrl);
	ebc_config_done(ebc);
	return 0;

err_mclk:
	clk_disable_unprepare(ebc->mclk);
err_aclk:
	clk_disable_unprepare(ebc->aclk);
err_hclk:
	clk_disable_unprepare(ebc->hclk);
	return ret;
}

static void ebc_hw_fini(void *data)
{
	struct rk3026_ebc *ebc = data;

	clk_disable_unprepare(ebc->dclk);
	clk_disable_unprepare(ebc->mclk);
	clk_disable_unprepare(ebc->aclk);
	clk_disable_unprepare(ebc->hclk);
}

static void ebc_stop_thread(void *data)
{
	struct rk3026_ebc *ebc = data;

	kthread_stop(ebc->refresh_thread);
}

static void ebc_release_fw(void *data)
{
	release_firmware(data);
}

static int ebc_load_waveform(struct rk3026_ebc *ebc)
{
	struct ebc_waveform *wf;
	int ret;

	ret = request_firmware(&ebc->fw, EBC_WAVEFORM_FW, ebc->dev);
	if (ret) {
		dev_warn(ebc->dev, "waveform %s not found (%d)\n",
			 EBC_WAVEFORM_FW, ret);
		return 0;
	}
	ret = devm_add_action_or_reset(ebc->dev, ebc_release_fw,
				       (void *)ebc->fw);
	if (ret)
		return ret;

	wf = rk3026_ebc_waveform_parse(ebc->dev, ebc->fw->data, ebc->fw->size);
	if (IS_ERR(wf)) {
		dev_warn(ebc->dev, "waveform %s rejected (%ld)\n",
			 EBC_WAVEFORM_FW, PTR_ERR(wf));
		return 0;
	}
	ebc->waveform = wf;
	return 0;
}

static int ebc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rk3026_ebc *ebc;
	struct drm_device *drm;
	u32 rotation;
	int ret;

	ebc = devm_drm_dev_alloc(dev, &ebc_drm_driver, struct rk3026_ebc, drm);
	if (IS_ERR(ebc))
		return PTR_ERR(ebc);

	drm = &ebc->drm;
	ebc->dev = dev;
	platform_set_drvdata(pdev, ebc);

	ebc->epd_supply = devm_regulator_get(dev, "epd");
	if (IS_ERR(ebc->epd_supply))
		return dev_err_probe(dev, PTR_ERR(ebc->epd_supply),
				     "failed to get EPD rail supply\n");

	init_completion(&ebc->frame_done);
	init_waitqueue_head(&ebc->update_wq);
	init_waitqueue_head(&ebc->marker_wq);
	spin_lock_init(&ebc->queue_lock);
	ret = drmm_mutex_init(drm, &ebc->blit_lock);
	if (ret)
		return ret;

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
	/* Both optional so an older device tree still probes. */
	ebc->mclk = devm_clk_get_optional(dev, "mclk");
	if (IS_ERR(ebc->mclk))
		return dev_err_probe(dev, PTR_ERR(ebc->mclk), "bad mclk\n");
	ebc->dclk_parent = devm_clk_get_optional(dev, "dclk-parent");
	if (IS_ERR(ebc->dclk_parent))
		return dev_err_probe(dev, PTR_ERR(ebc->dclk_parent), "bad dclk-parent\n");

	ebc->grf = syscon_regmap_lookup_by_phandle(dev->of_node, "rockchip,grf");
	if (IS_ERR(ebc->grf)) {
		dev_warn(dev, "no rockchip,grf; skipping pad mux\n");
		ebc->grf = NULL;
	}

	ret = of_get_display_timing(dev->of_node, "panel-timing", &ebc->timing);
	if (ret)
		return dev_err_probe(dev, ret, "missing panel-timing\n");
	ebc->width = ebc->timing.hactive.typ;
	ebc->height = ebc->timing.vactive.typ;
	if (ebc->width % 8 || ebc->height % 2)
		return dev_err_probe(dev, -EINVAL, "unsupported panel size\n");

	/* E Ink GDCK_STA / LGONL; no display-timing binding covers them. */
	if (of_property_read_u32(dev->of_node, "rockchip,gdck-sta",
				 &ebc->gdck_sta))
		return dev_err_probe(dev, -EINVAL, "missing rockchip,gdck-sta\n");
	if (of_property_read_u32(dev->of_node, "rockchip,lgonl", &ebc->lgonl))
		return dev_err_probe(dev, -EINVAL, "missing rockchip,lgonl\n");

	/*
	 * Panel mounting angle, counter-clockwise, as the common "rotation"
	 * property. The panel table in the vendor firmware carries 270.
	 */
	ebc->rotation = DRM_MODE_PANEL_ORIENTATION_UNKNOWN;
	if (!of_property_read_u32(dev->of_node, "rotation", &rotation)) {
		switch (rotation) {
		case 0:
			ebc->rotation = DRM_MODE_PANEL_ORIENTATION_NORMAL;
			break;
		case 90:
			ebc->rotation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP;
			break;
		case 180:
			ebc->rotation = DRM_MODE_PANEL_ORIENTATION_BOTTOM_UP;
			break;
		case 270:
			ebc->rotation = DRM_MODE_PANEL_ORIENTATION_LEFT_UP;
			break;
		default:
			dev_warn(dev, "ignoring rotation %u\n", rotation);
			break;
		}
	}

	/*
	 * Panel thermometer, in the EPD PMIC. This is a REAL dependency, not a
	 * nicety: the waveform LUT is selected by temperature, so a driver that
	 * probes before the zone exists does not degrade, it drives every
	 * update from the wrong bucket for the life of the boot -- silently,
	 * behind one dev_warn. Defer instead, and let the ordering resolve.
	 *
	 * The zone comes from the tps65185's hwmon channel via the THERMAL_OF
	 * bridge, so this waits on an i2c driver several subsystems away, which
	 * is exactly the kind of ordering that varies between boots.
	 */
	ebc->tzd = thermal_zone_get_zone_by_name("epd-thermal");
	if (IS_ERR(ebc->tzd))
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "epd-thermal zone not ready (%ld)\n",
				     PTR_ERR(ebc->tzd));

	ret = of_reserved_mem_device_init(dev);
	if (ret)
		dev_warn(dev, "no reserved memory region (%d)\n", ret);

	ret = ebc_setup_buffers(ebc);
	if (ret)
		return ret;

	ret = ebc_load_waveform(ebc);
	if (ret)
		return ret;

	ebc->irq = platform_get_irq(pdev, 0);
	if (ebc->irq < 0)
		return ebc->irq;
	ret = devm_request_irq(dev, ebc->irq, ebc_irq, 0, DRIVER_NAME, ebc);
	if (ret)
		return dev_err_probe(dev, ret, "request_irq\n");

	ret = ebc_hw_init(ebc);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, ebc_hw_fini, ebc);
	if (ret)
		return ret;

	ebc->refresh_thread = kthread_run(ebc_refresh_thread, ebc, "ebc-refresh");
	if (IS_ERR(ebc->refresh_thread))
		return PTR_ERR(ebc->refresh_thread);
	ret = devm_add_action_or_reset(dev, ebc_stop_thread, ebc);
	if (ret)
		return ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.funcs = &ebc_mode_config_funcs;
	drm->mode_config.min_width = ebc->width;
	drm->mode_config.max_width = ebc->width;
	drm->mode_config.min_height = ebc->height;
	drm->mode_config.max_height = ebc->height;

	ret = drm_universal_plane_init(drm, &ebc->plane, 1, &ebc_plane_funcs,
				       ebc_plane_formats,
				       ARRAY_SIZE(ebc_plane_formats),
				       ebc_plane_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&ebc->plane, &ebc_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&ebc->plane);

	ret = drm_crtc_init_with_planes(drm, &ebc->crtc, &ebc->plane, NULL,
					&ebc_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&ebc->crtc, &ebc_crtc_helper_funcs);

	ret = drm_encoder_init(drm, &ebc->encoder, &ebc_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;
	ebc->encoder.possible_crtcs = drm_crtc_mask(&ebc->crtc);

	ret = drm_connector_init(drm, &ebc->connector, &ebc_connector_funcs,
				 DRM_MODE_CONNECTOR_Unknown);
	if (ret)
		return ret;
	drm_connector_helper_add(&ebc->connector, &ebc_connector_helper_funcs);
	drm_connector_set_panel_orientation(&ebc->connector, ebc->rotation);
	drm_connector_attach_encoder(&ebc->connector, &ebc->encoder);

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_client_setup(drm, NULL);

	dev_info(dev, "RK3026 EBC %ux%u\n", ebc->width, ebc->height);
	return 0;
}

static void ebc_remove(struct platform_device *pdev)
{
	struct rk3026_ebc *ebc = platform_get_drvdata(pdev);

	drm_dev_unplug(&ebc->drm);
	drm_atomic_helper_shutdown(&ebc->drm);
}

static void ebc_shutdown(struct platform_device *pdev)
{
	struct rk3026_ebc *ebc = platform_get_drvdata(pdev);

	drm_atomic_helper_shutdown(&ebc->drm);
}

static const struct of_device_id ebc_of_match[] = {
	{ .compatible = "rockchip,rk3026-ebc" },
	{ }
};
MODULE_DEVICE_TABLE(of, ebc_of_match);

static struct platform_driver ebc_driver = {
	.probe	= ebc_probe,
	.remove	= ebc_remove,
	.shutdown = ebc_shutdown,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= ebc_of_match,
	},
};
module_platform_driver(ebc_driver);

MODULE_DESCRIPTION("Rockchip RK3026 EBC E-Ink DRM driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(EBC_WAVEFORM_FW);
