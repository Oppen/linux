// SPDX-License-Identifier: GPL-2.0-only
/*
 * Active-semi ACT8931 PMU: regulators and charger state.
 *
 * Every write passes the regmap write callback, which allows only the
 * output registers, keeps the reserved bits and keeps each VSET (both of a
 * DC/DC) at or below the output's regulator-max-microvolt; the VSET
 * registers of an output without a maximum are not written.
 *
 * The ON bit is edge-triggered, so ON is the last edge written, not a
 * measurement. OK (power good) is set while the output is in regulation.
 *
 * The charger is only read (APCH 0x78 and 0x7a), and polled: the driver
 * enables none of its interrupts.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/i2c.h>
#include <linux/linear_range.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/workqueue.h>

#define ACT8931_SYS0		0x00
#define ACT8931_SYS1		0x01
#define ACT8931_DCDC1_VSET1	0x20
#define ACT8931_DCDC1_VSET2	0x21
#define ACT8931_DCDC1_CTRL	0x22
#define ACT8931_DCDC2_VSET1	0x30
#define ACT8931_DCDC2_VSET2	0x31
#define ACT8931_DCDC2_CTRL	0x32
#define ACT8931_DCDC3_VSET1	0x40
#define ACT8931_DCDC3_VSET2	0x41
#define ACT8931_DCDC3_CTRL	0x42
#define ACT8931_LDO1_VSET	0x50
#define ACT8931_LDO1_CTRL	0x51
#define ACT8931_LDO2_VSET	0x54
#define ACT8931_LDO2_CTRL	0x55
#define ACT8931_LDO3_VSET	0x60
#define ACT8931_LDO3_CTRL	0x61
#define ACT8931_LDO4_VSET	0x64
#define ACT8931_LDO4_CTRL	0x65
#define ACT8931_APCH_CFG	0x71
#define ACT8931_APCH_STATUS	0x78
#define ACT8931_APCH_INTR	0x79
#define ACT8931_APCH_STATE	0x7a

#define ACT8931_VSEL_MASK	GENMASK(5, 0)
#define ACT8931_VSET_RESERVED	GENMASK(7, 6)
#define ACT8931_CTRL_ON		BIT(7)
#define ACT8931_DCDC3_RESERVED	BIT(6)	/* in DCDC3 CTRL */
#define ACT8931_CTRL_DELAY	GENMASK(4, 2)	/* turn-on delay */
#define ACT8931_CTRL_OK		BIT(0)	/* power good */
#define ACT8931_N_VOLTAGES	64

#define ACT8931_STATUS_CHGDAT	BIT(0)	/* end of charge */
#define ACT8931_STATUS_INDAT	BIT(1)	/* valid input at CHGIN */
#define ACT8931_STATE_CSTATE	GENMASK(5, 4)
#define ACT8931_STATE_ACINSTAT	BIT(1)	/* ACIN above 1.2 V: AC mode */
/* the bits the properties are made of: a change raises a uevent */
#define ACT8931_STATUS_WATCH	(ACT8931_STATUS_CHGDAT | ACT8931_STATUS_INDAT)
#define ACT8931_STATE_WATCH	(ACT8931_STATE_CSTATE | ACT8931_STATE_ACINSTAT)

#define ACT8931_CSTATE_OFF	0	/* suspended, disabled or fault */
#define ACT8931_CSTATE_EOC	1
#define ACT8931_CSTATE_FAST	2	/* fast charge or top-off */
#define ACT8931_CSTATE_PRE	3

#define ACT8931_POLL_MS		5000

/*
 * Soft-start: 400 us for the DC/DC converters, 140 us for an LDO at
 * 2.9 V, both typical figures (datasheet); no figure is given for a VSET
 * step. The longer one is used for an enable and a voltage raise.
 */
#define ACT8931_SOFTSTART_US	400

enum {
	ACT8931_ID_DCDC1,
	ACT8931_ID_DCDC2,
	ACT8931_ID_DCDC3,
	ACT8931_ID_LDO1,
	ACT8931_ID_LDO2,
	ACT8931_ID_LDO3,
	ACT8931_ID_LDO4,
	ACT8931_NUM_RAILS,
};

