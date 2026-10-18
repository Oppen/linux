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

#define RK3026_DIV_PCLK_DBG_MASK	0xf
#define RK3026_DIV_PCLK_DBG_SHIFT	0
#define RK3026_DIV_ACLK_CORE_MASK	0x7
#define RK3026_DIV_ACLK_CORE_SHIFT	4
#define RK3026_DIV_ACLK_CPU_MASK	0x1f
#define RK3026_DIV_ACLK_CPU_SHIFT	8
#define RK3026_DIV_DCLK_EBC_MASK	0xff
#define RK3026_DIV_DCLK_EBC_SHIFT	8
#define RK3026_GATE_GPLL_CPU		BIT(1)

/* dclk_ebc /5 until the EBC driver probes */
#define RK3026_DCLK_EBC_DIV_INIT	4

/*
 * While aclk_cpu_src divided APLL, as the bootloader leaves it, the core mux
 * the cpuclk notifier performs did NOT protect it: the instant APLL relocked
 * at the new rate the whole CPU-domain AHB/APB (aclk_cpu, hclk_cpu, pclk_cpu,
 * and with them GRF and the DDR controller's APB) moved with it. This divider
 * was therefore sized for the HIGHEST rate in the table and written in
 * pre_muxs, before APLL changes: 600 MHz / 4 = 150 MHz, the rate this board
 * ran at since first light.
 *
 * rk3026_clk_init() now moves aclk_cpu_src onto GPLL, as the vendor kernel
 * runs it, so APLL changes leave the bus alone. The divider stays /4, which
 * is correct with either parent: 192 MHz on a 768 MHz GPLL.
 */
#define RK3026_ACLK_CPU_DIV_SAFE	3

#define RK3026_CLKSEL0(_aclk_cpu_div)						\
	{									\
		.reg = RK2928_CLKSEL_CON(0),					\
		.val = HIWORD_UPDATE(_aclk_cpu_div, RK3026_DIV_ACLK_CPU_MASK,	\
				RK3026_DIV_ACLK_CPU_SHIFT)			\
	}

#define RK3026_CLKSEL1(_pclk_dbg_div, _aclk_core_div)				\
	{									\
		.reg = RK2928_CLKSEL_CON(1),					\
		.val = HIWORD_UPDATE(_pclk_dbg_div, RK3026_DIV_PCLK_DBG_MASK,	\
				RK3026_DIV_PCLK_DBG_SHIFT) |			\
		       HIWORD_UPDATE(_aclk_core_div, RK3026_DIV_ACLK_CORE_MASK,	\
				RK3026_DIV_ACLK_CORE_SHIFT)			\
	}

/*
 * pre_muxs run before APLL is touched; divs run after it has relocked, while
 * the core is still on the alternate parent on a raise and after the remux on
 * a drop (clk-cpu.c). So the safe bus divider goes in pre_muxs and the
 * per-rate values in divs, and neither direction has a window in which a
 * clock is faster than its entry allows.
 */
#define RK3026_CPUCLK_RATE(_prate, _aclk_cpu_div, _pclk_dbg_div, _aclk_core_div) \
	{								\
		.prate = _prate,					\
		.pre_muxs = {						\
			RK3026_CLKSEL0(RK3026_ACLK_CPU_DIV_SAFE),	\
		},							\
		.divs = {						\
			RK3026_CLKSEL0(_aclk_cpu_div),			\
			RK3026_CLKSEL1(_pclk_dbg_div, _aclk_core_div),	\
		},							\
	}

/*
 * 600 MHz is what our U-Boot leaves APLL at (APLL_CON0/CON1 = 0x304b/0x1441,
 * clk_summary apll/armclk 600000000), and its dividers reproduce the state this
 * board has run in since first light: aclk_core 300, pclk_dbg 100 (and
 * aclk_cpu 150 while it was on APLL). 504 divides down from the same
 * dividers: 252 / 84 MHz.
 *
 * 🔴 912 MHz IS NOT HERE, AND MUST NOT BE ADDED WITHOUT A CARD AND A RESET PIN.
 * It kills this board. Measured 2026-08-09 by walking the transition one CRU
 * write at a time from userspace: raising the bus divider, muxing the core to
 * gpll_armclk, APLL to slow mode, reprogramming to 912, relocking, APLL back to
 * normal and BOTH CLKSEL_CON1 dividers all survive -- the board dies on the
 * single write that muxes the core back onto a 912 MHz APLL, with vdd_arm at a
 * measured 1.45 V and aclk_core/pclk_dbg at the vendor's own 228 MHz. A 600 ->
 * 504 -> 600 round trip through the identical sequence survives, so the
 * sequence is not the fault; the rate is. Vendor Android does run 912 on this
 * silicon, so something in its configuration differs from ours -- the open lead
 * is the PL310 latencies in rk3026.dtsi (tag 1 1 1, data 1 2 1), which are the
 * fastest settings possible and are not obviously valid at 912. Untested.
 */
