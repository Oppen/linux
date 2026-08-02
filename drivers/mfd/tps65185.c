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
#include <linux/regulator/driver.h>
#include <linux/thermal.h>

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

/* TMST1 register bits */
#define TPS65185_TMST1_CONV_END		BIT(5)
#define TPS65185_TMST1_READ_THERM	BIT(7)

/* Conversions observed to settle well inside 51 register polls. */
#define TPS65185_TMST_POLLS		51
#define TPS65185_TMST_SETTLE_MS		10

/* VCOM2 register bits */
#define TPS65185_VCOM2_MSB_MASK		BIT(0)
#define TPS65185_VCOM2_HIZ		BIT(5)
#define TPS65185_VCOM2_PROG		BIT(6)
#define TPS65185_VCOM2_ACQ		BIT(7)

/* VCOM is programmed in 10 mV steps; value = abs(vcom_mv) / 10 */
#define TPS65185_VCOM_STEP_MV		10
#define TPS65185_VCOM_MAX_MV		5110
/* 9-bit magnitude: papyrus_set_vcom_voltage keeps bit 8 in VCOM2 bit 0. */
#define TPS65185_VCOM_MASK		0x1ff

/* After WAKEUP assert, wait for EEPROM defaults to load */
#define TPS65185_EEPROM_DELAY_MS	50

/*
 * Rail sequencing, transcribed from papyrus_hw_power_req in the vendor kernel.
 * The five sequencing bytes are the ones its probe stores into the register
 * shadow, cross-checked against the same offsets in a live DRAM dump.
 */
#define TPS65185_VADJ_VALUE		0x03
#define TPS65185_UPSEQ0_VALUE		0xe4	/* VDDH, VPOS, VNEG, VEE */
#define TPS65185_UPSEQ1_VALUE		0x00	/* minimum strobe delays */
#define TPS65185_DWNSEQ0_VALUE		0x00
#define TPS65185_DWNSEQ1_VALUE		0x00

#define TPS65185_ENABLE_ACTIVE		BIT(7)
#define TPS65185_ENABLE_STANDBY		BIT(6)
#define TPS65185_ENABLE_V3P3_ONLY	0x20
#define TPS65185_ENABLE_UP		0xaf	/* ACTIVE | all rails */
#define TPS65185_ENABLE_RETRY		0x60
#define TPS65185_ENABLE_DOWN		0x6f	/* STANDBY | all rails */

/* Power-good: every rail reported up. Polled, not assumed. */
#define TPS65185_PG_ALL			0xfa
#define TPS65185_PG_POLLS		29
#define TPS65185_PG_DELAY_MS		20
#define TPS65185_UP_ATTEMPTS		3
#define TPS65185_UP_RETRY_MS		200
#define TPS65185_V3P3_SETTLE_US		2000

/*
 * Standby dwell before the next power-up. The vendor's is 0 on this unit --
 * papyrus_standby_dwell_time_ready compares whole seconds against zero and is
 * therefore always true, and the msleep it guards is passed a field that is 0.
 * Kept as a knob because too long only costs latency.
 */
#define TPS65185_DWELL_DEFAULT_MS	0

struct tps65185 {
	struct device		*dev;
	struct regmap		*regmap;
	struct gpio_desc	*wake_gpio;
	struct gpio_desc	*vcom_ctrl_gpio;
	u32			vcom_mv;
	u32			dwell_ms;
	unsigned long		off_jiffies;
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
	.max_register	= TPS65185_REG_REVID,
	.rd_table	= &tps65185_rd_table,
	.wr_table	= &tps65185_wr_table,
};

/* Magnitude in 10 mV steps: VCOM1 is the low byte, VCOM2 bit 0 is bit 8. */
static int tps65185_read_vcom(struct tps65185 *tps, unsigned int *steps)
{
	unsigned int vcom1, vcom2;
	int ret;

	ret = regmap_read(tps->regmap, TPS65185_REG_VCOM1, &vcom1);
	if (ret)
		return ret;
	ret = regmap_read(tps->regmap, TPS65185_REG_VCOM2, &vcom2);
	if (ret)
		return ret;

	*steps = vcom1 | ((vcom2 & TPS65185_VCOM2_MSB_MASK) << 8);
	return 0;
}

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

	vcom_val = (tps->vcom_mv / TPS65185_VCOM_STEP_MV) & TPS65185_VCOM_MASK;
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