struct act8931_rail {
	const char *name;
	const char *of_name;
	const char *supply;	/* the input pin, as the binding names it */
	unsigned int vset1;	/* DCDC only: the VSEL-low register */
	unsigned int vset;	/* LDO: the one VSET; DCDC: VSET2 */
	unsigned int ctrl;
	bool switchable;	/* may get enable/disable ops */
	bool settable;		/* may get set_voltage ops */
};

static const struct act8931_rail act8931_rails[ACT8931_NUM_RAILS] = {
	[ACT8931_ID_DCDC1] = {
		.name = "act8931-dcdc1", .of_name = "DCDC_REG1", .supply = "vp1",
		.vset1 = ACT8931_DCDC1_VSET1, .vset = ACT8931_DCDC1_VSET2,
		.ctrl = ACT8931_DCDC1_CTRL,
	},
	[ACT8931_ID_DCDC2] = {
		.name = "act8931-dcdc2", .of_name = "DCDC_REG2", .supply = "vp2",
		.vset1 = ACT8931_DCDC2_VSET1, .vset = ACT8931_DCDC2_VSET2,
		.ctrl = ACT8931_DCDC2_CTRL,
	},
	[ACT8931_ID_DCDC3] = {
		.name = "act8931-dcdc3", .of_name = "DCDC_REG3", .supply = "vp3",
		.vset1 = ACT8931_DCDC3_VSET1, .vset = ACT8931_DCDC3_VSET2,
		.ctrl = ACT8931_DCDC3_CTRL, .settable = true,
	},
	[ACT8931_ID_LDO1] = {
		.name = "act8931-ldo1", .of_name = "LDO_REG1", .supply = "inl",
		.vset = ACT8931_LDO1_VSET, .ctrl = ACT8931_LDO1_CTRL,
	},
	[ACT8931_ID_LDO2] = {
		.name = "act8931-ldo2", .of_name = "LDO_REG2", .supply = "inl",
		.vset = ACT8931_LDO2_VSET, .ctrl = ACT8931_LDO2_CTRL,
	},
	[ACT8931_ID_LDO3] = {
		.name = "act8931-ldo3", .of_name = "LDO_REG3", .supply = "inl",
		.vset = ACT8931_LDO3_VSET, .ctrl = ACT8931_LDO3_CTRL,
	},
	[ACT8931_ID_LDO4] = {
		.name = "act8931-ldo4", .of_name = "LDO_REG4", .supply = "inl",
		.vset = ACT8931_LDO4_VSET, .ctrl = ACT8931_LDO4_CTRL,
		.switchable = true,
	},
};

struct act8931 {
	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;
	bool vsel_high;
	/* highest selector a VSET write may set, for the outputs in capped */
	unsigned int cap[ACT8931_NUM_RAILS];
	DECLARE_BITMAP(capped, ACT8931_NUM_RAILS);
	struct of_regulator_match match[ACT8931_NUM_RAILS];
	struct regulator_desc desc[ACT8931_NUM_RAILS];
	struct power_supply_desc charger_desc;	/* type follows ACINSTAT */
	struct power_supply *charger;
	struct delayed_work poll;
	unsigned int last_status;
	unsigned int last_state;
};

/* ACT8931 output voltage table: three linear ranges of the 6-bit selector */
static const struct linear_range act8931_voltage_ranges[] = {
	REGULATOR_LINEAR_RANGE(600000, 0, 23, 25000),
	REGULATOR_LINEAR_RANGE(1200000, 24, 47, 50000),
	REGULATOR_LINEAR_RANGE(2400000, 48, 63, 100000),
};

static bool act8931_ctrl_enabled(unsigned int ctrl)
{
	return (ctrl & ACT8931_CTRL_ON) && (ctrl & ACT8931_CTRL_OK);
}

/*
 * The turn-on delay set in CTRL (DELAY n > 0: 2^n ms, up to 12% longer)
 * plus the soft-start
 */
