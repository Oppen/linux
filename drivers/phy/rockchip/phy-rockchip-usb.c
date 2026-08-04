// SPDX-License-Identifier: GPL-2.0-only
/*
 * Rockchip usb PHY driver
 *
 * Copyright (C) 2014 Yunzhi Li <lyz@rock-chips.com>
 * Copyright (C) 2014 ROCKCHIP, Inc.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/hw_bitfield.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/delay.h>

static int enable_usb_uart;

#define UOC_CON0					0x00
#define UOC_CON0_SIDDQ					BIT(13)
#define UOC_CON0_DISABLE				BIT(4)
#define UOC_CON0_COMMON_ON_N				BIT(0)

/*
 * UOC_CON1 bit 0 is the PHY's suspend bit. The vendor kernel's
 * usb20host_phy_suspend()/resume writes exactly it: suspend sets it, resume
 * clears it (docs/re/VENDOR-KERNEL-USB.md §2). Confirmed on hardware
 * 2026-08-04 by reading Android with the radio enumerated: UOC1_CON1 is
 * 0x1d5 (bit 0 set) while the host is idle and 0x1d4 (bit 0 clear) while a
 * device is attached and running.
 *
 * Mainline never touched this register, so a host PHY left suspended produced
 * no 480 MHz UTMI clock, dwc2's CSFTRST could never self-clear, and the
 * controller failed probe with -16 on every cold boot.
 */
#define UOC_CON1					0x04
#define UOC_CON1_SUSPEND				BIT(0)

#define UOC_CON2					0x08
#define UOC_CON2_SOFT_CON_SEL				BIT(2)

#define UOC_CON3					0x0c
/* bits present on rk3188 and rk3288 phys */
#define UOC_CON3_UTMI_TERMSEL_FULLSPEED			BIT(5)
#define UOC_CON3_UTMI_XCVRSEELCT_FSTRANSC		1UL
#define UOC_CON3_UTMI_XCVRSEELCT_MASK			GENMASK(4, 3)
#define UOC_CON3_UTMI_OPMODE_NODRIVING			1UL
#define UOC_CON3_UTMI_OPMODE_MASK			GENMASK(2, 1)
#define UOC_CON3_UTMI_SUSPENDN				BIT(0)

struct rockchip_usb_phys {
	int reg;
	const char *pll_name;
	/*
	 * Drive UOC_CON1's suspend bit, and pulse the "phy-reset" reset if the
	 * node declares one, from power_on/power_off. Opt-in per PHY because the
	 * OTG PHY on RK3026 works without either and carries the only console
	 * this board has -- changing its sequence is not worth the risk.
	 */
	bool suspend_ctl;
};

struct rockchip_usb_phy_base;
struct rockchip_usb_phy_pdata {
	struct rockchip_usb_phys *phys;
	int (*init_usb_uart)(struct regmap *grf,
			     const struct rockchip_usb_phy_pdata *pdata);
	int usb_uart_phy;
};

struct rockchip_usb_phy_base {
	struct device *dev;
	struct regmap *reg_base;
	const struct rockchip_usb_phy_pdata *pdata;
};

struct rockchip_usb_phy {
	struct rockchip_usb_phy_base *base;
	struct device_node *np;
	unsigned int	reg_offset;
	struct clk	*clk;
	struct clk      *clk480m;
	struct clk_hw	clk480m_hw;
	struct phy	*phy;
	bool		uart_enabled;
	bool		suspend_ctl;
	struct reset_control *reset;
	struct regulator *vbus;
};

/* UOC_CON1 suspend bit; see the register comment above. */
static int rockchip_usb_phy_suspend(struct rockchip_usb_phy *phy, bool suspend)
{
	if (!phy->suspend_ctl)
		return 0;

	return regmap_write(phy->base->reg_base, phy->reg_offset + UOC_CON1,
			    FIELD_PREP_WM16(UOC_CON1_SUSPEND, suspend));
}

