// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Voltage regulation driver for active-semi ACT8945A and ACT8931A PMICs
 *
 * Copyright (C) 2015 Atmel Corporation
 *
 * Author: Wenyou Yang <wenyou.yang@atmel.com>
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <dt-bindings/regulator/active-semi,8945a-regulator.h>

/*
 * ACT8945A Global Register Map.
 */
#define ACT8945A_SYS_MODE	0x00
#define ACT8945A_SYS_CTRL	0x01
#define ACT8945A_SYS_UNLK_REGS	0x0b
#define ACT8945A_DCDC1_VSET1	0x20
#define ACT8945A_DCDC1_VSET2	0x21
#define ACT8945A_DCDC1_CTRL	0x22
#define ACT8945A_DCDC1_SUS	0x24
#define ACT8945A_DCDC2_VSET1	0x30
#define ACT8945A_DCDC2_VSET2	0x31
#define ACT8945A_DCDC2_CTRL	0x32
#define ACT8945A_DCDC2_SUS	0x34
#define ACT8945A_DCDC3_VSET1	0x40
#define ACT8945A_DCDC3_VSET2	0x41
#define ACT8945A_DCDC3_CTRL	0x42
#define ACT8945A_DCDC3_SUS	0x44
#define ACT8945A_LDO1_VSET	0x50
#define ACT8945A_LDO1_CTRL	0x51
#define ACT8945A_LDO1_SUS	0x52
#define ACT8945A_LDO2_VSET	0x54
#define ACT8945A_LDO2_CTRL	0x55
#define ACT8945A_LDO2_SUS	0x56
#define ACT8945A_LDO3_VSET	0x60
#define ACT8945A_LDO3_CTRL	0x61
#define ACT8945A_LDO3_SUS	0x62
#define ACT8945A_LDO4_VSET	0x64
#define ACT8945A_LDO4_CTRL	0x65
#define ACT8945A_LDO4_SUS	0x66

/*
 * Field Definitions.
 */
#define ACT8945A_ENA		0x80	/* ON - [7] */
#define ACT8945A_VSEL_MASK	0x3F	/* VSET - [5:0] */
#define ACT8931A_CTRL_DELAY	GENMASK(4, 2)	/* turn-on delay */
#define ACT8931A_CTRL_OK	BIT(0)		/* output in regulation */

/*
 * ACT8931A soft-start: 400 us for the DC/DC converters, 140 us for an LDO
 * (typical). The datasheet gives no slew rate for a VSET change; the longer
 * soft-start is used for an enable and as the least wait for a raise.
 */
#define ACT8931A_SOFTSTART_US	400

/*
 * ACT8945A Voltage Number
 */
#define ACT8945A_VOLTAGE_NUM	64

enum {
	ACT8945A_ID_DCDC1,
	ACT8945A_ID_DCDC2,
	ACT8945A_ID_DCDC3,
	ACT8945A_ID_LDO1,
	ACT8945A_ID_LDO2,
	ACT8945A_ID_LDO3,
	ACT8945A_ID_LDO4,
	ACT8945A_ID_MAX,
};

enum act8945a_type {
	ACT8931A,
	ACT8945A,
};

struct act8945a_pmic {
	struct regmap *regmap;
	enum act8945a_type type;
	u32 op_mode[ACT8945A_ID_MAX];
	unsigned long not_toggled;	/* ACT8931A: enable refused, by ID */
};

static const struct linear_range act8945a_voltage_ranges[] = {
	REGULATOR_LINEAR_RANGE(600000, 0, 23, 25000),
	REGULATOR_LINEAR_RANGE(1200000, 24, 47, 50000),
	REGULATOR_LINEAR_RANGE(2400000, 48, 63, 100000),
};

