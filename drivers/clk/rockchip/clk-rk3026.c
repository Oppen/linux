// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2014 MundoReader S.L.
 * Author: Heiko Stuebner <heiko@sntech.de>
 *
 * Copyright (c) 2024 Mario Rugiero
 * Author: Mario Rugiero <mario.rugiero@lambdaclass.com>
 *
 * Clock driver for the Rockchip RK3026 SoC.
 * Based on clk-rk3036.c and vendor kernel arch/arm/mach-rk3026/clock_data.c.
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/reboot.h>
#include <linux/syscore_ops.h>
#include <dt-bindings/clock/rk3026-cru.h>
#include "clk.h"

#define RK3026_GRF_SOC_STATUS0	0x14c

enum rk3026_plls {
	apll, dpll, cpll, gpll,
};

static struct rockchip_pll_rate_table rk3026_pll_rates[] = {
	/* _mhz, _refdiv, _fbdiv, _postdiv1, _postdiv2, _dsmpd, _frac */
	RK3036_PLL_RATE(1608000000, 1, 67, 1, 1, 1, 0),
	RK3036_PLL_RATE(1584000000, 1, 66, 1, 1, 1, 0),
	RK3036_PLL_RATE(1560000000, 1, 65, 1, 1, 1, 0),
	RK3036_PLL_RATE(1536000000, 1, 64, 1, 1, 1, 0),
	RK3036_PLL_RATE(1512000000, 1, 63, 1, 1, 1, 0),
	RK3036_PLL_RATE(1488000000, 1, 62, 1, 1, 1, 0),
	RK3036_PLL_RATE(1464000000, 1, 61, 1, 1, 1, 0),
	RK3036_PLL_RATE(1440000000, 1, 60, 1, 1, 1, 0),
	RK3036_PLL_RATE(1416000000, 1, 59, 1, 1, 1, 0),
	RK3036_PLL_RATE(1392000000, 1, 58, 1, 1, 1, 0),
	RK3036_PLL_RATE(1368000000, 1, 57, 1, 1, 1, 0),
	RK3036_PLL_RATE(1344000000, 1, 56, 1, 1, 1, 0),
	RK3036_PLL_RATE(1320000000, 1, 55, 1, 1, 1, 0),
	RK3036_PLL_RATE(1296000000, 1, 54, 1, 1, 1, 0),
	RK3036_PLL_RATE(1272000000, 1, 53, 1, 1, 1, 0),
	RK3036_PLL_RATE(1248000000, 1, 52, 1, 1, 1, 0),
	RK3036_PLL_RATE(1200000000, 1, 50, 1, 1, 1, 0),
	RK3036_PLL_RATE(1188000000, 2, 99, 1, 1, 1, 0),
	RK3036_PLL_RATE(1104000000, 1, 46, 1, 1, 1, 0),
	RK3036_PLL_RATE(1100000000, 12, 550, 1, 1, 1, 0),
	RK3036_PLL_RATE(1008000000, 1, 84, 2, 1, 1, 0),
	RK3036_PLL_RATE(1000000000, 6, 500, 2, 1, 1, 0),
	RK3036_PLL_RATE( 984000000, 1, 82, 2, 1, 1, 0),
	RK3036_PLL_RATE( 960000000, 1, 80, 2, 1, 1, 0),
	RK3036_PLL_RATE( 936000000, 1, 78, 2, 1, 1, 0),
	RK3036_PLL_RATE( 912000000, 1, 76, 2, 1, 1, 0),
	RK3036_PLL_RATE( 900000000, 4, 300, 2, 1, 1, 0),
	RK3036_PLL_RATE( 888000000, 1, 74, 2, 1, 1, 0),
	RK3036_PLL_RATE( 864000000, 1, 72, 2, 1, 1, 0),
	RK3036_PLL_RATE( 840000000, 1, 70, 2, 1, 1, 0),
	RK3036_PLL_RATE( 816000000, 1, 68, 2, 1, 1, 0),
	RK3036_PLL_RATE( 800000000, 6, 400, 2, 1, 1, 0),
	RK3036_PLL_RATE( 700000000, 6, 350, 2, 1, 1, 0),
	RK3036_PLL_RATE( 696000000, 1, 58, 2, 1, 1, 0),
	RK3036_PLL_RATE( 600000000, 1, 75, 3, 1, 1, 0),
	RK3036_PLL_RATE( 594000000, 2, 99, 2, 1, 1, 0),
	RK3036_PLL_RATE( 504000000, 1, 63, 3, 1, 1, 0),
	RK3036_PLL_RATE( 500000000, 6, 250, 2, 1, 1, 0),
	RK3036_PLL_RATE( 408000000, 1, 68, 2, 2, 1, 0),
	RK3036_PLL_RATE( 312000000, 1, 52, 2, 2, 1, 0),
	RK3036_PLL_RATE( 216000000, 1, 72, 4, 2, 1, 0),
	RK3036_PLL_RATE(  96000000, 1, 64, 4, 4, 1, 0),
	{ /* sentinel */ },
};

/*
 * GPLL rate table. Both entries match the vendor's gpll_clks at 0xc08af258.
 */
static struct rockchip_pll_rate_table rk3026_gpll_clks[] = {
	RK3036_PLL_RATE(768000000, 1, 32, 1, 1, 1, 0),
	RK3036_PLL_RATE(297000000, 2, 99, 4, 1, 1, 0),
	{ /* sentinel */ },
};