static int rockchip_usb_phy_power(struct rockchip_usb_phy *phy,
					   bool siddq)
{
	u32 val = FIELD_PREP_WM16(UOC_CON0_SIDDQ, siddq);

	return regmap_write(phy->base->reg_base, phy->reg_offset, val);
}

static unsigned long rockchip_usb_phy480m_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	return 480000000;
}

static void rockchip_usb_phy480m_disable(struct clk_hw *hw)
{
	struct rockchip_usb_phy *phy = container_of(hw,
						    struct rockchip_usb_phy,
						    clk480m_hw);

	if (phy->vbus)
		regulator_disable(phy->vbus);

	/* Power down usb phy analog blocks by set siddq 1 */
	rockchip_usb_phy_power(phy, 1);
}

static int rockchip_usb_phy480m_enable(struct clk_hw *hw)
{
	struct rockchip_usb_phy *phy = container_of(hw,
						    struct rockchip_usb_phy,
						    clk480m_hw);

	/* Power up usb phy analog blocks by set siddq 0 */
	return rockchip_usb_phy_power(phy, 0);
}

static int rockchip_usb_phy480m_is_enabled(struct clk_hw *hw)
{
	struct rockchip_usb_phy *phy = container_of(hw,
						    struct rockchip_usb_phy,
						    clk480m_hw);
	int ret;
	u32 val;

	ret = regmap_read(phy->base->reg_base, phy->reg_offset, &val);
	if (ret < 0)
		return ret;

	return (val & UOC_CON0_SIDDQ) ? 0 : 1;
}

static const struct clk_ops rockchip_usb_phy480m_ops = {
	.enable = rockchip_usb_phy480m_enable,
	.disable = rockchip_usb_phy480m_disable,
	.is_enabled = rockchip_usb_phy480m_is_enabled,
	.recalc_rate = rockchip_usb_phy480m_recalc_rate,
};

static int rockchip_usb_phy_power_off(struct phy *_phy)
{
	struct rockchip_usb_phy *phy = phy_get_drvdata(_phy);

	if (phy->uart_enabled)
		return -EBUSY;

	rockchip_usb_phy_suspend(phy, true);
	clk_disable_unprepare(phy->clk480m);

	return 0;
}

static int rockchip_usb_phy_power_on(struct phy *_phy)
{
	struct rockchip_usb_phy *phy = phy_get_drvdata(_phy);

	if (phy->uart_enabled)
		return -EBUSY;

	if (phy->vbus) {
		int ret;

		ret = regulator_enable(phy->vbus);
		if (ret)
			return ret;
	}

	/*
	 * Reset the PHY here rather than in the .reset op, because dwc2 -- the
	 * only consumer on this SoC -- calls phy_init() and phy_power_on() but
	 * NEVER phy_reset(), so that op is dead code for it (dwc2/platform.c).
	 * Opt-in via a "phy-reset" reset in the node, so a PHY that does not
	 * declare one behaves exactly as before.
	 */
	if (phy->suspend_ctl && phy->reset) {
		reset_control_assert(phy->reset);
		udelay(15);
		reset_control_deassert(phy->reset);
		/*
		 * The vendor waits 1500 us after deassert before using the block
		 * (usb20otg_soft_reset); the UTMI PLL needs to relock.
		 */
		usleep_range(1500, 2000);
	}

	rockchip_usb_phy_suspend(phy, false);

	return clk_prepare_enable(phy->clk480m);
}

static int rockchip_usb_phy_reset(struct phy *_phy)
{
	struct rockchip_usb_phy *phy = phy_get_drvdata(_phy);

	if (phy->reset) {
		reset_control_assert(phy->reset);
		udelay(10);
		reset_control_deassert(phy->reset);
	}

	return 0;
}

