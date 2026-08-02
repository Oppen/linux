// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip "rkf" waveform container walker for the RK3026 EBC.
 *
 * Layout, all offsets from the container base:
 *
 *   +0x000  u32 total size; the CRC32 word sits at +size
 *   +0x004  u32 payload start (0xad0 = 0x6d0 + 0x400)
 *   +0x008  magic "rkf:v" + version triplet + build date
 *   +0x01c  vendor tag, "Einks" or "OED"
 *   +0x02c  panel name, ASCII
 *   +0x090  five temperature-boundary tables, 0x40 bytes each
 *   +0x1d0  five u32 offset tables, 0x40 slots each
 *   +0x6d0  256 x u32 Huffman weights, XOR-obfuscated in a pristine file
 *   +0xad0  segmented Huffman payload, one segment per (mode, temperature)
 *
 * A segment is a frame-count byte, three pad bytes, then the bitstream. One
 * Huffman tree, built from the weight table, covers the whole container.
 *
 * Copyright (C) 2026
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "rk3026_ebc.h"

#define RKF_MAX_SIZE		0x40000
#define RKF_WEIGHTS_OFF		0x6d0
#define RKF_PAYLOAD_OFF		0xad0
#define RKF_NSYM		0x100
#define RKF_NNODE		(2 * RKF_NSYM - 1)
#define RKF_ROOT		(RKF_NNODE - 1)

/* Largest LUT the SRAM window can be fed: 255 frames of 0x40 bytes. */
#define RKF_LUT_MAX		(255 * EBC_LUT_FRAME_BYTES)
/* Largest raw segment we decode: mode 7 emits frames << 3. */
#define RKF_RAW_MAX		(255 * 8)

/* Container LUT modes, as selected by epd_lut_data_get(). */
#define RKF_LUT_RESET		1
#define RKF_LUT_GRAY16		2
#define RKF_LUT_MODE6		6
#define RKF_LUT_GRAY2		7
#define RKF_LUT_AUTO		8
#define RKF_LUT_GRAYA2		9

struct ebc_waveform {
	struct device	*dev;
	const u8	*base;
	size_t		size;

	s16		left[RKF_NNODE];
	s16		right[RKF_NNODE];

	/* Cache of the last (mode, temperature) pair, as the vendor keeps. */
	u32		cached_mode;
	int		cached_temp;
	bool		cached_valid;
	u32		frames;

	u8		*lut;	/* frames * EBC_LUT_FRAME_BYTES */
	u8		*raw;	/* Huffman output before the post-pass */
};

/*
 * The container CRC32 is MSB-first with polynomial 0x04c10db7 - the vendor's
 * typo of 0x04c11db7, bit 12 clear. Init 0, no reflection, no final xor, and
 * the extent is [base, base + size) so it covers the size field itself.
 * Computed bitwise: a table would cost 1 KiB for one call per probe.
 */
#define RKF_CRC_POLY		0x04c10db7u

static u32 rkf_crc32(const u8 *p, size_t n, size_t xor_from, size_t xor_to,
		     const u32 *key)
{
	u32 crc = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		u8 b = p[i];
		int k;

		/*
		 * A container read back out of the driver's staging buffer has
		 * already had its weight table deobfuscated in place, so the
		 * stored checksum only closes if the key is reapplied here.
		 */
		if (key && i >= xor_from && i < xor_to) {
			size_t w = (i - xor_from) / 4;

			b ^= (key[w & 1] >> (8 * ((i - xor_from) & 3))) & 0xff;
		}

		crc ^= (u32)b << 24;
		for (k = 0; k < 8; k++)
			crc = (crc << 1) ^ (crc & 0x80000000u ? RKF_CRC_POLY : 0);
	}
	return crc;
}

static u32 rkf_u32(const u8 *p)
{
	return get_unaligned_le32(p);
}

/*
 * Canonical Huffman build: repeatedly merge the two lightest parentless nodes.
 * Only nodes that already exist take part, and ties keep the lower index.
 */
static void rkf_build_tree(struct ebc_waveform *wf, const u32 *weights,
			   u32 *w, s16 *parent)
{
	unsigned int count = RKF_NSYM;
	unsigned int i, k;

	for (i = 0; i < RKF_NNODE; i++) {
		w[i] = i < RKF_NSYM ? weights[i] : 0;
		parent[i] = -1;
		wf->left[i] = -1;
		wf->right[i] = -1;
	}

	for (k = 0; k < RKF_NSYM - 1; k++) {
		u32 m1 = U32_MAX, m2 = U32_MAX;
		unsigned int i1 = 0, i2 = 0;

		for (i = 0; i < count; i++) {
			if (parent[i] != -1)
				continue;
			if (w[i] < m1) {
				m2 = m1;
				i2 = i1;
				m1 = w[i];
				i1 = i;
			} else if (w[i] < m2) {
				m2 = w[i];
				i2 = i;
			}
		}

		parent[i1] = count;
		parent[i2] = count;
		w[count] = w[i1] + w[i2];
		wf->left[count] = i1;
		wf->right[count] = i2;
		count++;
	}
}

