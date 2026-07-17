/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Rockchip RK3026 internal audio codec register definitions
 *
 * Copyright (c) 2024, Mario Rugiero
 */

#ifndef _RK3026_CODEC_H
#define _RK3026_CODEC_H

/* Register map */
#define RK3026_RESET			0x00
#define RK3026_ADC_INT_CTL1		0x08
#define RK3026_ADC_INT_CTL2		0x0c
#define RK3026_DAC_INT_CTL1		0x10
#define RK3026_DAC_INT_CTL2		0x14
#define RK3026_DAC_INT_CTL3		0x18
#define RK3026_ADC_MIC_CTL		0x88
#define RK3026_BST_CTL			0x8c
#define RK3026_ALC_MUNIN_CTL		0x90
#define RK3026_BSTL_ALCL_CTL		0x94
#define RK3026_ALCR_GAIN_CTL		0x98
#define RK3026_ADC_ENABLE		0x9c
#define RK3026_DAC_CTL			0xa0
#define RK3026_DAC_ENABLE		0xa4
#define RK3026_HPMIX_CTL		0xa8
#define RK3026_HPMIX_S_SELECT		0xac
#define RK3026_HPOUT_CTL		0xb0
#define RK3026_HPOUTL_GAIN		0xb4
#define RK3026_HPOUTR_GAIN		0xb8
#define RK3026_SELECT_CURRENT		0xbc
#define RK3026_PGAL_AGC_CTL1		0x100
#define RK3026_PGAL_AGC_CTL5		0x124
#define RK3026_PGAR_AGC_CTL1		0x140
#define RK3026_PGAR_AGC_CTL5		0x164

#define RK3026_MAX_REGISTER		RK3026_PGAR_AGC_CTL5

/*
 * RK3026_RESET (0x00)
 */
#define RK3026_RESET_VAL		0x00
#define RK3026_NORMAL_VAL		0x03

/*
 * RK3026_ADC_INT_CTL2 (0x0c)
 */
#define RK3026_I2S_MODE_MSK		BIT(4)
#define RK3026_I2S_MODE_MST		BIT(4)	/* 1 = master */
#define RK3026_I2S_MODE_SLV		0
#define RK3026_ADC_WL_MSK		(0x3 << 2)
#define RK3026_ADC_WL_16		(0x0 << 2)
#define RK3026_ADC_WL_20		(0x1 << 2)
#define RK3026_ADC_WL_24		(0x2 << 2)
#define RK3026_ADC_WL_32		(0x3 << 2)

/*
 * RK3026_DAC_INT_CTL1 (0x10)
 */
#define RK3026_DLRCK_POL_MSK		BIT(7)
#define RK3026_DLRCK_POL_INV		BIT(7)
#define RK3026_DLRCK_POL_NORM		0
#define RK3026_DAC_VWL_MSK		(0x3 << 5)
#define RK3026_DAC_VWL_16		(0x0 << 5)
#define RK3026_DAC_VWL_20		(0x1 << 5)
#define RK3026_DAC_VWL_24		(0x2 << 5)
#define RK3026_DAC_VWL_32		(0x3 << 5)
#define RK3026_DAC_DF_MSK		(0x3 << 3)
#define RK3026_DAC_DF_RJ		(0x0 << 3)
#define RK3026_DAC_DF_LJ		(0x1 << 3)
#define RK3026_DAC_DF_I2S		(0x2 << 3)
#define RK3026_DAC_DF_PCM		(0x3 << 3)

/*
 * RK3026_ADC_ENABLE (0x9c)
 */
#define RK3026_ADCL_EN			BIT(7)
#define RK3026_ADCL_WORK		BIT(6)
#define RK3026_ADCR_EN			BIT(3)
#define RK3026_ADCR_WORK		BIT(2)

/*
 * RK3026_DAC_ENABLE (0xa4)
 */
#define RK3026_DACL_EN			BIT(7)
#define RK3026_DACL_WORK		BIT(6)
#define RK3026_DACR_EN			BIT(3)
#define RK3026_DACR_WORK		BIT(2)

/*
 * RK3026_HPMIX_CTL (0xa8)
 */
#define RK3026_HPMIXL_EN		BIT(7)
#define RK3026_HPMIXL_WORK1		BIT(6)
#define RK3026_HPMIXL_WORK2		BIT(5)
#define RK3026_HPMIXR_EN		BIT(3)
#define RK3026_HPMIXR_WORK1		BIT(2)
#define RK3026_HPMIXR_WORK2		BIT(1)

/*
 * RK3026_HPMIX_S_SELECT (0xac) — route DAC L/R into mixer
 */
#define RK3026_HPMIXL_DAC_L		BIT(3)
#define RK3026_HPMIXR_DAC_R		BIT(0)

/*
 * RK3026_HPOUT_CTL (0xb0)
 */
#define RK3026_HPVREF_WORK		BIT(7)
#define RK3026_HPVREF_EN		BIT(6)
#define RK3026_HPOUTL_EN		BIT(5)
#define RK3026_HPOUTL_WORK		BIT(4)
#define RK3026_HPOUTR_EN		BIT(3)
#define RK3026_HPOUTR_WORK		BIT(2)
#define RK3026_HPOUTL_UNMUTE		BIT(1)	/* 1 = unmute */
#define RK3026_HPOUTR_UNMUTE		BIT(0)

/*
 * RK3026_HPOUTL_GAIN / RK3026_HPOUTR_GAIN (0xb4, 0xb8)
 * bits [4:0]: 0x00 = -39 dB, 0x1a = 0 dB, 0x1f = +6 dB, step 1.5 dB
 */
#define RK3026_HP_GAIN_MSK		0x1f
#define RK3026_HP_GAIN_0DB		0x1a
#define RK3026_HP_GAIN_MIN		0x00

#endif /* _RK3026_CODEC_H */