static const struct phy_ops ops = {
	.power_on	= rockchip_usb_phy_power_on,
	.power_off	= rockchip_usb_phy_power_off,
	.reset		= rockchip_usb_phy_reset,
	.owner		= THIS_MODULE,
};

static void rockchip_usb_phy_action(void *data)
{
	struct rockchip_usb_phy *rk_phy = data;

	if (!rk_phy->uart_enabled) {
		of_clk_del_provider(rk_phy->np);
		clk_unregister(rk_phy->clk480m);
	}

	if (rk_phy->clk)
		clk_put(rk_phy->clk);
}

static int rockchip_usb_phy_init(struct rockchip_usb_phy_base *base,
				 struct device_node *child)
{
	struct rockchip_usb_phy *rk_phy;
	unsigned int reg_offset;
	const char *clk_name;
	struct clk_init_data init;
	int err, i;

	rk_phy = devm_kzalloc(base->dev, sizeof(*rk_phy), GFP_KERNEL);
	if (!rk_phy)
		return -ENOMEM;

	rk_phy->base = base;
	rk_phy->np = child;

	if (of_property_read_u32(child, "reg", &reg_offset)) {
		dev_err(base->dev, "missing reg property in node %pOFn\n",
			child);
		return -EINVAL;
	}

	rk_phy->reset = of_reset_control_get(child, "phy-reset");
	if (IS_ERR(rk_phy->reset))
		rk_phy->reset = NULL;

	rk_phy->reg_offset = reg_offset;

	rk_phy->clk = of_clk_get_by_name(child, "phyclk");
	if (IS_ERR(rk_phy->clk))
		rk_phy->clk = NULL;

	i = 0;
	init.name = NULL;
	while (base->pdata->phys[i].reg) {
		if (base->pdata->phys[i].reg == reg_offset) {
			init.name = base->pdata->phys[i].pll_name;
			rk_phy->suspend_ctl = base->pdata->phys[i].suspend_ctl;
			break;
		}
		i++;
	}

	if (!init.name) {
		dev_err(base->dev, "phy data not found\n");
		return -EINVAL;
	}

	if (enable_usb_uart && base->pdata->usb_uart_phy == i) {
		dev_dbg(base->dev, "phy%d used as uart output\n", i);
		rk_phy->uart_enabled = true;
	} else {
		if (rk_phy->clk) {
			clk_name = __clk_get_name(rk_phy->clk);
			init.flags = 0;
			init.parent_names = &clk_name;
			init.num_parents = 1;
		} else {
			init.flags = 0;
			init.parent_names = NULL;
			init.num_parents = 0;
		}

		init.ops = &rockchip_usb_phy480m_ops;
		rk_phy->clk480m_hw.init = &init;

		rk_phy->clk480m = clk_register(base->dev, &rk_phy->clk480m_hw);
		if (IS_ERR(rk_phy->clk480m)) {
			err = PTR_ERR(rk_phy->clk480m);
			goto err_clk;
		}

		err = of_clk_add_provider(child, of_clk_src_simple_get,
					rk_phy->clk480m);
		if (err < 0)
			goto err_clk_prov;
	}

	err = devm_add_action_or_reset(base->dev, rockchip_usb_phy_action,
				       rk_phy);
	if (err)
		return err;

	rk_phy->phy = devm_phy_create(base->dev, child, &ops);
	if (IS_ERR(rk_phy->phy)) {
		dev_err(base->dev, "failed to create PHY\n");
		return PTR_ERR(rk_phy->phy);
	}
	phy_set_drvdata(rk_phy->phy, rk_phy);

	rk_phy->vbus = devm_regulator_get_optional(&rk_phy->phy->dev, "vbus");
	if (IS_ERR(rk_phy->vbus)) {
		if (PTR_ERR(rk_phy->vbus) == -EPROBE_DEFER)
			return PTR_ERR(rk_phy->vbus);
		rk_phy->vbus = NULL;
	}