static unsigned int act8931_turn_on_us(unsigned int ctrl)
{
	unsigned int delay = FIELD_GET(ACT8931_CTRL_DELAY, ctrl);

	return ACT8931_SOFTSTART_US + (delay ? 1120U << delay : 0);
}

/*
 * An output whose CTRL reads ON without power good may still be ramping
 * up: wait its turn-on time and read CTRL again.
 */
static int act8931_settle(struct regmap *map, unsigned int reg,
			  unsigned int *ctrl)
{
	if (!(*ctrl & ACT8931_CTRL_ON) || (*ctrl & ACT8931_CTRL_OK))
		return 0;
	fsleep(act8931_turn_on_us(*ctrl));
	return regmap_read(map, reg, ctrl);
}

/*
 * ON with OK is on, ON without OK is out of regulation, and ON reads 0
 * after a falling edge.
 */
static int act8931_get_status(struct regulator_dev *rdev)
{
	unsigned int ctrl;
	int ret;

	ret = regmap_read(rdev_get_regmap(rdev), rdev->desc->enable_reg, &ctrl);
	if (ret)
		return ret;
	if (!(ctrl & ACT8931_CTRL_ON))
		return REGULATOR_STATUS_OFF;
	if (!(ctrl & ACT8931_CTRL_OK))
		return REGULATOR_STATUS_ERROR;
	return REGULATOR_STATUS_ON;
}

/*
 * The core enables without a wait when this fails, and the enable reads
 * CTRL again: a failed read gives the longest turn-on time instead.
 */
static int act8931_enable_time(struct regulator_dev *rdev)
{
	unsigned int ctrl;

	if (regmap_read(rdev_get_regmap(rdev), rdev->desc->enable_reg, &ctrl))
		ctrl = ACT8931_CTRL_DELAY;
	return act8931_turn_on_us(ctrl);
}

/*
 * Enable: ON 0 then ON 1 from the value read back, the datasheet's
 * sequence; nothing is written when the output is on already (the core
 * enables an always-on or boot-on output at registration). An output ON
 * without power good after its turn-on time is never toggled: the falling
 * edge would cut a load that may still run from it.
 */
static int act8931_enable(struct regulator_dev *rdev)
{
	struct act8931 *chip = rdev_get_drvdata(rdev);
	struct regmap *map = rdev_get_regmap(rdev);
	unsigned int reg = rdev->desc->enable_reg;
	unsigned int ctrl;
	int ret;

	ret = regmap_read(map, reg, &ctrl);
	if (!ret)
		ret = act8931_settle(map, reg, &ctrl);
	if (ret)
		return ret;
	if (act8931_ctrl_enabled(ctrl))
		return 0;
	if (ctrl & ACT8931_CTRL_ON) {
		dev_err(chip->dev,
			"%s: ON without power good (CTRL 0x%02x): not toggled\n",
			rdev->desc->name, ctrl);
		return -EIO;
	}

	ret = regmap_write(map, reg, ctrl & ~ACT8931_CTRL_ON);
	if (ret)
		return ret;
	return regmap_write(map, reg, ctrl | ACT8931_CTRL_ON);
}

/*
 * Disable: ON 1 then ON 0. Nothing is written when ON reads 0: an output
 * kept on by the power-up sequence with ON at 0 cannot be told from one
 * that is off (an LDO that is off may read OK 1).
 */
static int act8931_disable(struct regulator_dev *rdev)
{
	struct regmap *map = rdev_get_regmap(rdev);
	unsigned int reg = rdev->desc->enable_reg;
	unsigned int ctrl;
	int ret;

	ret = regmap_read(map, reg, &ctrl);
	if (ret)
		return ret;
	if (!(ctrl & ACT8931_CTRL_ON))
		return 0;

	ret = regmap_write(map, reg, ctrl | ACT8931_CTRL_ON);
	if (ret)
		return ret;
	return regmap_write(map, reg, ctrl & ~ACT8931_CTRL_ON);
}

