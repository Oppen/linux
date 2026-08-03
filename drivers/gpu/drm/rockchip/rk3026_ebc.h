/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip RK3026 EBC (E-Ink Broadcaster) register map and driver-internal API.
 *
 * Base 0x10114000, size 16 KiB, GIC SPI 49. The shared display pads are routed
 * to EBC (instead of LCDC) by setting bit 0 of GRF register 0x150.
 */
#ifndef _RK3026_EBC_H
#define _RK3026_EBC_H

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/wait.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>
#include <drm/drm_rect.h>

#include <video/display_timing.h>

/* Register offsets within the EBC MMIO window. */
#define EBC_DSP_START		0x00
#define EBC_EPD_CTRL		0x04
#define EBC_DSP_CTRL		0x08
#define EBC_DSP_HTIMING0	0x0c
#define EBC_DSP_HTIMING1	0x10
#define EBC_DSP_VTIMING0	0x14
#define EBC_DSP_VTIMING1	0x18
#define EBC_DSP_ACT_INFO	0x1c
#define EBC_WIN_CTRL		0x20
#define EBC_WIN_MST0		0x24
#define EBC_WIN_MST1		0x28
#define EBC_WIN_VIR		0x2c
#define EBC_WIN_ACT		0x30
#define EBC_WIN_DSP		0x34
#define EBC_WIN_DSP_ST		0x38
#define EBC_INT			0x3c
#define EBC_VCOM0		0x40
#define EBC_VCOM1		0x44
#define EBC_VCOM2		0x48
#define EBC_VCOM3		0x4c
#define EBC_CONFIG_DONE		0x50

/*
 * Waveform LUT SRAM, mapped inside the same window at +0x1000: 0x40 frames of
 * 0x40 bytes. Each frame is 16 words indexed by the new grey level, 2 bits per
 * old grey level. A waveform longer than 0x40 frames is sent in chunks.
 */
#define EBC_LUT_BASE		0x1000
#define EBC_LUT_FRAME_BYTES	0x40
#define EBC_LUT_MAX_FRAMES	0x40

/* EBC_DSP_START. */
#define DSP_FRAME_START		BIT(0)
#define DSP_FRAME_NUM_MASK	(0x3fu << 2)
#define DSP_FRAME_NUM(x)	(((x) & 0x3f) << 2)

/*
 * EBC_EPD_CTRL. SDCK counts source-driver clocks per line (width/4,
 * saturated), LGONL and GDCK_STA are the E Ink gate timings offset by
 * hsync_len.
 */
#define EPD_SOURCE_SCAN_DIR	BIT(0)
#define EPD_GATE_SCAN_DIR	BIT(1)
#define EPD_SDCK(x)		(((x) & 0xff) << 8)
#define EPD_LGONL(x)		(((x) & 0x7ff) << 16)
#define EPD_GDCK_STA(x)		(((x) & 0x1f) << 27)

/* EBC_WIN_CTRL: win enable, AHB burst, 4bpp format. */
#define WIN_CTRL_FIXED		0x20fc0u

/* EBC_INT. Status bit 1 is frame end. */
#define EBC_INT_FRAME_END	BIT(1)
#define EBC_INT_DISPLAYEND_MASK	BIT(4)
#define EBC_INT_FRAMEEND_CLEAR	BIT(6)
#define EBC_INT_DISPLAYEND_CLEAR BIT(7)
#define EBC_INT_FRAMEFLAG_CLEAR	BIT(8)
#define EBC_INT_FRAME_NUM(x)	(((x) & 0x7f) << 9)
/* Bits 3+5, set by every frame start. */
#define EBC_INT_ENABLE		0x28u

/* EBC_DSP_CTRL fixed top bits. */
#define DSP_CTRL_FIXED		0xc0030000u
#define DSP_VCOM_MODE		BIT(27)
#define DSP_DISPLAY_MODE	BIT(28)
#define DSP_UPDATE_MODE		BIT(29)

/* Latch value written to EBC_CONFIG_DONE after each register group. */
#define EBC_CONFIG_DONE_VAL	0xffff

/* GRF register that selects EBC vs LCDC on the shared pads (bit 0). */
#define GRF_EBC_SEL_REG		0x150
#define GRF_EBC_SEL_BIT		BIT(0)

/*
 * GRF pad-mux registers for the eight EBC pads, read back only for reporting.
 * Two bits per pin at 0xa8 + 4 * group; bank 2 groups B, C and D.
 */
#define GRF_IOMUX_GPIO2B	0xcc
#define GRF_IOMUX_GPIO2C	0xd0
#define GRF_IOMUX_GPIO2D	0xd4

/*
 * Row stride used by the image-address adder: a hardcoded 1024 pixels at two
 * pixels per byte, independent of the panel width.
 */
#define EBC_ROW_STRIDE		1024

/*
 * Source-driver clock: 306 x 769 x 85 Hz = 20 MHz, which is the figure the
 * panel table carries and what DRM's mode->clock describes.
 *
 * dclk_ebc runs at four times that. The vendor stores 20 MHz in its panel
 * info and then shifts it left by two before clk_set_rate (rk29_ebc_probe:
 * "ldr r1,[r4,#144]; lsl r1,r1,#2"), because DSP_CTRL's fixed
 * SCLK_DIVIDE_RATE field divides the dot clock by four. Programming 20 MHz
 * here would run every waveform at a quarter of its intended frame rate.
 */