	/*
	 * When acting as uart-pipe, just keep clock on otherwise
	 * only power up usb phy when it use, so disable it when init
	 */
	if (rk_phy->uart_enabled)
		return clk_prepare_enable(rk_phy->clk);
	else
		return rockchip_usb_phy_power(rk_phy, 1);

err_clk_prov:
	if (!rk_phy->uart_enabled)
		clk_unregister(rk_phy->clk480m);
err_clk:
	if (rk_phy->clk)
		clk_put(rk_phy->clk);
	return err;
}

/*
 * The pll_name here is the name this driver REGISTERS for the PHY's 480 MHz
 * output, not a clock it looks up -- so it must not collide with anything the
 * CRU already registers. clk-rk3026.c has gates called "sclk_otgphy0" and
 * "sclk_otgphy1" (as mainline's rk3036 and rk3128 CRUs do), so naming the PLLs
 * the same made clk_hw_register() return -EEXIST and the phy probe fail with
 * -17. Both USB nodes then deferred forever and no UDC ever appeared -- the
 * long-standing "adbd is silent and a replug does not help" symptom.
 *
 * Every other pdata in this file uses the _480m suffix for exactly this
 * reason; rk3026 was added here without it.
 */
static const struct rockchip_usb_phy_pdata rk3026_pdata = {
	.phys = (struct rockchip_usb_phys[]){
		{ .reg = 0x17c, .pll_name = "sclk_otgphy0_480m" }, /* GRF_UOC0_CON0 */
		/*
		 * Host PHY. suspend_ctl only here: it is the one that needs
		 * UOC_CON1's suspend bit cleared and its UTMI reset pulsed, and
		 * the OTG PHY next to it carries this board's only console --
		 * see the UOC_CON1 comment and docs/SETTLED.md 2026-08-04.
		 */
		{ .reg = 0x190, .pll_name = "sclk_otgphy1_480m",
		  .suspend_ctl = true },			  /* GRF_UOC1_CON0 */
		{ /* sentinel */ }
	},
};

static const struct rockchip_usb_phy_pdata rk3066a_pdata = {
	.phys = (struct rockchip_usb_phys[]){
		{ .reg = 0x17c, .pll_name = "sclk_otgphy0_480m" },
		{ .reg = 0x188, .pll_name = "sclk_otgphy1_480m" },
		{ /* sentinel */ }
	},
};

static int __init rockchip_init_usb_uart_common(struct regmap *grf,
				const struct rockchip_usb_phy_pdata *pdata)
{
	int regoffs = pdata->phys[pdata->usb_uart_phy].reg;
	int ret;
	u32 val;

	/*
	 * COMMON_ON and DISABLE settings are described in the TRM,
	 * but were not present in the original code.
	 * Also disable the analog phy components to save power.
	 */
	val = FIELD_PREP_WM16(UOC_CON0_COMMON_ON_N, 1) |
	      FIELD_PREP_WM16(UOC_CON0_DISABLE, 1) |
	      FIELD_PREP_WM16(UOC_CON0_SIDDQ, 1);
	ret = regmap_write(grf, regoffs + UOC_CON0, val);
	if (ret)
		return ret;

	val = FIELD_PREP_WM16(UOC_CON2_SOFT_CON_SEL, 1);
	ret = regmap_write(grf, regoffs + UOC_CON2, val);
	if (ret)
		return ret;

	val = FIELD_PREP_WM16(UOC_CON3_UTMI_SUSPENDN, 0) |
	      FIELD_PREP_WM16(UOC_CON3_UTMI_OPMODE_MASK,
			   UOC_CON3_UTMI_OPMODE_NODRIVING) |
	      FIELD_PREP_WM16(UOC_CON3_UTMI_XCVRSEELCT_MASK,
			   UOC_CON3_UTMI_XCVRSEELCT_FSTRANSC) |
	      FIELD_PREP_WM16(UOC_CON3_UTMI_TERMSEL_FULLSPEED, 1);
	ret = regmap_write(grf, UOC_CON3, val);
	if (ret)
		return ret;

	return 0;
}