/*
 * Panel thermometer. The waveform lookup buckets by temperature, so the EBC
 * driver reads this zone before every refresh: trigger a conversion, wait for
 * the chip to clear READ_THERM and raise CONV_END, then read the value out.
 */
static int tps65185_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct tps65185 *tps = thermal_zone_device_priv(tz);
	unsigned int val;
	int retry = TPS65185_TMST_POLLS;
	int ret;

	ret = regmap_write(tps->regmap, TPS65185_REG_TMST1,
			   TPS65185_TMST1_READ_THERM);
	if (ret)
		return ret;

	do {
		ret = regmap_read(tps->regmap, TPS65185_REG_TMST1, &val);
		if (ret)
			return ret;
		if ((val & TPS65185_TMST1_CONV_END) &&
		    !(val & TPS65185_TMST1_READ_THERM))
			break;
	} while (--retry);

	if (!retry)
		return -ETIMEDOUT;

	msleep(TPS65185_TMST_SETTLE_MS);

	ret = regmap_read(tps->regmap, TPS65185_REG_TMST_VALUE, &val);
	if (ret)
		return ret;

	*temp = (int)(s8)val * 1000;
	return 0;
}

static const struct thermal_zone_device_ops tps65185_thermal_ops = {
	.get_temp = tps65185_get_temp,
};

/*
 * The EPD rails come up and go down as one group, so they are exposed as a
 * single regulator rather than one per rail: the panel is only ever driven
 * with all of them present, and the chip sequences them internally from
 * UPSEQ/DWNSEQ.
 */
static const struct reg_sequence tps65185_seq_regs[] = {
	{ TPS65185_REG_VADJ,    TPS65185_VADJ_VALUE },
	{ TPS65185_REG_UPSEQ0,  TPS65185_UPSEQ0_VALUE },
	{ TPS65185_REG_UPSEQ1,  TPS65185_UPSEQ1_VALUE },
	{ TPS65185_REG_DWNSEQ0, TPS65185_DWNSEQ0_VALUE },
	{ TPS65185_REG_DWNSEQ1, TPS65185_DWNSEQ1_VALUE },
};

/* Wait for every rail to report power-good, as the vendor does. */
static int tps65185_wait_power_good(struct tps65185 *tps)
{
	unsigned int val;
	int i, ret;

	for (i = 0; i < TPS65185_PG_POLLS; i++) {
		ret = regmap_read(tps->regmap, TPS65185_REG_PG, &val);
		if (ret)
			return ret;
		if ((val & TPS65185_PG_ALL) == TPS65185_PG_ALL)
			return 0;
		msleep(TPS65185_PG_DELAY_MS);
	}

	return -ETIMEDOUT;
}

static int tps65185_rails_enable(struct regulator_dev *rdev)
{
	struct tps65185 *tps = rdev_get_drvdata(rdev);
	unsigned long ready = tps->off_jiffies + msecs_to_jiffies(tps->dwell_ms);
	int i, ret;

	if (tps->dwell_ms && tps->off_jiffies && time_before(jiffies, ready))
		msleep(jiffies_to_msecs(ready - jiffies));

	ret = regmap_multi_reg_write(tps->regmap, tps65185_seq_regs,
				     ARRAY_SIZE(tps65185_seq_regs));
	if (ret)
		return ret;

	for (i = 0; i < TPS65185_UP_ATTEMPTS; i++) {
		/* V3P3 first on its own, then the high-voltage rails. */
		ret = regmap_write(tps->regmap, TPS65185_REG_ENABLE,
				   TPS65185_ENABLE_V3P3_ONLY);
		if (ret)
			return ret;

		usleep_range(TPS65185_V3P3_SETTLE_US,
			     TPS65185_V3P3_SETTLE_US + 200);

		ret = regmap_write(tps->regmap, TPS65185_REG_ENABLE,
				   TPS65185_ENABLE_UP);
		if (ret)
			return ret;

		ret = tps65185_wait_power_good(tps);
		if (!ret)
			return 0;

		regmap_write(tps->regmap, TPS65185_REG_ENABLE,
			     TPS65185_ENABLE_RETRY);
		msleep(TPS65185_UP_RETRY_MS);
	}

	dev_err(tps->dev, "EPD rails did not reach power-good\n");
	return -ETIMEDOUT;
}