/*
 * CPLL rate table, from the vendor's cpll_clks at 0xc08af280 (798/594/1064),
 * plus 560 MHz which is what this device actually runs.
 *
 * 560 is not in the vendor table because the vendor does not use a table here:
 * dclk_ebc's set_rate op (clksel_set_ebc_rate_freediv, 0xc0456cc4) picks a
 * divider from the requested rate -- 7 for anything <= 80 MHz -- then sweeps a
 * candidate CPLL rate from 700 MHz down to 400 MHz in 10 kHz steps until
 * cand/div matches, and sets CPLL to it. 80 MHz x 7 = 560 MHz, which is the
 * live CPLL rate. Binding CPLL to the GPLL table, as an earlier revision did,
 * would move it somewhere uncharacterised on the first EBC set_rate.
 */
static struct rockchip_pll_rate_table rk3026_cpll_clks[] = {
	RK3036_PLL_RATE(1064000000, 1, 133, 3, 1, 1, 0),
	RK3036_PLL_RATE( 798000000, 1, 133, 4, 1, 1, 0),
	RK3036_PLL_RATE( 594000000, 1,  99, 4, 1, 1, 0),
	RK3036_PLL_RATE( 560000000, 1,  70, 3, 1, 1, 0),
	{ /* sentinel */ },
};

#define RK3026_DIV_PERI_MASK		0xf
#define RK3026_DIV_PERI_SHIFT		0

#define RK3026_CLKSEL1(_core_periph_div)					\
	{									\
		.reg = RK2928_CLKSEL_CON(1),					\
		.val = HIWORD_UPDATE(_core_periph_div, RK3026_DIV_PERI_MASK,	\
				RK3026_DIV_PERI_SHIFT)				\
	}

#define RK3026_CPUCLK_RATE(_prate, _core_periph_div)			\
	{								\
		.prate = _prate,					\
		.divs = {						\
			RK3026_CLKSEL1(_core_periph_div),		\
		},							\
	}

/*
 * The vendor's ARM DVFS table on this unit has exactly one entry: 912 MHz at
 * 1.4 V (0xc0a53778, terminated by {0, 0xfffffffe}). 1008 MHz is above the
 * part's 1.0 GHz rating and was inherited, not measured; the apll_clks entry
 * for 912 MHz (0xc08af57c) programs CLKSEL_CON1 = 0x007f0033, i.e. the
 * core_periph field is 3 (divide by 4).
 */
static struct rockchip_cpuclk_rate_table rk3026_cpuclk_rates[] __initdata = {
	RK3026_CPUCLK_RATE(912000000, 3),
};

static const struct rockchip_cpuclk_reg_data rk3026_cpuclk_data = {
	.core_reg[0]    = RK2928_CLKSEL_CON(0),
	.div_core_shift[0] = 0,
	.div_core_mask[0]  = 0x1f,
	.num_cores      = 1,
	.mux_core_alt   = 1,
	.mux_core_main  = 0,
	.mux_core_shift = 7,
	.mux_core_mask  = 0x1,
};

PNAME(mux_pll_p)		= { "xin24m", "xin24m" };

PNAME(mux_armclk_p)		= { "apll", "gpll_armclk" };
/*
 * CPU AXI bus source: bit 13 in CLKSEL_CON(0), 1-bit (0=apll, 1=gpll).
 * Vendor kernel: CPU_CLK_PLL_SEL_SHIFT=13, parents={arm_pll, gpll}.
 */
PNAME(mux_busclk_p)		= { "apll", "gpll_cpu" };
PNAME(mux_ddrphy_p)		= { "dpll_ddr", "gpll_ddr" };
/*
 * Many peripheral clocks use a 1-bit mux between {cpll, gpll}
 * or {gpll, cpll} depending on the parent order in vendor clock_data.c.
 * SELECT_FROM_2PLLS_CG = {cpll, gpll} (bit=0 → cpll, bit=1 → gpll)
 * SELECT_FROM_2PLLS_GC = {gpll, cpll} (bit=0 → gpll, bit=1 → cpll)
 */
PNAME(mux_pll_src_cpll_gpll_p)	= { "cpll", "gpll" };
PNAME(mux_pll_src_gpll_cpll_p)	= { "gpll", "cpll" };

PNAME(mux_usb480m_p)	= { "usb480m_phy", "xin24m" };
PNAME(mux_mmc_src_p)	= { "apll", "dpll", "gpll", "xin24m" };
/*
 * I2S PLL: bit 15 of CLKSEL_CON(2), 1-bit (0=gpll, 1=cpll).
 * I2S fracmux: bits[9:8] of CLKSEL_CON(3), 2-bit (0=i2s_src, 1=i2s_frac, 2=xin12m).
 */
PNAME(mux_i2s_pll_p)	= { "gpll", "cpll" };
PNAME(mux_i2s_pre_p)	= { "i2s_src", "i2s_frac", "xin12m" };
/*
 * UART PLL src: bit 15 of CLKSEL_CON(12), 1-bit (0=gpll, 1=cpll).
 * UART fracmux: bits[9:8] of CLKSEL_CON(13/14/15), 2-bit.
 */
PNAME(mux_uart_pll_p)	= { "gpll", "cpll" };
PNAME(mux_uart0_p)	= { "uart0_src", "uart0_frac", "xin24m" };
PNAME(mux_uart1_p)	= { "uart1_src", "uart1_frac", "xin24m" };
PNAME(mux_uart2_p)	= { "uart2_src", "uart2_frac", "xin24m" };

