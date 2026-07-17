/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * TPS65185 EPD Power Management IC — platform data and register definitions
 *
 * Copyright (C) 2024 Mario Rugiero
 */

#ifndef __LINUX_MFD_TPS65185_H
#define __LINUX_MFD_TPS65185_H

/* Register addresses */
#define TPS65185_REG_TMST_VALUE		0x00
#define TPS65185_REG_ENABLE		0x01
#define TPS65185_REG_VADJ		0x02
#define TPS65185_REG_VCOM1		0x03
#define TPS65185_REG_VCOM2		0x04
#define TPS65185_REG_INT_EN1		0x05
#define TPS65185_REG_INT_EN2		0x06
#define TPS65185_REG_INT1		0x07
#define TPS65185_REG_INT2		0x08
#define TPS65185_REG_UPSEQ0		0x09
#define TPS65185_REG_UPSEQ1		0x0a
#define TPS65185_REG_DWNSEQ0		0x0b
#define TPS65185_REG_DWNSEQ1		0x0c
#define TPS65185_REG_TMST1		0x0d
#define TPS65185_REG_TMST2		0x0e
#define TPS65185_REG_PG			0x0f
#define TPS65185_REG_REVID		0x10

/* ENABLE register bits */
#define TPS65185_VPOS_EN		BIT(5)
#define TPS65185_VEE_EN			BIT(4)
#define TPS65185_VDDH_EN		BIT(3)
#define TPS65185_VNEG_EN		BIT(2)
#define TPS65185_VCOM_EN		BIT(1)
#define TPS65185_V3P3_EN		BIT(0)

#endif /* __LINUX_MFD_TPS65185_H */