static int tps65185_rails_disable(struct regulator_dev *rdev)
{
	struct tps65185 *tps = rdev_get_drvdata(rdev);
	int ret;

	ret = regmap_write(tps->regmap, TPS65185_REG_ENABLE,
			   TPS65185_ENABLE_DOWN);
	tps->off_jiffies = jiffies;
	if (ret)
		return ret;

	if (tps->dwell_ms)
		msleep(tps->dwell_ms);

	/* Drop V3P3 last, as the vendor does: 0x6f & ~0x20. */
	return regmap_write(tps->regmap, TPS65185_REG_ENABLE,
			    TPS65185_ENABLE_DOWN & ~TPS65185_ENABLE_V3P3_ONLY);
}

static int tps65185_rails_is_enabled(struct regulator_dev *rdev)
{
	struct tps65185 *tps = rdev_get_drvdata(rdev);
	unsigned int val;
	int ret;

	ret = regmap_read(tps->regmap, TPS65185_REG_ENABLE, &val);
	if (ret)
		return ret;

	return !!(val & TPS65185_ENABLE_ACTIVE);
}

static const struct regulator_ops tps65185_rails_ops = {
	.enable		= tps65185_rails_enable,
	.disable	= tps65185_rails_disable,
	.is_enabled	= tps65185_rails_is_enabled,
};

static const struct regulator_desc tps65185_rails_desc = {
	.name		= "epd",
	.of_match	= of_match_ptr("epd"),
	.regulators_node = of_match_ptr("regulators"),
	.id		= 0,
	.ops		= &tps65185_rails_ops,
	.type		= REGULATOR_VOLTAGE,
	.owner		= THIS_MODULE,
	.n_voltages	= 1,
	.fixed_uV	= 15000000,
	.enable_time	= 100000,
};

static int tps65185_probe(struct i2c_client *client)
{
	struct regulator_config config = { };
	struct regulator_dev *rdev;
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
	/* Live on the stock device this line sits high; low gates VCOM off. */
	tps->vcom_ctrl_gpio = devm_gpiod_get_optional(&client->dev, "vcom-ctrl",
						       GPIOD_OUT_HIGH);
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
		unsigned int cur;

		/*
		 * VCOM2:PROG commits to the chip's EEPROM, which has limited
		 * endurance. The stock driver never programs VCOM at all on
		 * this unit -- the value comes from the EEPROM and is only
		 * read back -- so only write when the chip actually disagrees.
		 */
		ret = tps65185_read_vcom(tps, &cur);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "failed to read back VCOM\n");

		if (cur != tps->vcom_mv / TPS65185_VCOM_STEP_MV) {
			dev_info(&client->dev,
				 "VCOM %u mV differs from stored %u mV, programming\n",
				 tps->vcom_mv, cur * TPS65185_VCOM_STEP_MV);
			ret = tps65185_program_vcom(tps);
			if (ret)
				return ret;
		}
	}

	if (IS_ENABLED(CONFIG_THERMAL_OF)) {
		struct thermal_zone_device *tzd;

		tzd = devm_thermal_of_zone_register(&client->dev, 0, tps,
						    &tps65185_thermal_ops);
		if (IS_ERR(tzd) && PTR_ERR(tzd) != -ENODEV)
			return dev_err_probe(&client->dev, PTR_ERR(tzd),
					     "failed to register thermal zone\n");
	}

	tps->dwell_ms = TPS65185_DWELL_DEFAULT_MS;
	device_property_read_u32(&client->dev, "ti,standby-dwell-ms",
				 &tps->dwell_ms);

	config.dev = &client->dev;
	config.driver_data = tps;
	config.regmap = tps->regmap;

	rdev = devm_regulator_register(&client->dev, &tps65185_rails_desc,
				       &config);
	if (IS_ERR(rdev))
		return dev_err_probe(&client->dev, PTR_ERR(rdev),
				     "failed to register EPD rail regulator\n");

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