static struct rockchip_pll_clock rk3026_pll_clks[] __initdata = {
	[apll] = PLL(pll_rk3036, PLL_APLL, "apll", mux_pll_p, 0, RK2928_PLL_CON(0),
		     RK2928_MODE_CON, 0, 5, 0, rk3026_pll_rates),
	[dpll] = PLL(pll_rk3036, PLL_DPLL, "dpll", mux_pll_p, 0, RK2928_PLL_CON(4),
		     RK2928_MODE_CON, 4, 4, 0, NULL),
	[cpll] = PLL(pll_rk3036, PLL_CPLL, "cpll", mux_pll_p, 0, RK2928_PLL_CON(8),
		     RK2928_MODE_CON, 8, 6, 0, rk3026_cpll_clks),
	[gpll] = PLL(pll_rk3036, PLL_GPLL, "gpll", mux_pll_p, 0, RK2928_PLL_CON(12),
		     RK2928_MODE_CON, 12, 7, ROCKCHIP_PLL_SYNC_RATE, rk3026_gpll_clks),
};

#define MFLAGS CLK_MUX_HIWORD_MASK
#define DFLAGS CLK_DIVIDER_HIWORD_MASK
#define GFLAGS (CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE)

static struct rockchip_clk_branch rk3026_uart0_fracmux __initdata =
	MUX(SCLK_UART0, "sclk_uart0", mux_uart0_p, CLK_SET_RATE_PARENT,
			RK2928_CLKSEL_CON(13), 8, 2, MFLAGS);

static struct rockchip_clk_branch rk3026_uart1_fracmux __initdata =
	MUX(SCLK_UART1, "sclk_uart1", mux_uart1_p, CLK_SET_RATE_PARENT,
			RK2928_CLKSEL_CON(14), 8, 2, MFLAGS);

static struct rockchip_clk_branch rk3026_uart2_fracmux __initdata =
	MUX(SCLK_UART2, "sclk_uart2", mux_uart2_p, CLK_SET_RATE_PARENT,
			RK2928_CLKSEL_CON(15), 8, 2, MFLAGS);

static struct rockchip_clk_branch rk3026_i2s_fracmux __initdata =
	MUX(SCLK_I2S, "sclk_i2s", mux_i2s_pre_p, CLK_SET_RATE_PARENT,
			RK2928_CLKSEL_CON(3), 8, 2, MFLAGS);

static struct rockchip_clk_branch rk3026_clk_branches[] __initdata = {
	/*
	 * Clock-Architecture Diagram 1
	 * ARM / CPU core domain
	 */

	GATE(0, "gpll_armclk", "gpll", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(0), 6, GFLAGS),

	FACTOR(0, "xin12m", "xin24m", 0, 1, 2),

	/*
	 * Clock-Architecture Diagram 2
	 * DDR domain
	 * CLKSEL_CON(26): bit8=mux (0=dpll,1=gpll), bits[1:0]=div (power-of-two)
	 */
	GATE(0, "dpll_ddr", "dpll", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(0), 2, GFLAGS),
	GATE(0, "gpll_ddr", "gpll", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(0), 8, GFLAGS),
	COMPOSITE_NOGATE(0, "ddrphy2x", mux_ddrphy_p, CLK_IGNORE_UNUSED,
			RK2928_CLKSEL_CON(26), 8, 1, MFLAGS, 0, 2,
			DFLAGS | CLK_DIVIDER_POWER_OF_TWO),
	FACTOR(0, "ddrphy", "ddrphy2x", 0, 1, 2),

	/*
	 * CPU bus clocks:
	 * CLKSEL_CON(0): bit[7]=armclk mux (0=apll,1=gpll), bit[13]=aclk_cpu mux,
	 *                bits[4:0]=core div, bits[12:8]=aclk_cpu div
	 * CLKSEL_CON(1): bits[3:0]=pclk_dbg div, bits[6:4]=aclk_core div,
	 *                bits[9:8]=hclk_cpu div, bits[14:12]=pclk_cpu div
	 */
	COMPOSITE_NOMUX(0, "pclk_dbg", "armclk", CLK_IGNORE_UNUSED,
			RK2928_CLKSEL_CON(1), 0, 4, DFLAGS | CLK_DIVIDER_READ_ONLY,
			RK2928_CLKGATE_CON(9), 1, GFLAGS),
	COMPOSITE_NOMUX(0, "aclk_core_pre", "armclk", CLK_IGNORE_UNUSED,
			RK2928_CLKSEL_CON(1), 4, 3, DFLAGS | CLK_DIVIDER_READ_ONLY,
			RK2928_CLKGATE_CON(0), 7, GFLAGS),

	GATE(0, "gpll_cpu", "gpll", 0, RK2928_CLKGATE_CON(0), 1, GFLAGS),

	/*
	 * aclk_cpu_src: 1-bit mux (0=apll, 1=gpll) at CLKSEL_CON(0) bit13,
	 * divided by bits[12:8].
	 */
	COMPOSITE_NOGATE(0, "aclk_cpu_src", mux_busclk_p, 0,
			RK2928_CLKSEL_CON(0), 13, 1, MFLAGS, 8, 5, DFLAGS),
	GATE(ACLK_CPU, "aclk_cpu", "aclk_cpu_src", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(0), 3, GFLAGS),
	COMPOSITE_NOMUX(HCLK_CPU, "hclk_cpu", "aclk_cpu_src", CLK_IGNORE_UNUSED,
			RK2928_CLKSEL_CON(1), 8, 2, DFLAGS | CLK_DIVIDER_READ_ONLY,
			RK2928_CLKGATE_CON(0), 4, GFLAGS),
	COMPOSITE_NOMUX(PCLK_CPU, "pclk_cpu", "aclk_cpu_src", CLK_IGNORE_UNUSED,
			RK2928_CLKSEL_CON(1), 12, 3, DFLAGS | CLK_DIVIDER_READ_ONLY,
			RK2928_CLKGATE_CON(0), 5, GFLAGS),