static int act8945a_set_suspend_state(struct regulator_dev *rdev, bool enable)
{
	struct regmap *regmap = rdev->regmap;
	int id = rdev_get_id(rdev);
	int reg, val;

	switch (id) {
	case ACT8945A_ID_DCDC1:
		reg = ACT8945A_DCDC1_SUS;
		val = 0xa8;
		break;
	case ACT8945A_ID_DCDC2:
		reg = ACT8945A_DCDC2_SUS;
		val = 0xa8;
		break;
	case ACT8945A_ID_DCDC3:
		reg = ACT8945A_DCDC3_SUS;
		val = 0xa8;
		break;
	case ACT8945A_ID_LDO1:
		reg = ACT8945A_LDO1_SUS;
		val = 0xe8;
		break;
	case ACT8945A_ID_LDO2:
		reg = ACT8945A_LDO2_SUS;
		val = 0xe8;
		break;
	case ACT8945A_ID_LDO3:
		reg = ACT8945A_LDO3_SUS;
		val = 0xe8;
		break;
	case ACT8945A_ID_LDO4:
		reg = ACT8945A_LDO4_SUS;
		val = 0xe8;
		break;
	default:
		return -EINVAL;
	}

	if (enable)
		val |= BIT(4);

	/*
	 * Ask the PMIC to enable/disable this output when entering hibernate
	 * mode.
	 */
	return regmap_write(regmap, reg, val);
}

static int act8945a_set_suspend_enable(struct regulator_dev *rdev)
{
	return act8945a_set_suspend_state(rdev, true);
}

static int act8945a_set_suspend_disable(struct regulator_dev *rdev)
{
	return act8945a_set_suspend_state(rdev, false);
}

static unsigned int act8945a_of_map_mode(unsigned int mode)
{
	switch (mode) {
	case ACT8945A_REGULATOR_MODE_FIXED:
	case ACT8945A_REGULATOR_MODE_NORMAL:
		return REGULATOR_MODE_NORMAL;
	case ACT8945A_REGULATOR_MODE_LOWPOWER:
		return REGULATOR_MODE_STANDBY;
	default:
		return REGULATOR_MODE_INVALID;
	}
}