#define RK3188_UOC0_CON0				0x10c
#define RK3188_UOC0_CON0_BYPASSSEL			BIT(9)
#define RK3188_UOC0_CON0_BYPASSDMEN			BIT(8)

/*
 * Enable the bypass of uart2 data through the otg usb phy.
 * See description of rk3288-variant for details.
 */
static int __init rk3188_init_usb_uart(struct regmap *grf,
				const struct rockchip_usb_phy_pdata *pdata)
{
	u32 val;
	int ret;

	ret = rockchip_init_usb_uart_common(grf, pdata);
	if (ret)
		return ret;

	val = FIELD_PREP_WM16(RK3188_UOC0_CON0_BYPASSSEL, 1) |
	      FIELD_PREP_WM16(RK3188_UOC0_CON0_BYPASSDMEN, 1);
	ret = regmap_write(grf, RK3188_UOC0_CON0, val);
	if (ret)
		return ret;

	return 0;
}

static const struct rockchip_usb_phy_pdata rk3188_pdata = {
	.phys = (struct rockchip_usb_phys[]){
		{ .reg = 0x10c, .pll_name = "sclk_otgphy0_480m" },
		{ .reg = 0x11c, .pll_name = "sclk_otgphy1_480m" },
		{ /* sentinel */ }
	},
	.init_usb_uart = rk3188_init_usb_uart,
	.usb_uart_phy = 0,
};

#define RK3288_UOC0_CON3				0x32c
#define RK3288_UOC0_CON3_BYPASSDMEN			BIT(6)
#define RK3288_UOC0_CON3_BYPASSSEL			BIT(7)

/*
 * Enable the bypass of uart2 data through the otg usb phy.
 * Original description in the TRM.
 * 1. Disable the OTG block by setting OTGDISABLE0 to 1’b1.
 * 2. Disable the pull-up resistance on the D+ line by setting
 *    OPMODE0[1:0] to 2’b01.
 * 3. To ensure that the XO, Bias, and PLL blocks are powered down in Suspend
 *    mode, set COMMONONN to 1’b1.
 * 4. Place the USB PHY in Suspend mode by setting SUSPENDM0 to 1’b0.
 * 5. Set BYPASSSEL0 to 1’b1.
 * 6. To transmit data, controls BYPASSDMEN0, and BYPASSDMDATA0.
 * To receive data, monitor FSVPLUS0.
 *
 * The actual code in the vendor kernel does some things differently.
 */
static int __init rk3288_init_usb_uart(struct regmap *grf,
				const struct rockchip_usb_phy_pdata *pdata)
{
	u32 val;
	int ret;

	ret = rockchip_init_usb_uart_common(grf, pdata);
	if (ret)
		return ret;

	val = FIELD_PREP_WM16(RK3288_UOC0_CON3_BYPASSSEL, 1) |
	      FIELD_PREP_WM16(RK3288_UOC0_CON3_BYPASSDMEN, 1);
	ret = regmap_write(grf, RK3288_UOC0_CON3, val);
	if (ret)
		return ret;

	return 0;
}

static const struct rockchip_usb_phy_pdata rk3288_pdata = {
	.phys = (struct rockchip_usb_phys[]){
		{ .reg = 0x320, .pll_name = "sclk_otgphy0_480m" },
		{ .reg = 0x334, .pll_name = "sclk_otgphy1_480m" },
		{ .reg = 0x348, .pll_name = "sclk_otgphy2_480m" },
		{ /* sentinel */ }
	},
	.init_usb_uart = rk3288_init_usb_uart,
	.usb_uart_phy = 0,
};