	/*
	 * Peripheral bus clocks
	 * CLKSEL_CON(10): bit15=mux (0=gpll,1=cpll), bits[4:0]=aclk_peri div,
	 *                 bits[9:8]=hclk_peri div (power-of-two),
	 *                 bits[13:12]=pclk_peri div (power-of-two)
	 */
	COMPOSITE(0, "aclk_peri_src", mux_pll_src_gpll_cpll_p, 0,
			RK2928_CLKSEL_CON(10), 15, 1, MFLAGS, 0, 5, DFLAGS,
			RK2928_CLKGATE_CON(2), 0, GFLAGS),
	GATE(ACLK_PERI, "aclk_peri", "aclk_peri_src", 0,
			RK2928_CLKGATE_CON(2), 1, GFLAGS),
	DIV(0, "hclk_peri_src", "aclk_peri_src", CLK_IGNORE_UNUSED,
			RK2928_CLKSEL_CON(10), 8, 2, DFLAGS | CLK_DIVIDER_POWER_OF_TWO),
	GATE(HCLK_PERI, "hclk_peri", "hclk_peri_src", 0,
			RK2928_CLKGATE_CON(2), 2, GFLAGS),
	DIV(0, "pclk_peri_src", "aclk_peri_src", CLK_IGNORE_UNUSED,
			RK2928_CLKSEL_CON(10), 12, 2, DFLAGS | CLK_DIVIDER_POWER_OF_TWO),
	GATE(PCLK_PERI, "pclk_peri", "pclk_peri_src", 0,
			RK2928_CLKGATE_CON(2), 3, GFLAGS),

	/*
	 * UART clocks
	 * CLKSEL_CON(12): bit15=uart_pll mux (0=gpll, 1=cpll)
	 * CLKSEL_CON(13): bits[6:0]=uart0 div, bits[9:8]=uart0 fracmux
	 * CLKSEL_CON(14): bits[6:0]=uart1 div, bits[9:8]=uart1 fracmux
	 * CLKSEL_CON(15): bits[6:0]=uart2 div, bits[9:8]=uart2 fracmux
	 * CLKSEL_CON(17/18/19): frac dividers for uart0/1/2
	 */
	MUX(0, "uart_pll_clk", mux_uart_pll_p, 0,
			RK2928_CLKSEL_CON(12), 15, 1, MFLAGS),
	COMPOSITE_NOMUX(0, "uart0_src", "uart_pll_clk", 0,
			RK2928_CLKSEL_CON(13), 0, 7, DFLAGS,
			RK2928_CLKGATE_CON(1), 8, GFLAGS),
	COMPOSITE_NOMUX(0, "uart1_src", "uart_pll_clk", 0,
			RK2928_CLKSEL_CON(14), 0, 7, DFLAGS,
			RK2928_CLKGATE_CON(1), 10, GFLAGS),
	COMPOSITE_NOMUX(0, "uart2_src", "uart_pll_clk", 0,
			RK2928_CLKSEL_CON(15), 0, 7, DFLAGS,
			RK2928_CLKGATE_CON(1), 12, GFLAGS),
	COMPOSITE_FRACMUX(0, "uart0_frac", "uart0_src", CLK_SET_RATE_PARENT,
			RK2928_CLKSEL_CON(17), 0,
			RK2928_CLKGATE_CON(1), 9, GFLAGS,
			&rk3026_uart0_fracmux),
	COMPOSITE_FRACMUX(0, "uart1_frac", "uart1_src", CLK_SET_RATE_PARENT,
			RK2928_CLKSEL_CON(18), 0,
			RK2928_CLKGATE_CON(1), 11, GFLAGS,
			&rk3026_uart1_fracmux),
	COMPOSITE_FRACMUX(0, "uart2_frac", "uart2_src", CLK_SET_RATE_PARENT,
			RK2928_CLKSEL_CON(19), 0,
			RK2928_CLKGATE_CON(1), 13, GFLAGS,
			&rk3026_uart2_fracmux),

	/*
	 * I2S clocks
	 * CLKSEL_CON(2): bit15=I2S PLL mux (0=gpll, 1=cpll)
	 * CLKSEL_CON(3): bits[6:0]=i2s_src div (gate=CLK_GATE_I2S_SRC, CON0 bit9)
	 *                bits[9:8]=i2s fracmux (0=i2s_src, 1=i2s_frac, 2=xin12m)
	 * CLKSEL_CON(7): i2s frac div (gate=CLK_GATE_I2S_FRAC_SRC, CON0 bit10)
	 */
	MUX(0, "i2s_pll", mux_i2s_pll_p, 0,
			RK2928_CLKSEL_CON(2), 15, 1, MFLAGS),
	COMPOSITE_NOMUX(0, "i2s_src", "i2s_pll", 0,
			RK2928_CLKSEL_CON(3), 0, 7, DFLAGS,
			RK2928_CLKGATE_CON(0), 9, GFLAGS),
	COMPOSITE_FRACMUX(0, "i2s_frac", "i2s_src", CLK_SET_RATE_PARENT,
			RK2928_CLKSEL_CON(7), 0,
			RK2928_CLKGATE_CON(0), 10, GFLAGS,
			&rk3026_i2s_fracmux),

