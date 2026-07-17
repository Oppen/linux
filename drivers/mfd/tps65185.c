// SPDX-License-Identifier: GPL-2.0-only
/*
 * TPS65185 EPD Power Management IC MFD driver
 *
 * Texas Instruments TPS65185 — I2C PMIC for E Ink electrophoretic displays.
 * Generates VPOS (+15V), VNEG (-15V), VDDH (+22V), VEE (-22V) rails required
 * by E Ink panels and provides a programmable VCOM bias voltage.
 *
 * Copyright (C) 2024 Mario Rugiero
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/mfd/tps65185.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

/* Register map */
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

#define TPS65185_NUM_REGS		(TPS65185_REG_REVID + 1)

/* ENABLE register bits */
#define TPS65185_VPOS_EN		BIT(5)
#define TPS65185_VEE_EN			BIT(4)
#define TPS65185_VDDH_EN		BIT(3)
#define TPS65185_VNEG_EN		BIT(2)
#define TPS65185_VCOM_EN		BIT(1)
#define TPS65185_V3P3_EN		BIT(0)

/* VCOM2 register bits */
#define TPS65185_VCOM2_MSB_MASK		GENMASK(1, 0)
#define TPS65185_VCOM2_HIZ		BIT(5)
#define TPS65185_VCOM2_PROG		BIT(6)
#define TPS65185_VCOM2_ACQ		BIT(7)

/* VCOM is programmed in 10 mV steps; value = abs(vcom_mv) / 10 */
#define TPS65185_VCOM_STEP_MV		10
#define TPS65185_VCOM_MAX_MV		5110
#define TPS65185_VCOM_10BIT_MASK	0x3ff

/* After WAKEUP assert, wait for EEPROM defaults to load */
#define TPS65185_EEPROM_DELAY_MS	50

struct tps65185 {
	struct device		*dev;
	struct regmap		*regmap;
	struct gpio_desc	*wake_gpio;
	struct gpio_desc	*vcom_ctrl_gpio;
	u32			vcom_mv;
};

/* MFD sub-devices */
static const struct mfd_cell tps65185_cells[] = {
	{
		.name = "tps65185-regulator",
		.of_compatible = "ti,tps65185-regulator",
	},
};

static const struct regmap_range tps65185_rd_ranges[] = {
	regmap_reg_range(TPS65185_REG_TMST_VALUE, TPS65185_REG_REVID),
};

static const struct regmap_range tps65185_wr_ranges[] = {
	regmap_reg_range(TPS65185_REG_ENABLE, TPS65185_REG_DWNSEQ1),
	regmap_reg_range(TPS65185_REG_TMST1, TPS65185_REG_TMST2),
};

static const struct regmap_access_table tps65185_rd_table = {
	.yes_ranges  = tps65185_rd_ranges,
	.n_yes_ranges = ARRAY_SIZE(tps65185_rd_ranges),
};

static const struct regmap_access_table tps65185_wr_table = {
	.yes_ranges  = tps65185_wr_ranges,
	.n_yes_ranges = ARRAY_SIZE(tps65185_wr_ranges),
};

static const struct regmap_config tps65185_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_registers	= TPS65185_NUM_REGS,
	.rd_table	= &tps65185_rd_table,
	.wr_table	= &tps65185_wr_table,
};