/*
 * The core calls this instead of its own delay: return that delay, and at
 * least the soft-start time for a raise (no slew rate is specified).
 */
static int act8931_set_voltage_time_sel(struct regulator_dev *rdev,
					unsigned int old_sel,
					unsigned int new_sel)
{
	int us = regulator_set_voltage_time_sel(rdev, old_sel, new_sel);

	if (us < 0 || new_sel <= old_sel)
		return us;
	return max(us, ACT8931_SOFTSTART_US);
}

/*
 * An output without enable/disable ops counts as enabled by ON alone, so
 * that a short drop of OK under load does not make the regulator core
 * see it as off; get_status reports OK.
 */
static const struct regulator_ops act8931_ro_ops = {
	.list_voltage		= regulator_list_voltage_linear_range,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.is_enabled		= regulator_is_enabled_regmap,
	.get_status		= act8931_get_status,
};

/* The write callback refuses a selector above the output's maximum */
static const struct regulator_ops act8931_set_ops = {
	.list_voltage		= regulator_list_voltage_linear_range,
	.map_voltage		= regulator_map_voltage_linear_range,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_sel	= regulator_set_voltage_sel_regmap,
	.set_voltage_time_sel	= act8931_set_voltage_time_sel,
	.is_enabled		= regulator_is_enabled_regmap,
	.get_status		= act8931_get_status,
};

/*
 * A switched output counts as enabled with ON and OK: enable has to see
 * an output that is not in regulation, and waits out a short drop of OK
 * itself before it reports an error.
 */
static const struct regulator_ops act8931_switch_ops = {
	.list_voltage		= regulator_list_voltage_linear_range,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.is_enabled		= regulator_is_enabled_regmap,
	.enable			= act8931_enable,
	.disable		= act8931_disable,
	.enable_time		= act8931_enable_time,
	.get_status		= act8931_get_status,
};

static bool act8931_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case ACT8931_SYS0:
	case ACT8931_SYS1:
	case ACT8931_DCDC1_VSET1 ... ACT8931_DCDC1_CTRL:
	case ACT8931_DCDC2_VSET1 ... ACT8931_DCDC2_CTRL:
	case ACT8931_DCDC3_VSET1 ... ACT8931_DCDC3_CTRL:
	case ACT8931_LDO1_VSET:
	case ACT8931_LDO1_CTRL:
	case ACT8931_LDO2_VSET:
	case ACT8931_LDO2_CTRL:
	case ACT8931_LDO3_VSET:
	case ACT8931_LDO3_CTRL:
	case ACT8931_LDO4_VSET:
	case ACT8931_LDO4_CTRL:
	case ACT8931_APCH_CFG:
	case ACT8931_APCH_STATUS:
	case ACT8931_APCH_INTR:
	case ACT8931_APCH_STATE:
		return true;
	default:
		return false;
	}
}

/*
 * The output an output register belongs to, with *vset true for a VSET
 * register, or -ENOENT for any other register.
 */
static int act8931_reg_rail(unsigned int reg, bool *vset)
{
	int i;

	for (i = 0; i < ACT8931_NUM_RAILS; i++) {
		const struct act8931_rail *rail = &act8931_rails[i];
		bool is_vset = reg == rail->vset ||
			       (rail->vset1 && reg == rail->vset1);

		if (is_vset || reg == rail->ctrl) {
			*vset = is_vset;
			return i;
		}
	}
	return -ENOENT;
}

static bool act8931_writeable_reg(struct device *dev, unsigned int reg)
{
	bool vset;

	return act8931_reg_rail(reg, &vset) >= 0;
}

/*
 * Reserved bits of an output register, which a write must keep. OK is a
 * status bit; its written value is not checked.
 */
static unsigned int act8931_reserved(unsigned int reg, bool vset)
{
	if (vset)
		return ACT8931_VSET_RESERVED;
	return reg == ACT8931_DCDC3_CTRL ? ACT8931_DCDC3_RESERVED : 0;
}