	/*
	 * MMC/SD/eMMC clocks (from vendor clock_data.c):
	 *
	 * SDMMC: CLKSEL_CON(11), mux=bit6 (0=cpll,1=gpll), div=bits[5:0]
	 *        gate=CLK_GATE_MMC0_SRC (CON2 bit11)
	 * SDIO:  CLKSEL_CON(12), mux=bit6 (0=cpll,1=gpll), div=bits[5:0]
	 *        gate=CLK_GATE_SDIO_SRC (CON2 bit13)
	 * eMMC:  CLKSEL_CON(12), mux=bit7 (0=cpll,1=gpll), div=bits[14:8]
	 *        gate=CLK_GATE_EMMC_SRC (CON2 bit14)
	 *
	 * All use {cpll, gpll} 1-bit mux (SELECT_FROM_2PLLS_CG).
	 */
	COMPOSITE(SCLK_SDMMC, "sclk_sdmmc", mux_pll_src_cpll_gpll_p, 0,
			RK2928_CLKSEL_CON(11), 6, 1, MFLAGS, 0, 6, DFLAGS,
			RK2928_CLKGATE_CON(2), 11, GFLAGS),

	COMPOSITE(SCLK_SDIO, "sclk_sdio", mux_pll_src_cpll_gpll_p, 0,
			RK2928_CLKSEL_CON(12), 6, 1, MFLAGS, 0, 6, DFLAGS,
			RK2928_CLKGATE_CON(2), 13, GFLAGS),

	COMPOSITE(SCLK_EMMC, "sclk_emmc", mux_pll_src_cpll_gpll_p, 0,
			RK2928_CLKSEL_CON(12), 7, 1, MFLAGS, 8, 6, DFLAGS,
			RK2928_CLKGATE_CON(2), 14, GFLAGS),

	MMC(SCLK_SDMMC_DRV,    "sdmmc_drv",    "sclk_sdmmc", RK3036_SDMMC_CON0, 1),
	MMC(SCLK_SDMMC_SAMPLE, "sdmmc_sample", "sclk_sdmmc", RK3036_SDMMC_CON1, 0),

	MMC(SCLK_SDIO_DRV,    "sdio_drv",    "sclk_sdio", RK3036_SDIO_CON0, 1),
	MMC(SCLK_SDIO_SAMPLE, "sdio_sample", "sclk_sdio", RK3036_SDIO_CON1, 0),

	MMC(SCLK_EMMC_DRV,    "emmc_drv",    "sclk_emmc", RK3036_EMMC_CON0, 1),
	MMC(SCLK_EMMC_SAMPLE, "emmc_sample", "sclk_emmc", RK3036_EMMC_CON1, 0),

	/*
	 * Display: LCDC0 (primary display controller)
	 * aclk_lcdc0_src: CLKSEL_CON(31), bit7=mux (0=cpll,1=gpll), bits[6:0]=div
	 *                 gate=CLK_GATE_ACLK_LCDC0_SRC (CON3 bit0)
	 * dclk_lcdc0: CLKSEL_CON(27), bit0=mux (0=cpll,1=gpll), bits[15:8]=div
	 *              gate=CLK_GATE_DCLK_LCDC0_SRC (CON3 bit1)
	 */
	COMPOSITE(0, "aclk_lcdc0_src", mux_pll_src_cpll_gpll_p, 0,
			RK2928_CLKSEL_CON(31), 7, 1, MFLAGS, 0, 5, DFLAGS,
			RK2928_CLKGATE_CON(3), 0, GFLAGS),
	GATE(ACLK_LCDC0, "aclk_lcdc0", "aclk_lcdc0_src", 0,
			RK2928_CLKGATE_CON(6), 0, GFLAGS),
	GATE(HCLK_LCDC0, "hclk_lcdc0", "hclk_peri_src", 0,
			RK2928_CLKGATE_CON(6), 1, GFLAGS),
	COMPOSITE(DCLK_LCDC0, "dclk_lcdc0", mux_pll_src_cpll_gpll_p, 0,
			RK2928_CLKSEL_CON(27), 0, 1, MFLAGS, 8, 8, DFLAGS,
			RK2928_CLKGATE_CON(3), 1, GFLAGS),