static int tps65185_program_vcom(struct tps65185 *tps)
{
	unsigned int vcom_val;
	u8 vcom1, vcom2;
	int ret;

	if (tps->vcom_mv > TPS65185_VCOM_MAX_MV) {
		dev_err(tps->dev, "vcom-voltage-mv %u exceeds maximum %u\n",
			tps->vcom_mv, TPS65185_VCOM_MAX_MV);
		return -EINVAL;
	}

	vcom_val = (tps->vcom_mv / TPS65185_VCOM_STEP_MV) & TPS65185_VCOM_10BIT_MASK;
	vcom1 = vcom_val & 0xff;
	vcom2 = (vcom_val >> 8) & TPS65185_VCOM2_MSB_MASK;

	ret = regmap_write(tps->regmap, TPS65185_REG_VCOM1, vcom1);
	if (ret)
		return ret;

	/* Write VCOM2 with PROG strobe bit set to commit the value */
	ret = regmap_write(tps->regmap, TPS65185_REG_VCOM2,
			   vcom2 | TPS65185_VCOM2_PROG);
	if (ret)
		return ret;

	dev_dbg(tps->dev, "VCOM programmed: -%u mV (raw 0x%03x)\n",
		tps->vcom_mv, vcom_val);
	return 0;
}

static int tps65185_probe(struct i2c_client *client)
{
	struct tps65185 *tps;
	unsigned int revid;
	int ret;

	tps = devm_kzalloc(&client->dev, sizeof(*tps), GFP_KERNEL);
	if (!tps)
		return -ENOMEM;

	tps->dev = &client->dev;
	i2c_set_clientdata(client, tps);

	/* Wake-up GPIO: assert high to bring TPS65185 out of sleep */
	tps->wake_gpio = devm_gpiod_get(&client->dev, "wake-up", GPIOD_OUT_HIGH);
	if (IS_ERR(tps->wake_gpio))
		return dev_err_probe(&client->dev, PTR_ERR(tps->wake_gpio),
				     "failed to get wake-up GPIO\n");

	/* VCOM control GPIO (optional — not all boards wire it) */
	tps->vcom_ctrl_gpio = devm_gpiod_get_optional(&client->dev, "vcom-ctrl",
						       GPIOD_OUT_LOW);
	if (IS_ERR(tps->vcom_ctrl_gpio))
		return dev_err_probe(&client->dev, PTR_ERR(tps->vcom_ctrl_gpio),
				     "failed to get vcom-ctrl GPIO\n");

	/* Allow EEPROM defaults to settle after wake-up */
	msleep(TPS65185_EEPROM_DELAY_MS);

	tps->regmap = devm_regmap_init_i2c(client, &tps65185_regmap_config);
	if (IS_ERR(tps->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(tps->regmap),
				     "failed to initialise regmap\n");

	/* Verify chip presence via REVID */
	ret = regmap_read(tps->regmap, TPS65185_REG_REVID, &revid);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to read REVID register\n");

	dev_info(&client->dev, "TPS65185 rev 0x%02x detected\n", revid);

	/* Read VCOM magnitude from DT; default 0 means skip programming */
	ret = device_property_read_u32(&client->dev, "vcom-voltage-mv",
				       &tps->vcom_mv);
	if (ret && ret != -EINVAL)
		return dev_err_probe(&client->dev, ret,
				     "failed to read vcom-voltage-mv\n");

	if (!ret && tps->vcom_mv) {
		ret = tps65185_program_vcom(tps);
		if (ret)
			return ret;
	}

	ret = devm_mfd_add_devices(&client->dev, PLATFORM_DEVID_AUTO,
				   tps65185_cells, ARRAY_SIZE(tps65185_cells),
				   NULL, 0, NULL);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to add MFD sub-devices\n");

	return 0;
}

static const struct of_device_id tps65185_of_match[] = {
	{ .compatible = "ti,tps65185" },
	{ }
};
MODULE_DEVICE_TABLE(of, tps65185_of_match);

static const struct i2c_device_id tps65185_id[] = {
	{ "tps65185", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tps65185_id);

static struct i2c_driver tps65185_driver = {
	.driver = {
		.name		= "tps65185",
		.of_match_table	= tps65185_of_match,
	},
	.probe		= tps65185_probe,
	.id_table	= tps65185_id,
};
module_i2c_driver(tps65185_driver);

MODULE_DESCRIPTION("TI TPS65185 EPD PMIC MFD driver");
MODULE_AUTHOR("Mario Rugiero");
MODULE_LICENSE("GPL");
