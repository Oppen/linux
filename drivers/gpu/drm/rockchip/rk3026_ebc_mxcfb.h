/* SPDX-License-Identifier: GPL-2.0 */
/*
 * i.MX EPDC ("mxcfb") compatibility ABI for the RK3026 EBC.
 *
 * Four incompatible variants of this interface are in the wild: V1, V1_NTX,
 * V2 and V3. They share ioctl type 'F' and command number 0x2e and differ
 * only in the tail of the payload, so _IOC_SIZE is what tells them apart.
 *
 * This shim implements **V1**, the original i.MX5 layout:
 *
 *   MXCFB_SEND_UPDATE                _IOW('F', 0x2e, struct mxcfb_update_data)
 *   MXCFB_WAIT_FOR_UPDATE_COMPLETE   _IOW('F', 0x2f, __u32)
 *
 * V1 is chosen because it is the smallest variant that still carries
 * everything the EBC needs (region, waveform mode, update mode, marker,
 * temperature), it has no 64-bit-unsafe userspace pointer in it (V1_NTX
 * embeds a void *), and it carries no dither or quantisation fields that this
 * controller cannot honour. The struct and the two command numbers match
 * KOReader's mxcfb_kobo_h.lua definitions byte for byte
 * (MXCFB_SEND_UPDATE_V1 = 0x4040462e, MXCFB_WAIT_FOR_UPDATE_COMPLETE_V1 =
 * 0x4004462f, sizeof(struct mxcfb_update_data_v1) == 64).
 */
#ifndef _RK3026_EBC_MXCFB_H
#define _RK3026_EBC_MXCFB_H

#include <linux/build_bug.h>
#include <linux/ioctl.h>
#include <linux/types.h>

struct mxcfb_rect {
	__u32 top;
	__u32 left;
	__u32 width;
	__u32 height;
};

struct mxcfb_alt_buffer_data {
	__u32 phys_addr;
	__u32 width;
	__u32 height;
	struct mxcfb_rect alt_update_region;
};

struct mxcfb_update_data {
	struct mxcfb_rect update_region;
	__u32 waveform_mode;
	__u32 update_mode;
	__u32 update_marker;
	__s32 temp;
	__u32 flags;
	struct mxcfb_alt_buffer_data alt_buffer_data;
};

/* The size is part of the ioctl number, so it also picks the variant. */
static_assert(sizeof(struct mxcfb_update_data) == 64);

#define MXCFB_SEND_UPDATE		_IOW('F', 0x2e, struct mxcfb_update_data)
#define MXCFB_WAIT_FOR_UPDATE_COMPLETE	_IOW('F', 0x2f, __u32)

/* update_mode */
#define MXCFB_UPDATE_MODE_PARTIAL	0
#define MXCFB_UPDATE_MODE_FULL		1

/* waveform_mode */
#define MXCFB_WAVEFORM_MODE_INIT	0
#define MXCFB_WAVEFORM_MODE_DU		1
#define MXCFB_WAVEFORM_MODE_GC16	2
#define MXCFB_WAVEFORM_MODE_GC4		3
#define MXCFB_WAVEFORM_MODE_A2		4
#define MXCFB_WAVEFORM_MODE_GL16	5
#define MXCFB_WAVEFORM_MODE_REAGL	6
#define MXCFB_WAVEFORM_MODE_REAGLD	7
#define MXCFB_WAVEFORM_MODE_AUTO	257

/* temp: read the panel thermometer instead of using the supplied value */
#define MXCFB_TEMP_USE_AMBIENT		0x1000

#endif /* _RK3026_EBC_MXCFB_H */