	/*
	 * Display: LCDC1 (secondary display controller)
	 * aclk_lcdc1_src: CLKSEL_CON(31), bit15=mux (0=cpll,1=gpll), bits[12:8]=div
	 *                 gate=CLK_GATE_ACLK_LCDC1_SRC (CON1 bit4)
	 * dclk_lcdc1: CLKSEL_CON(28), bit0=mux (0=cpll,1=gpll), bits[15:8]=div
	 *              gate=CLK_GATE_DCLK_LCDC1_SRC (CON3 bit2)
	 */
	COMPOSITE(ACLK_LCDC1_SRC, "aclk_lcdc1_src", mux_pll_src_cpll_gpll_p, 0,
			RK2928_CLKSEL_CON(31), 15, 1, MFLAGS, 8, 5, DFLAGS,
			RK2928_CLKGATE_CON(1), 4, GFLAGS),
	/*
	 * The video NIU sits between the source and aclk_lcdc1, and the EBC is
	 * what borrows that clock. Omitting it left the AXI path to DRAM gated:
	 * GFLAGS is SET_TO_DISABLE, so a gate nothing models is a gate nothing
	 * ever opens. Chain and gate_idx from the running kernel's own clk table
	 * (docs/re/CRU-GATES.md): aclk_lcdc1[CON9.b6] <- aclk_vio1_niu[CON9.b10]
	 * <- aclk_lcdc1_pre[CON1.b4].
	 */
	GATE(0, "aclk_vio1_niu", "aclk_lcdc1_src", 0,
			RK2928_CLKGATE_CON(9), 10, GFLAGS),
	GATE(ACLK_LCDC1, "aclk_lcdc1", "aclk_vio1_niu", 0,
			RK2928_CLKGATE_CON(9), 6, GFLAGS),
	GATE(HCLK_LCDC1, "hclk_lcdc1", "hclk_peri_src", 0,
			RK2928_CLKGATE_CON(9), 5, GFLAGS),
	COMPOSITE(DCLK_LCDC1, "dclk_lcdc1", mux_pll_src_cpll_gpll_p, 0,
			RK2928_CLKSEL_CON(28), 0, 1, MFLAGS, 8, 8, DFLAGS,
			RK2928_CLKGATE_CON(3), 2, GFLAGS),

	/*
	 * EBC (E-Paper display controller)
	 * dclk_ebc: CLKSEL_CON(23), bit0=mux (0=cpll,1=gpll), bits[15:8]=div
	 *           gate=CLK_GATE_DCLK_EBC_SRC (CON3 bit4)
	 * aclk_ebc: sourced from aclk_lcdc0_src, gated at CON9 bit10 (ACLK_VIO1)
	 * hclk_ebc: sourced from hclk_disp_pre, gated at CON9 bit9
	 *
	 * ⚠️ CLKSEL_CON(23) is NOT hiword-write-masked, unlike the other CLKSEL
	 * registers, so this composite takes no MFLAGS/DFLAGS. Measured on hardware
	 * 2026-08-03: bits[31:16] are readable storage, and a plain 32-bit store of
	 * 0x00000101 changed bit 0 -- a mask would have ignored the whole low half.
	 * By contrast CLKGATE_CON(0..9) all read back with zero upper halves, and
	 * CON(12) still held its bootloader value after this file's own
	 * HIWORD_UPDATE() at init, so both of those blocks are genuinely masked.
	 *
	 * With DFLAGS the divider write landed as the literal 0xff000100, whose
	 * would-be mask bits are just data here -- clearing mux bit 0 and parking
	 * dclk_ebc on CPLL, which sits in slow mode at 24 MHz. clk_set_rate() then
	 * returned success at 12 MHz and clk_get_rate() reported the 66 MHz CCF
	 * believed, because CCF's cached parent was still the GPLL it read at
	 * registration. A following clk_set_parent() to that same parent is a no-op,
	 * which is why asserting the parent last did not repair it either.
	 * Correcting the register by hand took a 54-frame scan from 4270 ms to
	 * 790 ms against the driver's own 635 ms expectation.
	 */
	COMPOSITE(DCLK_EBC, "dclk_ebc", mux_pll_src_cpll_gpll_p, 0,
			RK2928_CLKSEL_CON(23), 0, 1, 0, 8, 8, 0,
			RK2928_CLKGATE_CON(3), 4, GFLAGS),
	GATE(ACLK_EBC, "aclk_ebc", "aclk_lcdc0_src", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(9), 10, GFLAGS),
	GATE(HCLK_EBC, "hclk_ebc", "hclk_peri_src", 0,
			RK2928_CLKGATE_CON(9), 9, GFLAGS),

	/*
	 * Video codec (VEPU/VDPU)
	 * aclk_vcodec (vepu): CLKSEL_CON(32), bit7=mux (0=cpll,1=gpll), bits[6:0]=div
	 *                     gate=CLK_GATE_ACLK_VEPU_SRC (CON3 bit9)
	 * hclk_vcodec: aclk_vcodec / 4, gate=CLK_GATE_HCLK_VDPU (CON3 bit12)
	 */
	COMPOSITE(ACLK_VCODEC, "aclk_vcodec", mux_pll_src_cpll_gpll_p, 0,
			RK2928_CLKSEL_CON(32), 7, 1, MFLAGS, 0, 5, DFLAGS,
			RK2928_CLKGATE_CON(3), 9, GFLAGS),
	FACTOR_GATE(HCLK_VCODEC, "hclk_vcodec", "aclk_vcodec", 0, 1, 4,
			RK2928_CLKGATE_CON(3), 12, GFLAGS),

	/*
	 * GPU clock
	 * CLKSEL_CON(34): bit8=mux (0=cpll,1=gpll), bits[4:0]=div
	 * gate=CLK_GATE_GPU_PRE (CON3 bit13)
	 */
	COMPOSITE(ACLK_GPU, "aclk_gpu", mux_pll_src_cpll_gpll_p, 0,
			RK2928_CLKSEL_CON(34), 8, 1, MFLAGS, 0, 5, DFLAGS,
			RK2928_CLKGATE_CON(3), 13, GFLAGS),

	/*
	 * SPI clock
	 * CLKSEL_CON(25): bit8=mux (0=cpll,1=gpll), bits[6:0]=div
	 * gate=CLK_GATE_SPI0_SRC (CON2 bit9)
	 */
	COMPOSITE(SCLK_SPI, "sclk_spi", mux_pll_src_cpll_gpll_p, 0,
			RK2928_CLKSEL_CON(25), 8, 1, MFLAGS, 0, 7, DFLAGS,
			RK2928_CLKGATE_CON(2), 9, GFLAGS),