static int rockchip_usb_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rockchip_usb_phy_base *phy_base;
	struct phy_provider *phy_provider;
	int err;

	phy_base = devm_kzalloc(dev, sizeof(*phy_base), GFP_KERNEL);
	if (!phy_base)
		return -ENOMEM;

	phy_base->pdata = device_get_match_data(dev);
	if (!phy_base->pdata) {
		dev_err(dev, "missing phy data\n");
		return -EINVAL;
	}

	phy_base->dev = dev;
	phy_base->reg_base = ERR_PTR(-ENODEV);
	if (dev->parent && dev->parent->of_node)
		phy_base->reg_base = syscon_node_to_regmap(
						dev->parent->of_node);
	if (IS_ERR(phy_base->reg_base))
		phy_base->reg_base = syscon_regmap_lookup_by_phandle(
						dev->of_node, "rockchip,grf");
	if (IS_ERR(phy_base->reg_base)) {
		dev_err(&pdev->dev, "Missing rockchip,grf property\n");
		return PTR_ERR(phy_base->reg_base);
	}

	for_each_available_child_of_node_scoped(dev->of_node, child) {
		err = rockchip_usb_phy_init(phy_base, child);
		if (err)
			return err;
	}

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct of_device_id rockchip_usb_phy_dt_ids[] = {
	{ .compatible = "rockchip,rk3026-usb-phy",  .data = &rk3026_pdata },
	{ .compatible = "rockchip,rk3066a-usb-phy", .data = &rk3066a_pdata },
	{ .compatible = "rockchip,rk3188-usb-phy", .data = &rk3188_pdata },
	{ .compatible = "rockchip,rk3288-usb-phy", .data = &rk3288_pdata },
	{}
};

MODULE_DEVICE_TABLE(of, rockchip_usb_phy_dt_ids);

static struct platform_driver rockchip_usb_driver = {
	.probe		= rockchip_usb_phy_probe,
	.driver		= {
		.name	= "rockchip-usb-phy",
		.of_match_table = rockchip_usb_phy_dt_ids,
	},
};

module_platform_driver(rockchip_usb_driver);

#ifndef MODULE
static int __init rockchip_init_usb_uart(void)
{
	const struct of_device_id *match;
	const struct rockchip_usb_phy_pdata *data;
	struct device_node *np;
	struct regmap *grf;
	int ret;

	if (!enable_usb_uart)
		return 0;

	np = of_find_matching_node_and_match(NULL, rockchip_usb_phy_dt_ids,
					     &match);
	if (!np) {
		pr_err("%s: failed to find usbphy node\n", __func__);
		return -ENOTSUPP;
	}

	pr_debug("%s: using settings for %s\n", __func__, match->compatible);
	data = match->data;

	if (!data->init_usb_uart) {
		pr_err("%s: usb-uart not available on %s\n",
		       __func__, match->compatible);
		return -ENOTSUPP;
	}

	grf = ERR_PTR(-ENODEV);
	if (np->parent)
		grf = syscon_node_to_regmap(np->parent);
	if (IS_ERR(grf))
		grf = syscon_regmap_lookup_by_phandle(np, "rockchip,grf");
	if (IS_ERR(grf)) {
		pr_err("%s: Missing rockchip,grf property, %lu\n",
		       __func__, PTR_ERR(grf));
		return PTR_ERR(grf);
	}

	ret = data->init_usb_uart(grf, data);
	if (ret) {
		pr_err("%s: could not init usb_uart, %d\n", __func__, ret);
		enable_usb_uart = 0;
		return ret;
	}

	return 0;
}
early_initcall(rockchip_init_usb_uart);

static int __init rockchip_usb_uart(char *buf)
{
	enable_usb_uart = true;
	return 0;
}
early_param("rockchip.usb_uart", rockchip_usb_uart);
#endif

MODULE_AUTHOR("Yunzhi Li <lyz@rock-chips.com>");
MODULE_DESCRIPTION("Rockchip USB 2.0 PHY driver");
MODULE_LICENSE("GPL v2");