/* MSB-first walk from the root; a symbol is the index of the leaf reached. */
static void rkf_decode(struct ebc_waveform *wf, u8 *dst, const u8 *src,
		       size_t n)
{
	unsigned int bit = 0;
	int node = RKF_ROOT;

	while (n) {
		node = (*src & (0x80 >> bit)) ? wf->right[node] : wf->left[node];
		if (++bit == 8) {
			bit = 0;
			src++;
		}
		if (wf->left[node] == -1) {
			*dst++ = node;
			n--;
			node = RKF_ROOT;
		}
	}
}

/*
 * Temperature bucket: clamp to 0..48 (49 for anything hotter), then walk the
 * boundary table. A leading 0x00 is a real boundary, not a terminator - the
 * scan stops when the *next* byte is zero.
 */
static int rkf_temp_bucket(const u8 *tbl, int temp)
{
	int i;

	if (temp < 0)
		temp = 0;
	else if (temp > 0x30)
		temp = 0x31;

	for (i = 0; i < 0x32; i++) {
		if (temp < tbl[i])
			return i ? i - 1 : 0;
		if (!tbl[i + 1])
			return i;
	}
	return 0x31;
}

/* Per-LUT-mode table bases, as read out of epd_lut_data_get(). */
struct rkf_mode_desc {
	u16 temp_off;
	u16 offs_off;
};

static const struct rkf_mode_desc rkf_modes[] = {
	[RKF_LUT_RESET]  = { 0x090, 0x1d0 },
	[RKF_LUT_GRAY16] = { 0x0d0, 0x2d0 },
	[RKF_LUT_MODE6]  = { 0x150, 0x4d0 },
	[RKF_LUT_GRAY2]  = { 0x190, 0x5d0 },
	[RKF_LUT_GRAYA2] = { 0x110, 0x3d0 },
};

/* Locate a segment; returns its base or NULL when the mode is absent. */
static const u8 *rkf_segment(struct ebc_waveform *wf, u32 mode, int temp,
			     u32 *frames)
{
	const struct rkf_mode_desc *d;
	const u8 *seg;
	u32 off;
	int idx;

	if (mode >= ARRAY_SIZE(rkf_modes) || !rkf_modes[mode].temp_off)
		return NULL;
	d = &rkf_modes[mode];

	idx = rkf_temp_bucket(wf->base + d->temp_off, temp);
	off = rkf_u32(wf->base + d->offs_off + idx * 4);

	/* A zero slot means the mode is not present in this container. */
	if (off < RKF_PAYLOAD_OFF || off >= wf->size)
		return NULL;

	seg = wf->base + off;
	*frames = seg[0];
	return *frames ? seg : NULL;
}

/*
 * rst_format_change(): every 2-bit field of the decoded stream is one reset
 * frame, expanded to a full 0x40-byte frame of that same value.
 */
static void rkf_expand_reset(u8 *dst, const u8 *src, u32 frames)
{
	u32 f;

	for (f = 0; f < frames; f++) {
		u32 phase = (src[f / 4] >> (2 * (f % 4))) & 3;
		u32 word = phase * 0x55555555u;
		u32 i;

		for (i = 0; i < EBC_LUT_FRAME_BYTES / 4; i++)
			put_unaligned_le32(word, dst + f * EBC_LUT_FRAME_BYTES + i * 4);
	}
}

/*
 * gray2_data_change(): two source words per frame supply the low and the high
 * 2-bit field of each of the 16 LUT words.
 */
static void rkf_expand_gray2(u8 *dst, const u8 *src, u32 frames)
{
	u32 f;

	for (f = 0; f < frames; f++) {
		u32 w0 = rkf_u32(src + f * 8);
		u32 w1 = rkf_u32(src + f * 8 + 4);
		u32 i;

		for (i = 0; i < 16; i++) {
			u32 v = ((w0 >> (2 * i)) & 3) | ((w1 >> (2 * i)) << 30);

			put_unaligned_le32(v, dst + f * EBC_LUT_FRAME_BYTES + i * 4);
		}
	}
}

