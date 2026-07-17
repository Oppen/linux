/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip RK3026 EBC (E-Ink Broadcaster) register map.
 *
 * Recovered from the vendor driver:
 *   - register layout: rk3026-linux-sources/.../rk_epd/ebc.h (struct tagEBC_REG)
 *   - timing/frame programming: uudecoded hardware_ebc.o, function
 *     ebc_panel_scan_set() / ebc_dsp_ctl_set() / ebc_frame_start().
 *
 * Base 0x10114000, size 16 KiB, IRQ_EBC 81 (GIC SPI 49). The shared display
 * pads are routed to EBC (vs LCDC) by setting bit0 of GRF register 0x150.
 */
#ifndef _RK3026_EBC_H
#define _RK3026_EBC_H

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

/* EBC_DSP_START (vendor ebc.h m_FRAME_START / m_FRAME_NUM). */
#define DSP_FRAME_START		BIT(0)
#define DSP_FRAME_NUM(x)	(((x) & 0x3f) << 2)

/* EBC_INT (vendor ebc.h: frame-end / frame-flag clear at bits 6 / 8). */
#define EBC_INT_FRAMEEND_STATUS	BIT(0)
#define EBC_INT_FRAMEEND_CLEAR	BIT(6)
#define EBC_INT_FRAMEFLAG_CLEAR	BIT(8)

/* EBC_DSP_CTRL fixed top bits, from ebc_dsp_ctl_set(): 0xC0030000. */
#define DSP_CTRL_FIXED		0xc0030000u

/* Latch value written to EBC_CONFIG_DONE after each register group. */
#define EBC_CONFIG_DONE_VAL	0xffff

/* GRF register that selects EBC vs LCDC on the shared pads (bit0). */
#define GRF_EBC_SEL_REG		0x150
#define GRF_EBC_SEL_BIT		BIT(0)

#endif /* _RK3026_EBC_H */
