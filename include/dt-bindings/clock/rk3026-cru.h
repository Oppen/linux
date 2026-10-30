/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2024-2026
 * Device Tree binding constants for RK3026 Clock and Reset Unit (CRU)
 *
 * Extracted from vendor kernel: rychly/rk3026-linux-sources
 * Based on: arch/arm/mach-rk3026/clock_data.c
 *           arch/arm/mach-rk3026/include/mach/cru.h
 */

#ifndef _DT_BINDINGS_CLOCK_ROCKCHIP_RK3026_H
#define _DT_BINDINGS_CLOCK_ROCKCHIP_RK3026_H

/* PLLs */
#define PLL_APLL		0
#define PLL_DPLL		1
#define PLL_CPLL		2
#define PLL_GPLL		3

/* Core clocks */
#define ARMCLK			4
#define CORE_PERI		5
#define CORE_PERIPH		5  /* Alias */

/* AXI/AHB/APB Bus clocks */
#define ACLK_CPU		6
#define HCLK_CPU		7
#define PCLK_CPU		8

/* Peripheral clocks */
#define ACLK_PERI		9
#define HCLK_PERI		10
#define PCLK_PERI		11

/* UART clocks */
#define SCLK_UART0		12
#define SCLK_UART1		13
#define SCLK_UART2		14
#define PCLK_UART0		15
#define PCLK_UART1		16
#define PCLK_UART2		17

/* I2C clocks */
#define PCLK_I2C0		18
#define PCLK_I2C1		19
#define PCLK_I2C2		20
#define PCLK_I2C3		21

/* SPI clocks */
#define SCLK_SPI		22
#define PCLK_SPI		23

/* PWM clocks */
#define PCLK_PWM		24

/* Timer clocks */
#define PCLK_TIMER0		25
#define PCLK_TIMER1		26

/* GPIO clocks */
#define PCLK_GPIO0		27
#define PCLK_GPIO1		28
#define PCLK_GPIO2		29
#define PCLK_GPIO3		30

/* SDMMC/SDIO/eMMC clocks */
#define HCLK_SDMMC		31
#define HCLK_SDIO		32
#define HCLK_EMMC		33
#define SCLK_SDMMC		34
#define SCLK_SDIO		35
#define SCLK_EMMC		36

/* USB clocks */
#define HCLK_OTG		37
#define HCLK_HOST		38
#define USBPHY_480M		39

/* Display clocks */
#define ACLK_LCDC0		40
#define ACLK_LCDC1		41
#define HCLK_LCDC0		42
#define HCLK_LCDC1		43
#define DCLK_LCDC0		44
#define DCLK_LCDC1		45

/* EBC (E-Paper Display Controller) clocks */
#define ACLK_EBC		46
#define HCLK_EBC		47
#define DCLK_EBC		48

/* VOP (Video Output Processor) clocks */
#define ACLK_VOP		49
#define HCLK_VOP		50

/* Video codec clocks */
#define ACLK_VCODEC		51
#define HCLK_VCODEC		52

/* GPU clocks */
#define ACLK_GPU		53
#define HCLK_GPU		54

/* CIF (Camera Interface) clocks */
#define ACLK_CIF		55
#define HCLK_CIF		56
#define PCLK_CIF		57

/* RGA (Raster Graphic Accelerator) clocks */
#define ACLK_RGA		58
#define HCLK_RGA		59

/* IEP (Image Enhancement Processor) clocks */
#define ACLK_IEP		60
#define HCLK_IEP		61

/* I2S clocks */
#define SCLK_I2S		62
#define HCLK_I2S		63

/* SARADC clocks */
#define SCLK_SARADC		64
#define PCLK_SARADC		65

/* TSADC clocks */
#define SCLK_TSADC		66
#define PCLK_TSADC		67

/* Watchdog clocks */
#define PCLK_WDT		68

/* HDMI clocks */
#define PCLK_HDMI		69

/* Crypto clocks */
#define ACLK_CRYPTO		70
#define HCLK_CRYPTO		71

/* NAND clocks */
#define HCLK_NANDC		72

/* DMAC clocks */
#define ACLK_DMAC		73

/* GRF clocks */
#define PCLK_GRF		74

/* PMU clocks */
#define PCLK_PMU		75

/* DDR clocks */
#define PCLK_DDRUPCTL		76
#define PCLK_DDRPHY		77

/* ROM clock */
#define HCLK_ROM		78

/* Audio codec pclk */
#define PCLK_ACODEC		79

/* MMC phase clocks (drive and sample) */
#define SCLK_SDMMC_DRV		80
#define SCLK_SDMMC_SAMPLE	81
#define SCLK_SDIO_DRV		82
#define SCLK_SDIO_SAMPLE	83
#define SCLK_EMMC_DRV		84
#define SCLK_EMMC_SAMPLE	85

/* Display AXI source, exposed so the board can pin its parent and rate. */
#define ACLK_LCDC1_SRC		86