/*
 * APCH 0x78 bits 7:4 read back the charger interrupt status, which a
 * read clears
 */
static bool act8931_precious_reg(struct device *dev, unsigned int reg)
{
	return reg == ACT8931_APCH_STATUS;
}

static int act8931_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct act8931 *chip = context;
	int ret;

	ret = i2c_smbus_read_byte_data(chip->client, reg);
	if (ret < 0)
		return ret;

	*val = ret;
	return 0;
}

/* The one path to the bus for a write: the rule of the register first */
static int act8931_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct act8931 *chip = context;
	const char *why = NULL;
	unsigned int keep = 0, cur;
	bool vset;
	int id, ret;

	/* regmap refuses a register that is not writable before this */
	id = act8931_reg_rail(reg, &vset);
	if (id < 0)
		why = "not a writable register";
	else if (val > 0xff)
		why = "not a byte";
	else if (vset && (!test_bit(id, chip->capped) ||
			  (val & ACT8931_VSEL_MASK) > chip->cap[id]))
		why = "above the output's maximum";
	else
		keep = act8931_reserved(reg, vset);

	if (keep) {
		ret = act8931_reg_read(chip, reg, &cur);
		if (ret)
			return ret;
		if ((cur ^ val) & keep)
			why = "changes reserved bits";
	}

	if (why) {
		dev_err_ratelimited(chip->dev,
				    "write of 0x%02x to 0x%02x refused: %s\n",
				    val, reg, why);
		return -EPERM;
	}

	return i2c_smbus_write_byte_data(chip->client, reg, val);
}

static const struct regmap_config act8931_regmap_config = {
	.name		= "act8931",
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= ACT8931_APCH_STATE,
	.readable_reg	= act8931_readable_reg,
	.writeable_reg	= act8931_writeable_reg,
	.precious_reg	= act8931_precious_reg,
	.reg_read	= act8931_reg_read,
	.reg_write	= act8931_reg_write,
};

/*
 * The output's regulator-max-microvolt as the highest selector at or
 * below it in *sel; false if the node or the property is missing, the
 * node has a regulator-microvolt-offset (the output is then registered
 * without it) or the value is below the lowest step. Taken before the
 * regmap exists, as the bound of every VSET write.
 */
static bool act8931_max_sel(struct act8931 *chip, int id, unsigned int *sel)
{
	const struct regulator_init_data *init = chip->match[id].init_data;
	bool found;

	if (!init || init->constraints.max_uV <= 0 ||
	    init->constraints.uV_offset)
		return false;
	if (linear_range_get_selector_low_array(act8931_voltage_ranges,
						ARRAY_SIZE(act8931_voltage_ranges),
						init->constraints.max_uV,
						sel, &found)) {
		dev_warn(chip->dev, "%s: maximum %d uV is below the lowest step\n",
			 act8931_rails[id].of_name, init->constraints.max_uV);
		return false;
	}
	return true;
}

/*
 * The node's constraints, or NULL when the output is registered without
 * its node because the regulator core would write the output or refuse
 * the node at registration: a voltage offset, which the core takes off
 * the voltage it reads, only one end of the voltage range, or a voltage
 * outside it.
 */
static const struct regulation_constraints *
act8931_node_constraints(struct act8931 *chip, int id, unsigned int sel,
			 unsigned int uv)
{
	const struct regulator_init_data *init = chip->match[id].init_data;
	const char *name = act8931_rails[id].of_name;
	const struct regulation_constraints *c;

	if (!init)
		return NULL;
	c = &init->constraints;
	if (c->uV_offset) {
		dev_warn(chip->dev, "%s: regulator-microvolt-offset is not supported: registered without the node\n",
			 name);
		return NULL;
	}
	if (!c->min_uV && !c->max_uV)
		return c;
	if (c->min_uV <= 0 || c->max_uV < c->min_uV) {
		dev_warn(chip->dev, "%s: no usable voltage range in the node: registered without it\n",
			 name);
		return NULL;
	}
	if ((int)uv < c->min_uV || (int)uv > c->max_uV) {
		dev_warn(chip->dev, "%s: selector 0x%02x (%u uV) is outside the board's %d-%d uV; left as it is, registered without the node\n",
			 name, sel, uv, c->min_uV, c->max_uV);
		return NULL;
	}
	return c;
}

