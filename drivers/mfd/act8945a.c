// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MFD driver for Active-semi ACT8945A and ACT8931A PMICs
 *
 * Copyright (C) 2015 Atmel Corporation.
 *
 * Author: Wenyou Yang <wenyou.yang@atmel.com>
 */

#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

enum act8945a_chip_id {
	ACT8931A = 1,
	ACT8945A,
};

static const struct mfd_cell act8945a_devs[] = {
	{
		.name = "act8945a-regulator",
	},
	{
		.name = "act8945a-charger",
		.of_compatible = "active-semi,act8945a-charger",
	},
};

static const struct regmap_config act8945a_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

/*
 * ACT8931A: only the VSET and CTRL registers of the seven outputs can be
 * written. The chip has no 0x0B and no *_SUS registers, and its SYS and
 * APCH (charger) settings are left as the board has set them.
 */
static const struct mfd_cell act8931a_devs[] = {
	{
		.name = "act8931a-regulator",
	},
	{
		.name = "act8931a-charger",
	},
};

static const struct regmap_range act8931a_wr_ranges[] = {
	regmap_reg_range(0x20, 0x22),	/* DCDC1 VSET1, VSET2, CTRL */
	regmap_reg_range(0x30, 0x32),	/* DCDC2 VSET1, VSET2, CTRL */
	regmap_reg_range(0x40, 0x42),	/* DCDC3 VSET1, VSET2, CTRL */
	regmap_reg_range(0x50, 0x51),	/* LDO1 VSET, CTRL */
	regmap_reg_range(0x54, 0x55),	/* LDO2 VSET, CTRL */
	regmap_reg_range(0x60, 0x61),	/* LDO3 VSET, CTRL */
	regmap_reg_range(0x64, 0x65),	/* LDO4 VSET, CTRL */
};

static const struct regmap_access_table act8931a_wr_table = {
	.yes_ranges = act8931a_wr_ranges,
	.n_yes_ranges = ARRAY_SIZE(act8931a_wr_ranges),
};

static const struct regmap_range act8931a_rd_ranges[] = {
	regmap_reg_range(0x00, 0x01),	/* SYS0, SYS1 */
	regmap_reg_range(0x20, 0x22),
	regmap_reg_range(0x30, 0x32),
	regmap_reg_range(0x40, 0x42),
	regmap_reg_range(0x50, 0x51),
	regmap_reg_range(0x54, 0x55),
	regmap_reg_range(0x60, 0x61),
	regmap_reg_range(0x64, 0x65),
	regmap_reg_range(0x71, 0x71),	/* APCH1 (0x70 is reserved) */
	regmap_reg_range(0x78, 0x7a),	/* APCH status, masks, charger state */
};

static const struct regmap_access_table act8931a_rd_table = {
	.yes_ranges = act8931a_rd_ranges,
	.n_yes_ranges = ARRAY_SIZE(act8931a_rd_ranges),
};

/* reading 0x78 clears its interrupt status bits */
static const struct regmap_range act8931a_precious_ranges[] = {
	regmap_reg_range(0x78, 0x78),
};

static const struct regmap_access_table act8931a_precious_table = {
	.yes_ranges = act8931a_precious_ranges,
	.n_yes_ranges = ARRAY_SIZE(act8931a_precious_ranges),
};

static const struct regmap_config act8931a_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x7a,
	.wr_table = &act8931a_wr_table,
	.rd_table = &act8931a_rd_table,
	.precious_table = &act8931a_precious_table,
};

static int act8945a_i2c_probe(struct i2c_client *i2c)
{
	int ret;
	struct regmap *regmap;
	const struct regmap_config *config;
	const struct mfd_cell *cells;
	int n_cells;

	switch ((uintptr_t)i2c_get_match_data(i2c)) {
	case ACT8931A:
		config = &act8931a_regmap_config;
		cells = act8931a_devs;
		n_cells = ARRAY_SIZE(act8931a_devs);
		break;
	case ACT8945A:
		config = &act8945a_regmap_config;
		cells = act8945a_devs;
		n_cells = ARRAY_SIZE(act8945a_devs);
		break;
	default:
		return -ENODEV;
	}

	regmap = devm_regmap_init_i2c(i2c, config);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(&i2c->dev, "regmap init failed: %d\n", ret);
		return ret;
	}

	i2c_set_clientdata(i2c, regmap);

	ret = devm_mfd_add_devices(&i2c->dev, PLATFORM_DEVID_NONE,
				   cells, n_cells,
				   NULL, 0, NULL);
	if (ret) {
		dev_err(&i2c->dev, "Failed to add sub devices\n");
		return ret;
	}

	return 0;
}

static const struct i2c_device_id act8945a_i2c_id[] = {
	{ "act8931a", ACT8931A },
	{ "act8945a", ACT8945A },
	{}
};
MODULE_DEVICE_TABLE(i2c, act8945a_i2c_id);

static const struct of_device_id act8945a_of_match[] = {
	{ .compatible = "active-semi,act8931a", .data = (void *)ACT8931A },
	{ .compatible = "active-semi,act8945a", .data = (void *)ACT8945A },
	{},
};
MODULE_DEVICE_TABLE(of, act8945a_of_match);

static struct i2c_driver act8945a_i2c_driver = {
	.driver = {
		   .name = "act8945a",
		   .of_match_table = act8945a_of_match,
	},
	.probe = act8945a_i2c_probe,
	.id_table = act8945a_i2c_id,
};

static int __init act8945a_i2c_init(void)
{
	return i2c_add_driver(&act8945a_i2c_driver);
}
subsys_initcall(act8945a_i2c_init);

static void __exit act8945a_i2c_exit(void)
{
	i2c_del_driver(&act8945a_i2c_driver);
}
module_exit(act8945a_i2c_exit);

MODULE_DESCRIPTION("ACT8945A and ACT8931A PMIC multi-function driver");
MODULE_AUTHOR("Wenyou Yang <wenyou.yang@atmel.com>");
MODULE_LICENSE("GPL");