static struct rockchip_cpuclk_rate_table rk3026_cpuclk_rates[] __initdata = {
	/* _prate, aclk_cpu div, pclk_dbg div, aclk_core div (register values) */
	RK3026_CPUCLK_RATE(600000000, 3, 5, 1),
	RK3026_CPUCLK_RATE(504000000, 3, 5, 1),
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
	/*
	 * CLKSEL_CON(1)[3:0] is both the debug-APB divider and the A9 MPCore's
	 * PERIPHCLK, so it carries the id the dts asks for. Without CORE_PERI
	 * the TWD's of_clk_get() fails, twd_clk_init() skips
	 * clk_notifier_register(), and the tick never follows cpufreq (P69).
	 *
	 * Keep CLK_DIVIDER_READ_ONLY: rk3026_cpuclk_rates[] owns this field.
	 */
	COMPOSITE_NOMUX(CORE_PERI, "core_peri", "armclk", CLK_IGNORE_UNUSED,
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
	/*
	 * The display AHB source, and the parent of both hclk_lcdc1 and
	 * hclk_ebc. It was not modelled at all until 2026-08-06, and both were
	 * parented to hclk_peri_src instead -- a clock the hardware does not
	 * route here. Nothing then ever wrote CLKSEL_CON30, so hclk_ebc ran at
	 * whatever the loader left it: mux on CPLL, which sits in slow mode at
	 * 24 MHz, divided by 4 = 6 MHz, against the vendor's 256.
	 *
	 * That was the dclk_ebc ceiling. Measured on hardware the same day, one
	 * variable, eyes on the glass: at hclk 6 MHz a 22 MHz dclk renders
	 * garbage and 16.5 MHz is the most that renders; with this register on
	 * GPLL/1 = 132 MHz, 22, 33 and 66 MHz all render a clean circle. 66 MHz
	 * takes a 38-frame refresh from 3010 ms to 570 ms. GPLL is now 768 MHz,
	 * and rk3026_clk_set_gpll_dividers() sets this divider to /3 = 256 MHz.
	 *
	 * Parent and divider from the vendor kernel's own clk table
	 * (re-workspace/evidence/boox-zImage): hclk_disp_pre's struct clk carries
	 * reg CRU+0xbc = CLKSEL_CON30, mask 0x1f shift 8, mux mask 1 shift 15
	 * over { codec_pll, general_pll }, gate CLKGATE_CON(0) bit 11 -- and
	 * hclk_ebc's own recalc pointer is NULL, i.e. it is a pure gate that
	 * takes its parent's rate with no divider of its own.
	 */
	COMPOSITE(HCLK_DISP_PRE, "hclk_disp_pre", mux_pll_src_cpll_gpll_p, 0,
			RK2928_CLKSEL_CON(30), 15, 1, MFLAGS, 8, 5, DFLAGS,
			RK2928_CLKGATE_CON(0), 11, GFLAGS),
	GATE(HCLK_LCDC1, "hclk_lcdc1", "hclk_disp_pre", 0,
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
	/*
	 * ACLK_EBC is the SAME physical clock as aclk_vio1_niu: both were
	 * modelled as GATE objects on CLKGATE_CON(9) bit 10, which is one bit
	 * with two refcounts -- a disable through either closes it under the
	 * other, and the EBC driver's fini path disables its own. That hazard is
	 * why enabling the EBC's AXI master clock was left switched off in
	 * rk3026_ebc.c for two days (docs/PENDING.md P8).
	 *
	 * So it is one clock, expressed as a 1:1 factor with no second gate. The
	 * gate lives on aclk_vio1_niu, which the EBC also holds via "aclk".
	 *
	 * ⚠️ The old parent, aclk_lcdc0_src, was wrong as well (P16): it is gated
	 * at CLKGATE_CON(3) bit 0 on BOTH our kernel and the vendor's, so a
	 * consumer enabling aclk_ebc was asking CCF for a clock whose modelled
	 * parent is off, while the hardware bit it actually needs is CON9 b10.
	 */
	FACTOR(ACLK_EBC, "aclk_ebc", "aclk_vio1_niu", 0, 1, 1),
	/* Parent is the display AHB source, NOT hclk_peri_src -- see above. */
	GATE(HCLK_EBC, "hclk_ebc", "hclk_disp_pre", 0,
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
	GATE(0, "pclk_dbg_pre", "core_peri", CLK_IGNORE_UNUSED,
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

/*
 * The vendor's dividers below GPLL, for the 768 MHz GPLL that rk3026.dtsi
 * asks for; keep the two in sync. Values from CLKSEL_CON0-34 as the vendor
 * kernel's clock init leaves them (GPLL 768, CPLL unused): aclk/hclk/pclk_cpu
 * 192/96/48, aclk/hclk/pclk_peri 192/96/48, aclk_lcdc1_src and hclk_disp_pre
 * 256, sclk_sdmmc 48, sclk_sdio 24, sclk_emmc 192, sclk_spi 48, uart*_src 12,
 * dclk_lcdc* 3, aclk_vdpu/vepu 384, aclk_gpu 128. Its drivers change some
 * later: under Android the eMMC runs at 96, the GPU at 192, and with CPLL
 * at 640 hclk_disp_pre moves to CPLL/2 = 320. CPLL is not touched here.
 *
 * mask and val are raw CLKSEL_CON bits, written hiword-masked, so they read
 * directly against the vendor's register values. The order matters.
 */
struct rk3026_clksel_init {
	u8 con;
	u16 mask;
	u16 val;
};

static const struct rk3026_clksel_init rk3026_gpll_dividers[] __initconst = {
	{  1, 0x7300, 0x3100 },	/* first: hclk_cpu /2, pclk_cpu /4 */
	{ 10, 0xb31f, 0x2103 },	/* aclk_peri GPLL /4, hclk_peri /2, pclk_peri /4 */
	{ 11, 0x003f, 0x000f },	/* sclk_sdmmc /16 */
	{ 12, 0x3f3f, 0x031f },	/* sclk_emmc /4, sclk_sdio /32 */
	{ 13, 0x007f, 0x003f },	/* uart0_src /64 */
	{ 14, 0x007f, 0x003f },	/* uart1_src /64 */
	{ 15, 0x007f, 0x003f },	/* uart2_src /64 */
	{ 25, 0x007f, 0x000f },	/* sclk_spi /16 */
	{ 27, 0xff00, 0xff00 },	/* dclk_lcdc0 /256 */
	{ 28, 0xff00, 0xff00 },	/* dclk_lcdc1 /256 */
	{ 30, 0x9f00, 0x8200 },	/* hclk_disp_pre GPLL /3 */
	{ 31, 0xffff, 0x8282 },	/* aclk_lcdc1_src, aclk_lcdc0_src GPLL /3 */
	{ 32, 0x1f1f, 0x0101 },	/* aclk_vdpu /2, aclk_vepu /2 */
	{ 34, 0x001f, 0x0005 },	/* aclk_gpu /6 */
	{  0, 0x3f00, 0x2300 },	/* last: aclk_cpu GPLL /4 */
};

/*
 * The bootloader leaves GPLL at 132 MHz with the peripheral bus at /1, and
 * the GPLL rate is only raised when this provider is registered and its
 * assigned-clock-rates are applied. A PLL rate change does not touch the
 * dividers below it, so the dividers of the clocks here that can sit on
 * GPLL are written first: below 768 MHz they only run slower, at 768 MHz
 * none of them ends up faster than the vendor's clock init runs it. Only
 * divider fields are written, except for the muxes of the bus sources the
 * vendor runs from GPLL (aclk_cpu, aclk_peri and the display sources in
 * CLKSEL_CON30/31), so a clock that sits on CPLL stays there and takes the
 * same divider of the CPLL rate. Not written: armclk and ddrphy2x, which
 * run from APLL and DPLL, the I2S source (the vendor runs it at GPLL/1),
 * and the camera clock output divider (CLKSEL_CON29), which has no clock
 * here and stays as the bootloader leaves it.
 *
 * Before registration on purpose: the clock framework caches a composite's
 * parent from the register when the branch is registered, so a later raw
 * write would leave the cached parent wrong and turn a following
 * clk_set_parent() to that same parent into a no-op.
 *
 * CLKSEL_CON1 goes first, while aclk_cpu still runs from APLL, so hclk_cpu
 * and pclk_cpu never exceed half and a quarter of it; CLKSEL_CON0 moves
 * aclk_cpu onto GPLL last. CLKSEL_CON30's low byte is not identified in any
 * binary we hold, so only its mux and divider are masked.
 *
 * dclk_ebc: the EBC driver sets its parent ("dclk-parent") and rate when it
 * probes. Until then it keeps the bootloader's mux, and on GPLL /4 would be
 * 192 MHz, above the vendor's 160 (CPLL/4), so it gets /5: 153.6 MHz on
 * GPLL. CLKSEL_CON23 is not hiword-masked, see dclk_ebc above.
 */
static void __init rk3026_clk_set_gpll_dividers(void __iomem *reg_base)
{
	const struct rk3026_clksel_init *sel;
	void __iomem *con23 = reg_base + RK2928_CLKSEL_CON(23);
	unsigned int i;
	u32 val;

	/* gpll_cpu, which aclk_cpu moves onto last: open whatever left it */
	writel_relaxed(HIWORD_UPDATE(0, RK3026_GATE_GPLL_CPU, 0),
		       reg_base + RK2928_CLKGATE_CON(0));

	for (i = 0; i < ARRAY_SIZE(rk3026_gpll_dividers); i++) {
		sel = &rk3026_gpll_dividers[i];
		writel_relaxed(HIWORD_UPDATE(sel->val, sel->mask, 0),
			       reg_base + RK2928_CLKSEL_CON(sel->con));
	}

	val = readl_relaxed(con23);
	val &= ~(RK3026_DIV_DCLK_EBC_MASK << RK3026_DIV_DCLK_EBC_SHIFT);
	val |= RK3026_DCLK_EBC_DIV_INIT << RK3026_DIV_DCLK_EBC_SHIFT;
	writel_relaxed(val, con23);
}

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

	rk3026_clk_set_gpll_dividers(reg_base);

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