/*
 * Ops are given so that binding the driver moves no output, except that
 * the core switches on an always-on or boot-on LDO4 that is off:
 *  - DCDC3 gets set_voltage ops when its node states a range; the core
 *    keeps the voltage inside it;
 *  - LDO4 gets enable/disable ops when its node states one voltage
 *    (minimum = maximum), which it then runs at; whether it stays on is
 *    the board's choice (regulator-always-on);
 *  - the other outputs are only read.
 */
static void act8931_set_rail_ops(struct act8931 *chip, int id,
				 const struct regulation_constraints *c)
{
	const struct act8931_rail *rail = &act8931_rails[id];
	struct regulator_desc *desc = &chip->desc[id];
	bool range = c->min_uV > 0;

	if (rail->settable && range) {
		desc->ops = &act8931_set_ops;
	} else if (rail->switchable && range && c->min_uV == c->max_uV) {
		desc->ops = &act8931_switch_ops;
		desc->enable_mask = ACT8931_CTRL_ON | ACT8931_CTRL_OK;
		desc->enable_val = desc->enable_mask;
	} else if (rail->settable || rail->switchable) {
		dev_info(chip->dev, "%s: the node states no %s: read only\n",
			 rail->of_name, rail->settable ?
			 "voltage range" : "single voltage");
	}
}