#define EBC_PIXEL_RATE		20000000
#define EBC_DCLK_RATE		(EBC_PIXEL_RATE * 4)

/*
 * EBC work modes. These index the controller's mode dispatch and select both
 * the DSP_CTRL class and the waveform table; the numbering is the vendor
 * driver's and is baked into the mode-mask constants below.
 */
#define EBC_MODE_AUTO		0
#define EBC_MODE_FULL		1
#define EBC_MODE_A2		2
#define EBC_MODE_PART		3
#define EBC_MODE_FULL_DITHER	4
#define EBC_MODE_RESET		5
#define EBC_MODE_BLACK_WHITE	6
#define EBC_MODE_TEXT		7
#define EBC_MODE_BLOCK		8
#define EBC_MODE_FULL_WIN	9
#define EBC_MODE_OED_PART	10
#define EBC_MODE_DIRECT_PART	11
#define EBC_MODE_DIRECT_A2	12
#define EBC_MODE_MAX		12

/* Temperatures above this are replaced by 25 degrees before bucket lookup. */
#define EBC_TEMP_MAX		50
#define EBC_TEMP_FALLBACK	25

struct clk;
struct fb_info;
struct fb_ops;
struct firmware;
struct regmap;
struct thermal_zone_device;

/* One queued panel update. */
struct rk3026_ebc_update {
	struct drm_rect	clip;
	u32		mode;
	u32		marker;
	int		temp;
	bool		temp_valid;
};

#define EBC_UPDATE_QUEUE_LEN	8

struct rk3026_ebc {
	struct drm_device	drm;
	struct drm_plane	plane;
	struct drm_crtc		crtc;
	struct drm_encoder	encoder;
	struct drm_connector	connector;

	struct device		*dev;
	void __iomem		*regs;
	struct clk		*hclk;
	struct clk		*dclk;
	struct clk		*aclk;
	/* The EBC's own AXI master port; "aclk" is the video NIU, not this. */
	struct clk		*mclk;
	/* Asserted as dclk's parent after set_rate — see ebc_hw_init(). */
	struct clk		*dclk_parent;
	struct regulator	*epd_supply;
	struct regmap		*grf;
	struct thermal_zone_device *tzd;
	int			irq;

	struct display_timing	timing;
	u32			width;
	u32			height;
	u32			gdck_sta;
	u32			lgonl;
	unsigned int		rotation;

	/* The two 4bpp images the EBC scans: on-panel and target. */
	void			*prev_virt;
	dma_addr_t		prev_phys;
	void			*next_virt;
	dma_addr_t		next_phys;
	size_t			img_size;

	/* Write-only registers the controller does not read back. */
	u32			dsp_start;
	u32			dsp_ctrl;
	u32			int_reg;

	struct completion	frame_done;

	/* Refresh thread and its update queue. */
	struct task_struct	*refresh_thread;
	wait_queue_head_t	update_wq;
	wait_queue_head_t	marker_wq;
	spinlock_t		queue_lock;
	struct rk3026_ebc_update queue[EBC_UPDATE_QUEUE_LEN];
	unsigned int		queue_head;
	unsigned int		queue_tail;
	u32			marker_active;

	/* Serialises writers into @next_virt. */
	struct mutex		blit_lock;

	const struct firmware	*fw;
	struct ebc_waveform	*waveform;

	/* mxcfb shim state; unused when the shim is not built. */
	struct fb_ops		*fbops;
	const struct fb_ops	*orig_fbops;
	atomic_t		fb_users;
	bool			mxcfb_active;
};

static inline struct rk3026_ebc *drm_to_ebc(struct drm_device *drm)
{
	return container_of(drm, struct rk3026_ebc, drm);
}

/* rk3026_ebc.c */
void rk3026_ebc_blit_xrgb8888(struct rk3026_ebc *ebc, const void *src,
			      unsigned int src_pitch, const struct drm_rect *clip);
void rk3026_ebc_wait_idle(struct rk3026_ebc *ebc);
void rk3026_ebc_queue_update(struct rk3026_ebc *ebc, const struct drm_rect *clip,
			     u32 mode, int temp, bool temp_valid, u32 marker);
int rk3026_ebc_wait_marker(struct rk3026_ebc *ebc, u32 marker);

/* rk3026_ebc_waveform.c */
struct ebc_waveform *rk3026_ebc_waveform_parse(struct device *dev,
					       const u8 *buf, size_t len);
const u8 *rk3026_ebc_waveform_lut(struct ebc_waveform *wf, u32 mode, int temp,
				  u32 *frames);

/* rk3026_ebc_mxcfb.c */
#if IS_ENABLED(CONFIG_DRM_RK3026_EBC_MXCFB)
void rk3026_ebc_mxcfb_install(struct rk3026_ebc *ebc, struct fb_info *info);
#else
static inline void rk3026_ebc_mxcfb_install(struct rk3026_ebc *ebc,
					    struct fb_info *info) { }
#endif

#endif /* _RK3026_EBC_H */