/*
 * auto_data_change(): overlay a gray2 stream onto an already decoded gray16
 * LUT, replacing only the outermost grey levels.
 */
static void rkf_overlay_gray2(u8 *dst, const u8 *src, u32 frames)
{
	u32 f;

	for (f = 0; f < frames; f++) {
		u32 w0 = rkf_u32(src + f * 8);
		u32 w1 = rkf_u32(src + f * 8 + 4);
		u32 i;

		for (i = 0; i < 16; i++) {
			u8 *p = dst + f * EBC_LUT_FRAME_BYTES + i * 4;
			u32 v = rkf_u32(p) & 0x3ffffffcu;

			v |= ((w0 >> (2 * i)) & 3) | ((w1 >> (2 * i)) << 30);
			put_unaligned_le32(v, p);
		}
	}
}

static int rkf_load_direct(struct ebc_waveform *wf, u32 mode, int temp,
			   u32 *frames)
{
	const u8 *seg = rkf_segment(wf, mode, temp, frames);

	if (!seg)
		return -ENOENT;
	if (*frames > RKF_LUT_MAX / EBC_LUT_FRAME_BYTES)
		return -EINVAL;

	rkf_decode(wf, wf->lut, seg + 4, *frames * EBC_LUT_FRAME_BYTES);
	return 0;
}

static int rkf_load_gray2(struct ebc_waveform *wf, int temp, u32 *frames)
{
	const u8 *seg = rkf_segment(wf, RKF_LUT_GRAY2, temp, frames);

	if (!seg)
		return -ENOENT;
	if (*frames > RKF_RAW_MAX / 8)
		return -EINVAL;

	rkf_decode(wf, wf->raw, seg + 4, *frames * 8);
	rkf_expand_gray2(wf->lut, wf->raw, *frames);
	return 0;
}

static int rkf_load_lut(struct ebc_waveform *wf, u32 mode, int temp,
			u32 *frames)
{
	const u8 *seg;
	u32 n;
	int ret;

	switch (mode) {
	case RKF_LUT_RESET:
		seg = rkf_segment(wf, RKF_LUT_RESET, temp, frames);
		if (!seg)
			return -ENOENT;
		if (*frames > RKF_LUT_MAX / EBC_LUT_FRAME_BYTES)
			return -EINVAL;
		rkf_decode(wf, wf->raw, seg + 4, (*frames + 3) >> 2);
		rkf_expand_reset(wf->lut, wf->raw, *frames);
		return 0;

	case RKF_LUT_MODE6:
		ret = rkf_load_direct(wf, RKF_LUT_MODE6, temp, frames);
		if (!ret)
			return 0;
		return rkf_load_direct(wf, RKF_LUT_GRAY16, temp, frames);

	case RKF_LUT_GRAYA2:
		ret = rkf_load_direct(wf, RKF_LUT_GRAYA2, temp, frames);
		if (!ret)
			return 0;
		return rkf_load_gray2(wf, temp, frames);

	case RKF_LUT_GRAY2:
		return rkf_load_gray2(wf, temp, frames);

	case RKF_LUT_AUTO:
		ret = rkf_load_direct(wf, RKF_LUT_GRAY16, temp, frames);
		if (ret)
			return ret;
		seg = rkf_segment(wf, RKF_LUT_GRAY2, temp, &n);
		if (!seg || n > *frames || n > RKF_RAW_MAX / 8)
			return 0;
		rkf_decode(wf, wf->raw, seg + 4, n * 8);
		rkf_overlay_gray2(wf->lut, wf->raw, n);
		return 0;

	default:
		return rkf_load_direct(wf, RKF_LUT_GRAY16, temp, frames);
	}
}

/* EBC work mode to container LUT mode, as dispatched by ebc_register_update(). */
static u32 rkf_lut_mode(u32 ebc_mode)
{
	switch (ebc_mode) {
	case EBC_MODE_AUTO:
		return RKF_LUT_AUTO;
	case EBC_MODE_A2:
	case EBC_MODE_DIRECT_A2:
		return RKF_LUT_GRAYA2;
	case EBC_MODE_RESET:
		return RKF_LUT_RESET;
	case EBC_MODE_BLACK_WHITE:
		return RKF_LUT_GRAY2;
	case EBC_MODE_OED_PART:
		return RKF_LUT_MODE6;
	default:
		return RKF_LUT_GRAY16;
	}
}

/**
 * rk3026_ebc_waveform_lut - fetch the packed LUT for one mode and temperature
 * @wf: parsed container
 * @mode: EBC work mode
 * @temp: panel temperature in degrees Celsius
 * @frames: number of waveform frames, on success
 *
 * Returns a pointer to @frames * 0x40 bytes ready for the LUT SRAM window, or
 * NULL if the container has no waveform for this mode.
 */