/*
 * Display AHB source: the real parent of hclk_ebc and hclk_lcdc1, on
 * CLKSEL_CON30. Nothing in this port wrote that register until 2026-08-06, so
 * hclk_ebc ran at whatever the loader left -- slow-mode CPLL / 4 = 6 MHz,
 * against the vendor's 256 -- and that, not the AXI clock, was what capped
 * dclk_ebc at 12 MHz.
 */
#define HCLK_DISP_PRE		87

/* Number of clocks */
#define CLK_NR_CLKS		(HCLK_DISP_PRE + 1)

/*
 * Soft Resets
 *
 * Ids are 16 * CON + bit: the reset driver maps an id to
 * CRU_SOFTRST_CON(id / 16), bit (id % 16). Names follow the vendor
 * enum cru_soft_reset; unnamed bits are reserved.
 */

/* SOFTRST_CON0 */
#define SRST_CORE_SRST_WDT_SEL	0
#define SRST_MCORE		2
#define SRST_CORE0		3
#define SRST_CORE1		4
#define SRST_MCORE_DBG		7
#define SRST_CORE0_DBG		8
#define SRST_CORE1_DBG		9
#define SRST_CORE0_WDT		10
#define SRST_CORE1_WDT		11
#define SRST_ACLK_CORE		13
#define SRST_STRC_SYS_AXI	14
#define SRST_L2C		15

/* SOFTRST_CON1 */
#define SRST_CPUSYS_AHB		17
#define SRST_L2MEM_CON_AXI	18
#define SRST_AHB2APB		19
#define SRST_INTMEM		21
#define SRST_ROM		22
#define SRST_PERI_NIU		23
#define SRST_I2S		24
#define SRST_DDR_DLL		25
#define SRST_GPU_DLL		26
#define SRST_TIMER0		27
#define SRST_TIMER1		28
#define SRST_CORE_DLL		29
#define SRST_EFUSE_APB		30
#define SRST_ACODEC		31

/* SOFTRST_CON2 */
#define SRST_GPIO0		32
#define SRST_GPIO1		33
#define SRST_GPIO2		34
#define SRST_GPIO3		35
#define SRST_UART0		39
#define SRST_UART1		40
#define SRST_UART2		41
#define SRST_I2C0		43
#define SRST_I2C1		44
#define SRST_I2C2		45
#define SRST_I2C3		46

/* SOFTRST_CON3 */
#define SRST_PWM0		48
#define SRST_PWM1		49
#define SRST_DAP_PO		50
#define SRST_DAP		51
#define SRST_DAP_SYS		52
#define SRST_CRYPTO		53
#define SRST_GRF		55
#define SRST_PERIPHSYS_AXI	57
#define SRST_PERIPHSYS_AHB	58
#define SRST_PERIPHSYS_APB	59
#define SRST_PWM2		60
#define SRST_CPU_PERI		61
#define SRST_EMEM_PERI		62
#define SRST_USB_PERI		63

/* SOFTRST_CON4 */
#define SRST_DMA2		64
#define SRST_GPS		67
#define SRST_NANDC		68
#define SRST_USBOTG0		69
#define SRST_OTGC0		71
#define SRST_USBOTG1		72
#define SRST_OTGC1		74
#define SRST_DDRMSCH		79

/* SOFTRST_CON5 */
#define SRST_MMC0		81
#define SRST_SDIO		82
#define SRST_EMMC		83
#define SRST_SPI0		84
#define SRST_WDT		86
#define SRST_SARADC		87
#define SRST_DDRPHY		88
#define SRST_DDRPHY_APB		89
#define SRST_DDRCTRL		90
#define SRST_DDRCTRL_APB	91

/* SOFTRST_CON6 */
#define SRST_HDMI_PCLK		96
#define SRST_VIO_ARBI_AHB	97
#define SRST_VIO0_AXI		98
#define SRST_VIO_BUS_AHB	99
#define SRST_LCDC0_AXI		100
#define SRST_LCDC0_AHB		101
#define SRST_LCDC0_DCLK		102
#define SRST_UTMI0		103
#define SRST_UTMI1		104
#define SRST_USBPOR		105
#define SRST_IEP_AXI		106
#define SRST_IEP_AHB		107
#define SRST_RGA_AXI		108
#define SRST_RGA_AHB		109
#define SRST_CIF0		110

/* SOFTRST_CON7 */
#define SRST_VCODEC_AXI		112
#define SRST_VCODEC_AHB		113
#define SRST_VIO1_AXI		114
#define SRST_VCODEC_NIU_AXI	116
#define SRST_LCDC1_AXI		117
#define SRST_LCDC1_AHB		118
#define SRST_LCDC1_DCLK		119
#define SRST_GPU		120
#define SRST_GPU_NIU_AXI	122
#define SRST_EBC_AXI		123
#define SRST_EBC_AHB		124

/* SOFTRST_CON8 */
#define SRST_CORE_DBG		130
#define SRST_DBG_APB		131

#define SRST_NR_RESETS		(SRST_DBG_APB + 1)

#endif /* _DT_BINDINGS_CLOCK_ROCKCHIP_RK3026_H */