/* Registers one output, bound to its node if the node is usable */
static int act8931_register_rail(struct act8931 *chip, int id)
{
	const struct act8931_rail *rail = &act8931_rails[id];
	const struct regulation_constraints *c;
	struct regulator_desc *desc = &chip->desc[id];
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	unsigned int sel, ctrl, uv;
	int ret;

	desc->name = rail->name;
	desc->supply_name = rail->supply;
	desc->id = id;
	desc->type = REGULATOR_VOLTAGE;
	desc->owner = THIS_MODULE;
	desc->ops = &act8931_ro_ops;
	desc->n_voltages = ACT8931_N_VOLTAGES;
	desc->linear_ranges = act8931_voltage_ranges;
	desc->n_linear_ranges = ARRAY_SIZE(act8931_voltage_ranges);
	desc->vsel_reg = (rail->vset1 && !chip->vsel_high) ?
			 rail->vset1 : rail->vset;
	desc->vsel_mask = ACT8931_VSEL_MASK;
	desc->enable_reg = rail->ctrl;
	desc->enable_mask = ACT8931_CTRL_ON;
	desc->enable_val = ACT8931_CTRL_ON;

	ret = regmap_read(chip->regmap, desc->vsel_reg, &sel);
	if (!ret)
		ret = regmap_read(chip->regmap, rail->ctrl, &ctrl);
	if (ret)
		return dev_err_probe(chip->dev, ret, "%s: read failed\n",
				     rail->of_name);
	sel &= ACT8931_VSEL_MASK;
	linear_range_get_value_array(act8931_voltage_ranges,
				     ARRAY_SIZE(act8931_voltage_ranges), sel, &uv);
	dev_dbg(chip->dev, "%s: selector 0x%02x = %u uV, CTRL 0x%02x\n",
		rail->of_name, sel, uv, ctrl);

	c = act8931_node_constraints(chip, id, sel, uv);
	if (c) {
		act8931_set_rail_ops(chip, id, c);
		config.init_data = chip->match[id].init_data;
		config.of_node = chip->match[id].of_node;
	}

	/*
	 * The core enables an always-on or boot-on output at registration,
	 * and a failed enable fails the registration. An LDO4 that stays ON
	 * without power good is left alone here and not registered.
	 */
	if (c && desc->ops == &act8931_switch_ops &&
	    (c->always_on || c->boot_on)) {
		ret = act8931_settle(chip->regmap, rail->ctrl, &ctrl);
		if (ret)
			return dev_err_probe(chip->dev, ret, "%s: read failed\n",
					     rail->of_name);
		if ((ctrl & ACT8931_CTRL_ON) && !(ctrl & ACT8931_CTRL_OK)) {
			dev_warn(chip->dev,
				 "%s: ON without power good (CTRL 0x%02x): not registered\n",
				 rail->of_name, ctrl);
			return 0;
		}
	}
	config.dev = chip->dev;
	config.regmap = chip->regmap;
	config.driver_data = chip;

	rdev = devm_regulator_register(chip->dev, desc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(chip->dev, PTR_ERR(rdev),
				     "%s: not registered\n", rail->of_name);
	return 0;
}

/* The one reader of APCH 0x78, whose bits 7:4 clear on read */
static int act8931_charger_read(struct act8931 *chip, unsigned int *status,
				unsigned int *state)
{
	int ret;

	ret = regmap_read(chip->regmap, ACT8931_APCH_STATUS, status);
	if (ret)
		return ret;
	return regmap_read(chip->regmap, ACT8931_APCH_STATE, state);
}

static int act8931_charger_status(unsigned int status, unsigned int state)
{
	switch (FIELD_GET(ACT8931_STATE_CSTATE, state)) {
	case ACT8931_CSTATE_PRE:
	case ACT8931_CSTATE_FAST:
		return POWER_SUPPLY_STATUS_CHARGING;
	case ACT8931_CSTATE_EOC:
		if (status & ACT8931_STATUS_CHGDAT)
			return POWER_SUPPLY_STATUS_FULL;
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
	case ACT8931_CSTATE_OFF:
	default:
		if (status & ACT8931_STATUS_INDAT)
			return POWER_SUPPLY_STATUS_NOT_CHARGING;
		return POWER_SUPPLY_STATUS_DISCHARGING;
	}
}

static int act8931_charger_type(unsigned int state)
{
	switch (FIELD_GET(ACT8931_STATE_CSTATE, state)) {
	case ACT8931_CSTATE_PRE:
		return POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
	case ACT8931_CSTATE_FAST:
		return POWER_SUPPLY_CHARGE_TYPE_FAST;
	default:
		return POWER_SUPPLY_CHARGE_TYPE_NONE;
	}
}

static enum power_supply_type act8931_supply_type(unsigned int state)
{
	if (state & ACT8931_STATE_ACINSTAT)
		return POWER_SUPPLY_TYPE_MAINS;
	return POWER_SUPPLY_TYPE_USB;
}

static int act8931_charger_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct act8931 *chip = power_supply_get_drvdata(psy);
	unsigned int status, state;
	int ret;

	ret = act8931_charger_read(chip, &status, &state);
	if (ret)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = !!(status & ACT8931_STATUS_INDAT);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = act8931_charger_status(status, state);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = act8931_charger_type(state);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const enum power_supply_property act8931_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
};

static const struct power_supply_desc act8931_charger_desc = {
	.name		= "act8931-charger",
	.properties	= act8931_charger_props,
	.num_properties	= ARRAY_SIZE(act8931_charger_props),
	.get_property	= act8931_charger_get_property,
};

static void act8931_poll_arm(struct act8931 *chip)
{
	queue_delayed_work(system_freezable_power_efficient_wq, &chip->poll,
			   msecs_to_jiffies(ACT8931_POLL_MS));
}

static void act8931_charger_poll(struct work_struct *work)
{
	struct act8931 *chip = container_of(work, struct act8931, poll.work);
	unsigned int status, state;

	if (!act8931_charger_read(chip, &status, &state)) {
		status &= ACT8931_STATUS_WATCH;
		state &= ACT8931_STATE_WATCH;
		if (status != chip->last_status || state != chip->last_state) {
			chip->last_status = status;
			chip->last_state = state;
			chip->charger_desc.type = act8931_supply_type(state);
			power_supply_changed(chip->charger);
		}
	}
	act8931_poll_arm(chip);
}

/*
 * The charger comes first: a failure there leaves no output registered
 * that a consumer could already hold.
 */