	/*
	 * SARADC clock
	 * CLKSEL_CON(24): bits[15:8]=div (sourced from xin24m, no mux)
	 * gate=CLK_GATE_SARADC_SRC (CON2 bit8)
	 */
	COMPOSITE_NOMUX(SCLK_SARADC, "sclk_saradc", "xin24m", 0,
			RK2928_CLKSEL_CON(24), 8, 8, DFLAGS,
			RK2928_CLKGATE_CON(2), 8, GFLAGS),

	/*
	 * USB480M PHY clock
	 */
	MUX(USBPHY_480M, "usb480m", mux_usb480m_p, CLK_SET_RATE_PARENT,
			RK2928_MISC_CON, 15, 1, MFLAGS),

	/*
	 * HDMI pclk — sourced from pclk_cpu
	 * gate=CLK_GATE_PCLK_HDMI (CON3 bit8)
	 */
	GATE(PCLK_HDMI, "pclk_hdmi", "pclk_cpu", 0,
			RK2928_CLKGATE_CON(3), 8, GFLAGS),

	/*
	 * USB OTG PHY reference clocks
	 * gate=CLK_GATE_OTGPHY0/1 (CON1 bit5/bit6)
	 */
	GATE(0, "sclk_otgphy0", "xin12m", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(1), 5, GFLAGS),
	GATE(0, "sclk_otgphy1", "xin12m", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(1), 6, GFLAGS),

	/*
	 * Clock-Architecture Diagram 3
	 * Gate clocks sourced from bus clocks
	 */

	/* aclk_cpu gates */
	GATE(0, "aclk_intmem", "aclk_cpu", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(4), 12, GFLAGS),
	GATE(0, "aclk_strc_sys", "aclk_cpu", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(4), 10, GFLAGS),

	/* hclk_cpu gates */
	GATE(HCLK_ROM, "hclk_rom", "hclk_cpu", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(5), 6, GFLAGS),

	/* pclk_cpu gates */
	GATE(PCLK_GRF, "pclk_grf", "pclk_cpu", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(5), 4, GFLAGS),
	GATE(PCLK_DDRUPCTL, "pclk_ddrupctl", "pclk_cpu", 0,
			RK2928_CLKGATE_CON(5), 7, GFLAGS),
	GATE(PCLK_ACODEC, "pclk_acodec", "pclk_peri_src", 0,
			RK2928_CLKGATE_CON(5), 14, GFLAGS),

	/* aclk_peri gates */
	GATE(ACLK_DMAC, "aclk_dmac", "aclk_peri", 0,
			RK2928_CLKGATE_CON(5), 1, GFLAGS),
	GATE(0, "aclk_peri_niu", "aclk_peri", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(9), 15, GFLAGS),

	/* hclk_peri gates */
	GATE(0, "hclk_usb_peri", "hclk_peri", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(9), 13, GFLAGS),
	GATE(0, "hclk_peri_arbi", "hclk_peri", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(9), 14, GFLAGS),
	GATE(HCLK_NANDC, "hclk_nandc", "hclk_peri", 0,
			RK2928_CLKGATE_CON(5), 9, GFLAGS),
	GATE(HCLK_SDMMC, "hclk_sdmmc", "hclk_peri", 0,
			RK2928_CLKGATE_CON(5), 10, GFLAGS),
	GATE(HCLK_SDIO, "hclk_sdio", "hclk_peri", 0,
			RK2928_CLKGATE_CON(5), 11, GFLAGS),
	GATE(HCLK_EMMC, "hclk_emmc", "hclk_peri", 0,
			RK2928_CLKGATE_CON(7), 0, GFLAGS),
	GATE(HCLK_OTG, "hclk_otg", "hclk_peri", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(5), 13, GFLAGS),
	GATE(HCLK_HOST, "hclk_host", "hclk_peri", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(7), 3, GFLAGS),
	GATE(HCLK_I2S, "hclk_i2s", "hclk_peri", 0,
			RK2928_CLKGATE_CON(7), 2, GFLAGS),