const u8 *rk3026_ebc_waveform_lut(struct ebc_waveform *wf, u32 mode, int temp,
				  u32 *frames)
{
	u32 lut_mode = rkf_lut_mode(mode);
	int ret;

	if (wf->cached_valid && wf->cached_mode == lut_mode &&
	    wf->cached_temp == temp) {
		*frames = wf->frames;
		return wf->lut;
	}

	wf->cached_valid = false;
	ret = rkf_load_lut(wf, lut_mode, temp, &wf->frames);
	if (ret) {
		dev_warn_ratelimited(wf->dev,
				     "no waveform for mode %u at %d C (%d)\n",
				     mode, temp, ret);
		return NULL;
	}

	wf->cached_mode = lut_mode;
	wf->cached_temp = temp;
	wf->cached_valid = true;
	*frames = wf->frames;
	return wf->lut;
}

/**
 * rk3026_ebc_waveform_parse - validate a container and build its Huffman tree
 * @dev: device to allocate against and to log through
 * @buf: container bytes, including the trailing CRC32 word
 * @len: length of @buf
 */
struct ebc_waveform *rk3026_ebc_waveform_parse(struct device *dev,
					       const u8 *buf, size_t len)
{
	/* Checkerboard key; the phase is chosen by the vendor tag. */
	static const u32 key_einks[2] = { 0xaa55aa55, 0x55aa55aa };
	static const u32 key_oed[2] = { 0x55aa55aa, 0xaa55aa55 };
	const u32 *key = key_einks;
	struct ebc_waveform *wf;
	u32 *weights, *work;
	s16 *parent;
	bool plain;
	size_t size;
	u32 crc;
	int i;

	if (len < RKF_PAYLOAD_OFF + 4)
		return ERR_PTR(-EINVAL);

	size = rkf_u32(buf);
	if (size > RKF_MAX_SIZE || size + 4 > len || size <= RKF_PAYLOAD_OFF)
		return ERR_PTR(-EINVAL);

	if (memcmp(buf + 8, "rkf:v", 5))
		return ERR_PTR(-EINVAL);

	if (!memcmp(buf + 0x1c, "Einks", 5))
		key = key_einks;
	else if (!memcmp(buf + 0x1c, "OED", 3))
		key = key_oed;
	else
		return ERR_PTR(-EINVAL);

	wf = devm_kzalloc(dev, sizeof(*wf), GFP_KERNEL);
	if (!wf)
		return ERR_PTR(-ENOMEM);
	wf->dev = dev;
	wf->base = buf;
	wf->size = size;

	weights = kmalloc_array(RKF_NSYM, sizeof(*weights), GFP_KERNEL);
	if (!weights)
		return ERR_PTR(-ENOMEM);

	/*
	 * A weight is a symbol count over at most 0x40000 payload bytes, so a
	 * word of 2^28 or more means the table is still obfuscated.
	 */
	plain = true;
	for (i = 0; i < RKF_NSYM; i++) {
		weights[i] = rkf_u32(buf + RKF_WEIGHTS_OFF + i * 4);
		if (weights[i] >= (1u << 28))
			plain = false;
	}

	crc = rkf_crc32(buf, size, RKF_WEIGHTS_OFF, RKF_PAYLOAD_OFF,
			plain ? key : NULL);
	if (crc != rkf_u32(buf + size)) {
		dev_err(dev, "waveform CRC mismatch: got %08x want %08x\n",
			crc, rkf_u32(buf + size));
		kfree(weights);
		return ERR_PTR(-EINVAL);
	}

	if (!plain)
		for (i = 0; i < RKF_NSYM; i++)
			weights[i] ^= key[i & 1];

	work = kmalloc_array(RKF_NNODE, sizeof(*work) + sizeof(*parent),
			     GFP_KERNEL);
	if (!work) {
		kfree(weights);
		return ERR_PTR(-ENOMEM);
	}
	parent = (s16 *)(work + RKF_NNODE);

	rkf_build_tree(wf, weights, work, parent);
	kfree(work);
	kfree(weights);

	wf->lut = devm_kmalloc(dev, RKF_LUT_MAX, GFP_KERNEL);
	wf->raw = devm_kmalloc(dev, RKF_RAW_MAX, GFP_KERNEL);
	if (!wf->lut || !wf->raw)
		return ERR_PTR(-ENOMEM);

	dev_info(dev, "waveform %.11s, panel %.32s, %zu bytes\n",
		 buf + 8, buf + 0x2c, size);
	return wf;
}