static int act8945a_set_mode(struct regulator_dev *rdev, unsigned int mode)
{
	struct act8945a_pmic *act8945a = rdev_get_drvdata(rdev);
	struct regmap *regmap = rdev->regmap;
	int id = rdev_get_id(rdev);
	int reg, ret, val = 0;

	switch (id) {
	case ACT8945A_ID_DCDC1:
		reg = ACT8945A_DCDC1_CTRL;
		break;
	case ACT8945A_ID_DCDC2:
		reg = ACT8945A_DCDC2_CTRL;
		break;
	case ACT8945A_ID_DCDC3:
		reg = ACT8945A_DCDC3_CTRL;
		break;
	case ACT8945A_ID_LDO1:
		reg = ACT8945A_LDO1_CTRL;
		break;
	case ACT8945A_ID_LDO2:
		reg = ACT8945A_LDO2_CTRL;
		break;
	case ACT8945A_ID_LDO3:
		reg = ACT8945A_LDO3_CTRL;
		break;
	case ACT8945A_ID_LDO4:
		reg = ACT8945A_LDO4_CTRL;
		break;
	default:
		return -EINVAL;
	}

	switch (mode) {
	case REGULATOR_MODE_STANDBY:
		if (id > ACT8945A_ID_DCDC3)
			val = BIT(5);
		break;
	case REGULATOR_MODE_NORMAL:
		if (id <= ACT8945A_ID_DCDC3)
			val = BIT(5);
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_update_bits(regmap, reg, BIT(5), val);
	if (ret)
		return ret;

	act8945a->op_mode[id] = mode;

	return 0;
}

static unsigned int act8945a_get_mode(struct regulator_dev *rdev)
{
	struct act8945a_pmic *act8945a = rdev_get_drvdata(rdev);
	int id = rdev_get_id(rdev);

	if (id < ACT8945A_ID_DCDC1 || id >= ACT8945A_ID_MAX)
		return -EINVAL;

	return act8945a->op_mode[id];
}

static const struct regulator_ops act8945a_ops = {
	.list_voltage		= regulator_list_voltage_linear_range,
	.map_voltage		= regulator_map_voltage_linear_range,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_sel	= regulator_set_voltage_sel_regmap,
	.enable			= regulator_enable_regmap,
	.disable		= regulator_disable_regmap,
	.set_mode		= act8945a_set_mode,
	.get_mode		= act8945a_get_mode,
	.is_enabled		= regulator_is_enabled_regmap,
	.set_suspend_enable	= act8945a_set_suspend_enable,
	.set_suspend_disable	= act8945a_set_suspend_disable,
};

/*
 * ACT8931A: the ON bit of CTRL is edge-triggered. The output follows the
 * last 0->1 or 1->0 edge written to ON, and ON reads back that edge, not the
 * output state. OK is set while the output is in regulation. An LDO is
 * enabled when it reads ON and OK (a DC/DC: act8931a_dcdc_ops).
 */
static bool act8931a_ctrl_enabled(unsigned int ctrl)
{
	return (ctrl & ACT8945A_ENA) && (ctrl & ACT8931A_CTRL_OK);
}

/*
 * The turn-on delay set in CTRL (DELAY n > 0: 2^n ms, up to 12 % longer)
 * plus the soft-start
 */
static unsigned int act8931a_turn_on_us(unsigned int ctrl)
{
	unsigned int delay = FIELD_GET(ACT8931A_CTRL_DELAY, ctrl);

	return ACT8931A_SOFTSTART_US + (delay ? 1120U << delay : 0);
}

static int act8931a_is_enabled(struct regulator_dev *rdev)
{
	unsigned int ctrl;
	int ret;

	ret = regmap_read(rdev->regmap, rdev->desc->enable_reg, &ctrl);
	if (ret)
		return ret;
	return act8931a_ctrl_enabled(ctrl);
}

/* ON without OK is out of regulation; ON reads 0 after a falling edge */
static int act8931a_get_status(struct regulator_dev *rdev)
{
	unsigned int ctrl;
	int ret;

	ret = regmap_read(rdev->regmap, rdev->desc->enable_reg, &ctrl);
	if (ret)
		return ret;
	if (!(ctrl & ACT8945A_ENA))
		return REGULATOR_STATUS_OFF;
	if (!(ctrl & ACT8931A_CTRL_OK))
		return REGULATOR_STATUS_ERROR;
	return REGULATOR_STATUS_ON;
}

/*
 * The core asks for this before each enable, also for an always-on output
 * at registration: nothing to wait for when the output is enabled already.
 * A failed read gives the longest turn-on time.
 */
static int act8931a_enable_time(struct regulator_dev *rdev)
{
	unsigned int ctrl;

	if (regmap_read(rdev->regmap, rdev->desc->enable_reg, &ctrl))
		ctrl = ACT8931A_CTRL_DELAY;
	else if (act8931a_ctrl_enabled(ctrl))
		return 0;
	return act8931a_turn_on_us(ctrl);
}

/*
 * Read CTRL for an enable. An output that reads ON without OK may still be
 * ramping up: it gets its turn-on time to reach OK. One that still reads ON
 * without OK is not enabled: the falling edge that would have to come first
 * cuts a load that may still run from it.
 */
static int act8931a_read_ctrl(struct regulator_dev *rdev, unsigned int *ctrl)
{
	struct act8945a_pmic *act8945a = rdev_get_drvdata(rdev);
	unsigned int reg = rdev->desc->enable_reg;
	int ret;

	ret = regmap_read(rdev->regmap, reg, ctrl);
	if (ret || !(*ctrl & ACT8945A_ENA) || (*ctrl & ACT8931A_CTRL_OK))
		return ret;

	fsleep(act8931a_turn_on_us(*ctrl));
	ret = regmap_read(rdev->regmap, reg, ctrl);
	if (ret || !(*ctrl & ACT8945A_ENA) || (*ctrl & ACT8931A_CTRL_OK))
		return ret;

	dev_err(&rdev->dev, "%s: ON without OK (CTRL 0x%02x): not toggled\n",
		rdev->desc->name, *ctrl);
	set_bit(rdev_get_id(rdev), &act8945a->not_toggled);
	return -EIO;
}

/* Enable: a rising edge of ON; nothing when ON reads 1 (and OK) */
static int act8931a_enable(struct regulator_dev *rdev)
{
	unsigned int ctrl;
	int ret;

	ret = act8931a_read_ctrl(rdev, &ctrl);
	if (ret || (ctrl & ACT8945A_ENA))
		return ret;
	return regmap_write(rdev->regmap, rdev->desc->enable_reg,
			    ctrl | ACT8945A_ENA);
}

/*
 * Disable: a falling edge of ON, also when ON reads 1 without OK: the
 * requested state, and the only way to recover a faulted output. Nothing
 * when ON reads 0: an output kept on by the power-up sequence with ON at 0
 * cannot be told from one that is off (an LDO that is off may read OK).
 */
static int act8931a_disable(struct regulator_dev *rdev)
{
	unsigned int ctrl;
	int ret;

	ret = regmap_read(rdev->regmap, rdev->desc->enable_reg, &ctrl);
	if (ret || !(ctrl & ACT8945A_ENA))
		return ret;
	return regmap_write(rdev->regmap, rdev->desc->enable_reg,
			    ctrl & ~ACT8945A_ENA);
}

/*
 * The core calls this instead of its own ramp and settling time: return
 * that, and at least the soft-start time for a raise.
 */
static int act8931a_set_voltage_time_sel(struct regulator_dev *rdev,
					 unsigned int old_sel,
					 unsigned int new_sel)
{
	int us = regulator_set_voltage_time_sel(rdev, old_sel, new_sel);

	if (us < 0 || new_sel <= old_sel)
		return us;
	return max(us, ACT8931A_SOFTSTART_US);
}

/*
 * ACT8931A DC/DC converters get no disable op. A falling edge of ON switches
 * a converter off at once, and when it supplies the SoC (its core, memory or
 * I/O) the kernel cannot write the rising edge back: a node that lacks
 * regulator-always-on (the unused-regulator pass) or a force-disable would
 * stop the system instead of failing a call. The enable op stays, so that an
 * always-on or boot-on node gets ON set. The core may still log "disabling"
 * and send a disable event for such a converter; the output stays on.
 *
 * As the kernel never writes their falling edge, a DC/DC is enabled when it
 * reads ON; OK goes to get_status only. With ON and OK, an OK drop (a load
 * step, a raise still ramping) would make the core treat the rail as off
 * and skip the max-step stepping and the wait of the next voltage change.
 * Enable writes the rising edge when ON reads 0, nothing when it reads 1.
 */
static const struct regulator_ops act8931a_dcdc_ops = {
	.list_voltage		= regulator_list_voltage_linear_range,
	.map_voltage		= regulator_map_voltage_linear_range,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_sel	= regulator_set_voltage_sel_regmap,
	.set_voltage_time_sel	= act8931a_set_voltage_time_sel,
	.enable			= regulator_enable_regmap,
	.enable_time		= act8931a_enable_time,
	.set_mode		= act8945a_set_mode,
	.get_mode		= act8945a_get_mode,
	.is_enabled		= regulator_is_enabled_regmap,
	.get_status		= act8931a_get_status,
};

static const struct regulator_ops act8931a_ldo_ops = {
	.list_voltage		= regulator_list_voltage_linear_range,
	.map_voltage		= regulator_map_voltage_linear_range,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_sel	= regulator_set_voltage_sel_regmap,
	.set_voltage_time_sel	= act8931a_set_voltage_time_sel,
	.enable			= act8931a_enable,
	.disable		= act8931a_disable,
	.enable_time		= act8931a_enable_time,
	.set_mode		= act8945a_set_mode,
	.get_mode		= act8945a_get_mode,
	.is_enabled		= act8931a_is_enabled,
	.get_status		= act8931a_get_status,
};

#define ACT89xx_REG(_name, _id, _vsel_reg, _supply, _ops)		\
	[ACT8945A_ID_##_id] = {						\
		.name			= _name,			\
		.supply_name		= _supply,			\
		.of_match		= of_match_ptr("REG_"#_id),	\
		.of_map_mode		= act8945a_of_map_mode,		\
		.regulators_node	= of_match_ptr("regulators"),	\
		.id			= ACT8945A_ID_##_id,		\
		.type			= REGULATOR_VOLTAGE,		\
		.ops			= _ops,				\
		.n_voltages		= ACT8945A_VOLTAGE_NUM,		\
		.linear_ranges		= act8945a_voltage_ranges,	\
		.n_linear_ranges	= ARRAY_SIZE(act8945a_voltage_ranges), \
		.vsel_reg		= ACT8945A_##_id##_##_vsel_reg,	\
		.vsel_mask		= ACT8945A_VSEL_MASK,		\
		.enable_reg		= ACT8945A_##_id##_CTRL,	\
		.enable_mask		= ACT8945A_ENA,			\
		.owner			= THIS_MODULE,			\
	}

static const struct regulator_desc act8945a_regulators[] = {
	ACT89xx_REG("DCDC_REG1", DCDC1, VSET1, "vp1", &act8945a_ops),
	ACT89xx_REG("DCDC_REG2", DCDC2, VSET1, "vp2", &act8945a_ops),
	ACT89xx_REG("DCDC_REG3", DCDC3, VSET1, "vp3", &act8945a_ops),
	ACT89xx_REG("LDO_REG1", LDO1, VSET, "inl45", &act8945a_ops),
	ACT89xx_REG("LDO_REG2", LDO2, VSET, "inl45", &act8945a_ops),
	ACT89xx_REG("LDO_REG3", LDO3, VSET, "inl67", &act8945a_ops),
	ACT89xx_REG("LDO_REG4", LDO4, VSET, "inl67", &act8945a_ops),
};

static const struct regulator_desc act8945a_alt_regulators[] = {
	ACT89xx_REG("DCDC_REG1", DCDC1, VSET2, "vp1", &act8945a_ops),
	ACT89xx_REG("DCDC_REG2", DCDC2, VSET2, "vp2", &act8945a_ops),
	ACT89xx_REG("DCDC_REG3", DCDC3, VSET2, "vp3", &act8945a_ops),
	ACT89xx_REG("LDO_REG1", LDO1, VSET, "inl45", &act8945a_ops),
	ACT89xx_REG("LDO_REG2", LDO2, VSET, "inl45", &act8945a_ops),
	ACT89xx_REG("LDO_REG3", LDO3, VSET, "inl67", &act8945a_ops),
	ACT89xx_REG("LDO_REG4", LDO4, VSET, "inl67", &act8945a_ops),
};

static const struct regulator_desc act8931a_regulators[] = {
	ACT89xx_REG("DCDC_REG1", DCDC1, VSET1, "vp1", &act8931a_dcdc_ops),
	ACT89xx_REG("DCDC_REG2", DCDC2, VSET1, "vp2", &act8931a_dcdc_ops),
	ACT89xx_REG("DCDC_REG3", DCDC3, VSET1, "vp3", &act8931a_dcdc_ops),
	ACT89xx_REG("LDO_REG1", LDO1, VSET, "inl45", &act8931a_ldo_ops),
	ACT89xx_REG("LDO_REG2", LDO2, VSET, "inl45", &act8931a_ldo_ops),
	ACT89xx_REG("LDO_REG3", LDO3, VSET, "inl67", &act8931a_ldo_ops),
	ACT89xx_REG("LDO_REG4", LDO4, VSET, "inl67", &act8931a_ldo_ops),
};

static const struct regulator_desc act8931a_alt_regulators[] = {
	ACT89xx_REG("DCDC_REG1", DCDC1, VSET2, "vp1", &act8931a_dcdc_ops),
	ACT89xx_REG("DCDC_REG2", DCDC2, VSET2, "vp2", &act8931a_dcdc_ops),
	ACT89xx_REG("DCDC_REG3", DCDC3, VSET2, "vp3", &act8931a_dcdc_ops),
	ACT89xx_REG("LDO_REG1", LDO1, VSET, "inl45", &act8931a_ldo_ops),
	ACT89xx_REG("LDO_REG2", LDO2, VSET, "inl45", &act8931a_ldo_ops),
	ACT89xx_REG("LDO_REG3", LDO3, VSET, "inl67", &act8931a_ldo_ops),
	ACT89xx_REG("LDO_REG4", LDO4, VSET, "inl67", &act8931a_ldo_ops),
};

/*
 * The ACT8931A outputs without an available node under "regulators": they
 * are not registered, where the regulator core would register them with
 * empty constraints. The ACT8945A registers all seven, as before.
 */
static unsigned long act8931a_absent_outputs(struct device_node *np,
					     const struct regulator_desc *descs,
					     int num)
{
	struct device_node *regs __free(device_node) =
		of_get_child_by_name(np, "regulators");
	unsigned long absent = 0;
	int i;

	for (i = 0; i < num; i++) {
		struct device_node *child __free(device_node) =
			of_get_available_child_by_name(regs, descs[i].of_match);

		if (!child)
			absent |= BIT(i);
	}

	return absent;
}

static int act8945a_pmic_probe(struct platform_device *pdev)
{
	const struct platform_device_id *id = platform_get_device_id(pdev);
	struct regulator_config config = { };
	const struct regulator_desc *regulators;
	struct act8945a_pmic *act8945a;
	struct regulator_dev *rdev;
	unsigned long absent = 0;
	int i, num_regulators;
	bool voltage_select;

	if (!id)
		return -ENODEV;

	act8945a = devm_kzalloc(&pdev->dev, sizeof(*act8945a), GFP_KERNEL);
	if (!act8945a)
		return -ENOMEM;

	act8945a->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!act8945a->regmap) {
		dev_err(&pdev->dev,
			"could not retrieve regmap from parent device\n");
		return -EINVAL;
	}

	act8945a->type = id->driver_data;

	voltage_select = of_property_read_bool(pdev->dev.parent->of_node,
					       "active-semi,vsel-high");

	if (voltage_select) {
		regulators = act8945a_alt_regulators;
		num_regulators = ARRAY_SIZE(act8945a_alt_regulators);
	} else {
		regulators = act8945a_regulators;
		num_regulators = ARRAY_SIZE(act8945a_regulators);
	}

	/*
	 * The ACT8931A: the same output registers, its own ops; only the
	 * outputs its node describes.
	 */
	if (act8945a->type == ACT8931A) {
		regulators = voltage_select ? act8931a_alt_regulators :
					      act8931a_regulators;
		absent = act8931a_absent_outputs(pdev->dev.parent->of_node,
						 regulators, num_regulators);
	}

	device_set_of_node_from_dev(&pdev->dev, pdev->dev.parent);

	config.dev = &pdev->dev;
	config.driver_data = act8945a;
	for (i = 0; i < num_regulators; i++) {
		if (absent & BIT(i))
			continue;

		rdev = devm_regulator_register(&pdev->dev, &regulators[i],
					       &config);
		/*
		 * An ACT8931A LDO that the core enables at registration
		 * (always-on, boot-on) and that reads ON without OK: the
		 * enable refused it with nothing written. Leave it out and
		 * register the others.
		 */
		if (PTR_ERR_OR_ZERO(rdev) == -EIO &&
		    test_bit(i, &act8945a->not_toggled)) {
			dev_warn(&pdev->dev,
				 "%s: ON without OK, not registered\n",
				 regulators[i].name);
			continue;
		}
		if (IS_ERR(rdev)) {
			dev_err(&pdev->dev,
				"failed to register %s regulator\n",
				regulators[i].name);
			return PTR_ERR(rdev);
		}
	}

	platform_set_drvdata(pdev, act8945a);

	/* The ACT8931A has no expert registers. */
	if (act8945a->type == ACT8931A)
		return 0;

	/* Unlock expert registers. */
	return regmap_write(act8945a->regmap, ACT8945A_SYS_UNLK_REGS, 0xef);
}

static int __maybe_unused act8945a_suspend(struct device *pdev)
{
	struct act8945a_pmic *act8945a = dev_get_drvdata(pdev);

	/* The ACT8931A has no suspend mode (SYS_CTRL bit 6, *_SUS). */
	if (act8945a->type == ACT8931A)
		return 0;

	/*
	 * Ask the PMIC to enter the suspend mode on the next PWRHLD
	 * transition.
	 */
	return regmap_write(act8945a->regmap, ACT8945A_SYS_CTRL, 0x42);
}

static SIMPLE_DEV_PM_OPS(act8945a_pm, act8945a_suspend, NULL);

static void act8945a_pmic_shutdown(struct platform_device *pdev)
{
	struct act8945a_pmic *act8945a = platform_get_drvdata(pdev);

	/*
	 * The ACT8931A has no shutdown mode in SYS_CTRL, and its outputs keep
	 * their voltages across a reset of the SoC. Nothing is put back here:
	 * a write from shutdown would bypass the core (constraints, coupling,
	 * notifiers) and race the other users of the rail. A CPU rail is its
	 * cpufreq driver's: cpufreq-dt switches to the opp-suspend point
	 * before the drivers shut down.
	 */
	if (act8945a->type == ACT8931A)
		return;

	/*
	 * Ask the PMIC to shutdown everything on the next PWRHLD transition.
	 */
	regmap_write(act8945a->regmap, ACT8945A_SYS_CTRL, 0x0);
}

static const struct platform_device_id act8945a_pmic_id[] = {
	{ "act8931a-regulator", ACT8931A },
	{ "act8945a-regulator", ACT8945A },
	{ }
};
MODULE_DEVICE_TABLE(platform, act8945a_pmic_id);

static struct platform_driver act8945a_pmic_driver = {
	.driver = {
		.name = "act8945a-regulator",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.pm = &act8945a_pm,
	},
	.probe = act8945a_pmic_probe,
	.shutdown = act8945a_pmic_shutdown,
	.id_table = act8945a_pmic_id,
};
module_platform_driver(act8945a_pmic_driver);

MODULE_DESCRIPTION("Active-semi ACT8945A/ACT8931A voltage regulator driver");
MODULE_AUTHOR("Wenyou Yang <wenyou.yang@atmel.com>");
MODULE_LICENSE("GPL");