	/* pclk_peri gates */
	GATE(PCLK_TIMER0, "pclk_timer0", "pclk_peri", 0,
			RK2928_CLKGATE_CON(7), 7, GFLAGS),
	GATE(PCLK_TIMER1, "pclk_timer1", "pclk_peri", 0,
			RK2928_CLKGATE_CON(7), 8, GFLAGS),
	GATE(PCLK_PWM, "pclk_pwm", "pclk_peri", 0,
			RK2928_CLKGATE_CON(7), 10, GFLAGS),
	GATE(PCLK_SPI, "pclk_spi", "pclk_peri", 0,
			RK2928_CLKGATE_CON(7), 12, GFLAGS),
	GATE(PCLK_SARADC, "pclk_saradc", "pclk_peri", 0,
			RK2928_CLKGATE_CON(7), 14, GFLAGS),
	GATE(PCLK_WDT, "pclk_wdt", "pclk_peri", 0,
			RK2928_CLKGATE_CON(7), 15, GFLAGS),
	GATE(PCLK_UART0, "pclk_uart0", "pclk_peri", 0,
			RK2928_CLKGATE_CON(8), 0, GFLAGS),
	GATE(PCLK_UART1, "pclk_uart1", "pclk_peri", 0,
			RK2928_CLKGATE_CON(8), 1, GFLAGS),
	GATE(PCLK_UART2, "pclk_uart2", "pclk_peri", 0,
			RK2928_CLKGATE_CON(8), 2, GFLAGS),
	GATE(PCLK_I2C0, "pclk_i2c0", "pclk_peri", 0,
			RK2928_CLKGATE_CON(8), 4, GFLAGS),
	GATE(PCLK_I2C1, "pclk_i2c1", "pclk_peri", 0,
			RK2928_CLKGATE_CON(8), 5, GFLAGS),
	GATE(PCLK_I2C2, "pclk_i2c2", "pclk_peri", 0,
			RK2928_CLKGATE_CON(8), 6, GFLAGS),
	GATE(PCLK_I2C3, "pclk_i2c3", "pclk_peri", 0,
			RK2928_CLKGATE_CON(8), 7, GFLAGS),
	GATE(PCLK_GPIO0, "pclk_gpio0", "pclk_peri", 0,
			RK2928_CLKGATE_CON(8), 9, GFLAGS),
	GATE(PCLK_GPIO1, "pclk_gpio1", "pclk_peri", 0,
			RK2928_CLKGATE_CON(8), 10, GFLAGS),
	GATE(PCLK_GPIO2, "pclk_gpio2", "pclk_peri", 0,
			RK2928_CLKGATE_CON(8), 11, GFLAGS),
	GATE(PCLK_GPIO3, "pclk_gpio3", "pclk_peri", 0,
			RK2928_CLKGATE_CON(8), 12, GFLAGS),

	/* Debug clocks */
	GATE(0, "clk_core_dbg", "armclk", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(9), 0, GFLAGS),
	GATE(0, "pclk_dbg_pre", "pclk_dbg", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(9), 1, GFLAGS),
	GATE(0, "clk_l2c", "armclk", CLK_IGNORE_UNUSED,
			RK2928_CLKGATE_CON(9), 4, GFLAGS),
};

static const char *const rk3026_critical_clocks[] __initconst = {
	"aclk_cpu",
	"aclk_peri",
	"hclk_peri",
	"pclk_peri",
	"pclk_ddrupctl",
	"ddrphy",
};

/*
 * Global software reset.
 *
 * rockchip_register_restart_notifier() cannot express this one: it always
 * writes 0xfdb9, whereas the MiniLoader resets by writing 0xeca8 to
 * CRU+0x104 (GLB_SRST_SND) -- "mov r4,#0x20000000; movw r0,#0xeca8;
 * str r0,[r4,#0x104]" at 0x60001430, with the same code again in rk30usbplug.
 *
 * CRU+0x100 (GLB_SRST_FST) is a real register -- the vendor kernel's own
 * dump_regs names it -- but nothing in any binary we hold ever writes it, and
 * 0xfdb9 appears in none of them either. Use the sequence that is proven.
 */
static void __iomem *rk3026_cru_base;

static int rk3026_restart_notify(struct notifier_block *this,
				 unsigned long mode, void *cmd)
{
	writel(0xeca8, rk3026_cru_base + RK2928_GLB_SRST_SND);
	return NOTIFY_DONE;
}

static struct notifier_block rk3026_restart_handler = {
	.notifier_call = rk3026_restart_notify,
	.priority = 128,
};

static void __init rk3026_clk_init(struct device_node *np)
{
	struct rockchip_clk_provider *ctx;
	unsigned long clk_nr_clks;
	void __iomem *reg_base;

	reg_base = of_iomap(np, 0);
	if (!reg_base) {
		pr_err("%s: could not map cru region\n", __func__);
		return;
	}

	/*
	 * Make uart_pll_clk a child of gpll, as all other sources are
	 * not that stable.  This mirrors what the vendor bootloader does.
	 */
	writel_relaxed(HIWORD_UPDATE(0, 0x1, 15),
		       reg_base + RK2928_CLKSEL_CON(12));

	clk_nr_clks = rockchip_clk_find_max_clk_id(rk3026_clk_branches,
						    ARRAY_SIZE(rk3026_clk_branches)) + 1;
	ctx = rockchip_clk_init(np, reg_base, clk_nr_clks);
	if (IS_ERR(ctx)) {
		pr_err("%s: rockchip clk init failed\n", __func__);
		iounmap(reg_base);
		return;
	}

	rockchip_clk_register_plls(ctx, rk3026_pll_clks,
				   ARRAY_SIZE(rk3026_pll_clks),
				   RK3026_GRF_SOC_STATUS0);
	rockchip_clk_register_branches(ctx, rk3026_clk_branches,
				       ARRAY_SIZE(rk3026_clk_branches));
	rockchip_clk_protect_critical(rk3026_critical_clocks,
				      ARRAY_SIZE(rk3026_critical_clocks));

	rockchip_clk_register_armclk(ctx, ARMCLK, "armclk",
			mux_armclk_p, ARRAY_SIZE(mux_armclk_p),
			&rk3026_cpuclk_data, rk3026_cpuclk_rates,
			ARRAY_SIZE(rk3026_cpuclk_rates));

	rockchip_register_softrst(np, 9, reg_base + RK2928_SOFTRST_CON(0),
				  ROCKCHIP_SOFTRST_HIWORD_MASK);

	rk3026_cru_base = reg_base;
	if (register_restart_handler(&rk3026_restart_handler))
		pr_err("%s: failed to register restart handler\n", __func__);

	rockchip_clk_of_add_provider(np, ctx);
}
CLK_OF_DECLARE(rk3026_cru, "rockchip,rk3026-cru", rk3026_clk_init);