static int act8931_charger_init(struct act8931 *chip)
{
	struct power_supply_config psy_cfg = { };
	unsigned int status, state;
	int ret;

	ret = act8931_charger_read(chip, &status, &state);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "charger state read failed\n");
	chip->last_status = status & ACT8931_STATUS_WATCH;
	chip->last_state = state & ACT8931_STATE_WATCH;
	chip->charger_desc = act8931_charger_desc;
	chip->charger_desc.type = act8931_supply_type(state);

	/* no fwnode: the charger stays out of the board's supply graph */
	psy_cfg.drv_data = chip;
	chip->charger = devm_power_supply_register(chip->dev,
						   &chip->charger_desc,
						   &psy_cfg);
	if (IS_ERR(chip->charger))
		return dev_err_probe(chip->dev, PTR_ERR(chip->charger),
				     "charger registration failed\n");

	ret = devm_delayed_work_autocancel(chip->dev, &chip->poll,
					   act8931_charger_poll);
	if (ret)
		return ret;
	act8931_poll_arm(chip);
	return 0;
}

static int act8931_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *regs;
	struct act8931 *chip;
	unsigned int val;
	int i, n, ret;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->dev = dev;
	chip->client = client;
	chip->vsel_high = of_property_read_bool(dev->of_node,
						"active-semi,vsel-high");

	/*
	 * The nodes are matched here, not through .of_match: the bounds of
	 * the VSET writes come from them before the regmap exists.
	 */
	regs = of_get_child_by_name(dev->of_node, "regulators");
	if (regs) {
		for (i = 0; i < ACT8931_NUM_RAILS; i++)
			chip->match[i].name = act8931_rails[i].of_name;
		n = of_get_child_count(regs);
		ret = of_regulator_match(dev, regs, chip->match,
					 ACT8931_NUM_RAILS);
		of_node_put(regs);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "failed to parse regulators node\n");
		if (ret < n)
			dev_warn(dev, "regulators: %d of %d child nodes name no output (DCDC_REG1-3, LDO_REG1-4)\n",
				 n - ret, n);
		/* a disabled node counts as absent, as in the core's lookup */
		for (i = 0; i < ACT8931_NUM_RAILS; i++)
			if (chip->match[i].of_node &&
			    !of_device_is_available(chip->match[i].of_node))
				chip->match[i].init_data = NULL;
	}
	for (i = 0; i < ACT8931_NUM_RAILS; i++)
		if (act8931_max_sel(chip, i, &chip->cap[i]))
			__set_bit(i, chip->capped);

	chip->regmap = devm_regmap_init(dev, NULL, chip,
					&act8931_regmap_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(dev, PTR_ERR(chip->regmap),
				     "regmap init failed\n");

	/* the chip has no ID register: check that it answers */
	ret = regmap_read(chip->regmap, ACT8931_SYS0, &val);
	if (ret)
		return dev_err_probe(dev, ret, "no answer at 0x%02x\n",
				     client->addr);

	ret = act8931_charger_init(chip);
	if (ret)
		return ret;

	for (i = 0; i < ACT8931_NUM_RAILS; i++) {
		ret = act8931_register_rail(chip, i);
		if (ret)
			return ret;
	}
	return 0;
}

static const struct of_device_id act8931_of_match[] = {
	{ .compatible = "active-semi,act8931" },
	{ }
};
MODULE_DEVICE_TABLE(of, act8931_of_match);

static const struct i2c_device_id act8931_ids[] = {
	{ "act8931" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, act8931_ids);

static struct i2c_driver act8931_driver = {
	.driver = {
		.name		= "act8931",
		.of_match_table	= act8931_of_match,
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe		= act8931_probe,
	.id_table	= act8931_ids,
};
module_i2c_driver(act8931_driver);

MODULE_DESCRIPTION("Active-semi ACT8931 regulators and charger state");
MODULE_AUTHOR("Sergei Khomenkov <shomenkow@gmail.com>");
MODULE_LICENSE("GPL");
