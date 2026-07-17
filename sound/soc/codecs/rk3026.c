// SPDX-License-Identifier: GPL-2.0-only
/*
 * Rockchip RK3026 internal audio codec driver
 *
 * The RK3026 integrates a stereo audio codec (DAC + ADC + HP amplifier)
 * connected via MMIO.  Physical base address is 0x20030000, size 0x4000.
 *
 * Copyright (c) 2024, Mario Rugiero
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

#include "rk3026.h"

struct rk3026_priv {
	struct regmap	*regmap;
	struct device	*dev;
};

/* -39 dB .. +6 dB in 1.5 dB steps (32 steps, 0x00–0x1f) */
static const DECLARE_TLV_DB_SCALE(rk3026_hp_tlv, -3900, 150, 0);

/* ------------------------------------------------------------------ */
/* Mixer controls                                                       */
/* ------------------------------------------------------------------ */

static const struct snd_kcontrol_new rk3026_controls[] = {
	SOC_SINGLE_RANGE_TLV("Headphone Left Volume",
			     RK3026_HPOUTL_GAIN, 0,
			     RK3026_HP_GAIN_MIN, RK3026_HP_GAIN_MSK,
			     0, rk3026_hp_tlv),
	SOC_SINGLE_RANGE_TLV("Headphone Right Volume",
			     RK3026_HPOUTR_GAIN, 0,
			     RK3026_HP_GAIN_MIN, RK3026_HP_GAIN_MSK,
			     0, rk3026_hp_tlv),
};

/* ------------------------------------------------------------------ */
/* DAPM mixer source controls                                          */
/* ------------------------------------------------------------------ */

static const struct snd_kcontrol_new rk3026_hpmixl_controls[] = {
	SOC_DAPM_SINGLE("DAC L Switch", RK3026_HPMIX_S_SELECT,
			3, 1, 0),
};

static const struct snd_kcontrol_new rk3026_hpmixr_controls[] = {
	SOC_DAPM_SINGLE("DAC R Switch", RK3026_HPMIX_S_SELECT,
			0, 1, 0),
};

/* ------------------------------------------------------------------ */
/* DAPM widgets                                                         */
/* ------------------------------------------------------------------ */

static const struct snd_soc_dapm_widget rk3026_dapm_widgets[] = {
	/* Digital inputs */
	SND_SOC_DAPM_AIF_IN("AIF Playback", "HiFi Playback", 0,
			    SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("AIF Capture", "HiFi Capture", 0,
			     SND_SOC_NOPM, 0, 0),

	/* DACs — DACL_EN = BIT(7), DACR_EN = BIT(3) */
	SND_SOC_DAPM_DAC("DAC L", NULL, RK3026_DAC_ENABLE, 7, 0),
	SND_SOC_DAPM_DAC("DAC R", NULL, RK3026_DAC_ENABLE, 3, 0),

	/* ADCs — ADCL_EN = BIT(7), ADCR_EN = BIT(3) */
	SND_SOC_DAPM_ADC("ADC L", NULL, RK3026_ADC_ENABLE, 7, 0),
	SND_SOC_DAPM_ADC("ADC R", NULL, RK3026_ADC_ENABLE, 3, 0),

	/* HP mixers — HPMIXL_EN = BIT(7), HPMIXR_EN = BIT(3) */
	SND_SOC_DAPM_MIXER("HP Mixer L", RK3026_HPMIX_CTL, 7, 0,
			   rk3026_hpmixl_controls,
			   ARRAY_SIZE(rk3026_hpmixl_controls)),
	SND_SOC_DAPM_MIXER("HP Mixer R", RK3026_HPMIX_CTL, 3, 0,
			   rk3026_hpmixr_controls,
			   ARRAY_SIZE(rk3026_hpmixr_controls)),

	/* HP output amplifiers — HPOUTL_EN = BIT(5), HPOUTR_EN = BIT(3) */
	SND_SOC_DAPM_PGA("HP Out L", RK3026_HPOUT_CTL, 5, 0, NULL, 0),
	SND_SOC_DAPM_PGA("HP Out R", RK3026_HPOUT_CTL, 3, 0, NULL, 0),

	/* Physical ports */
	SND_SOC_DAPM_INPUT("MIC Jack"),
	SND_SOC_DAPM_OUTPUT("Headphone"),
};

/* ------------------------------------------------------------------ */
/* DAPM routes                                                          */
/* ------------------------------------------------------------------ */

static const struct snd_soc_dapm_route rk3026_dapm_routes[] = {
	/* Playback path */
	{ "DAC L",       NULL,          "AIF Playback" },
	{ "DAC R",       NULL,          "AIF Playback" },

	{ "HP Mixer L",  "DAC L Switch", "DAC L" },
	{ "HP Mixer R",  "DAC R Switch", "DAC R" },

	{ "HP Out L",    NULL,           "HP Mixer L" },
	{ "HP Out R",    NULL,           "HP Mixer R" },

	{ "Headphone",   NULL,           "HP Out L" },
	{ "Headphone",   NULL,           "HP Out R" },

	/* Capture path */
	{ "ADC L",       NULL,           "MIC Jack" },
	{ "ADC R",       NULL,           "MIC Jack" },

	{ "AIF Capture", NULL,           "ADC L" },
	{ "AIF Capture", NULL,           "ADC R" },
};

/* ------------------------------------------------------------------ */
/* DAI operations                                                       */
/* ------------------------------------------------------------------ */

static int rk3026_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;

	/* Clock provider role — codec is always slave on this SoC */
	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_CBC_CFC:
		/* bit clock + frame clock consumer (slave) — expected */
		snd_soc_component_update_bits(component,
					      RK3026_ADC_INT_CTL2,
					      RK3026_I2S_MODE_MSK,
					      RK3026_I2S_MODE_SLV);
		break;
	case SND_SOC_DAIFMT_CBP_CFP:
		/* master mode is possible but unusual */
		snd_soc_component_update_bits(component,
					      RK3026_ADC_INT_CTL2,
					      RK3026_I2S_MODE_MSK,
					      RK3026_I2S_MODE_MST);
		break;
	default:
		dev_err(dai->dev, "unsupported clock provider mode\n");
		return -EINVAL;
	}

	/* Data format */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		snd_soc_component_update_bits(component,
					      RK3026_DAC_INT_CTL1,
					      RK3026_DAC_DF_MSK,
					      RK3026_DAC_DF_I2S);
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		snd_soc_component_update_bits(component,
					      RK3026_DAC_INT_CTL1,
					      RK3026_DAC_DF_MSK,
					      RK3026_DAC_DF_LJ);
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		snd_soc_component_update_bits(component,
					      RK3026_DAC_INT_CTL1,
					      RK3026_DAC_DF_MSK,
					      RK3026_DAC_DF_RJ);
		break;
	case SND_SOC_DAIFMT_DSP_A:
		snd_soc_component_update_bits(component,
					      RK3026_DAC_INT_CTL1,
					      RK3026_DAC_DF_MSK,
					      RK3026_DAC_DF_PCM);
		break;
	default:
		dev_err(dai->dev, "unsupported DAI format\n");
		return -EINVAL;
	}

	/* LR clock polarity */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		snd_soc_component_update_bits(component,
					      RK3026_DAC_INT_CTL1,
					      RK3026_DLRCK_POL_MSK,
					      RK3026_DLRCK_POL_NORM);
		break;
	case SND_SOC_DAIFMT_NB_IF:
	case SND_SOC_DAIFMT_IB_IF:
		snd_soc_component_update_bits(component,
					      RK3026_DAC_INT_CTL1,
					      RK3026_DLRCK_POL_MSK,
					      RK3026_DLRCK_POL_INV);
		break;
	default:
		dev_err(dai->dev, "unsupported polarity\n");
		return -EINVAL;
	}

	return 0;
}

static int rk3026_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	unsigned int adc_wl, dac_vwl;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		adc_wl  = RK3026_ADC_WL_16;
		dac_vwl = RK3026_DAC_VWL_16;
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		adc_wl  = RK3026_ADC_WL_20;
		dac_vwl = RK3026_DAC_VWL_20;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		adc_wl  = RK3026_ADC_WL_24;
		dac_vwl = RK3026_DAC_VWL_24;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		adc_wl  = RK3026_ADC_WL_32;
		dac_vwl = RK3026_DAC_VWL_32;
		break;
	default:
		dev_err(dai->dev, "unsupported PCM format\n");
		return -EINVAL;
	}

	snd_soc_component_update_bits(component, RK3026_ADC_INT_CTL2,
				      RK3026_ADC_WL_MSK, adc_wl);
	snd_soc_component_update_bits(component, RK3026_DAC_INT_CTL1,
				      RK3026_DAC_VWL_MSK, dac_vwl);

	return 0;
}

static const struct snd_soc_dai_ops rk3026_dai_ops = {
	.set_fmt	= rk3026_set_fmt,
	.hw_params	= rk3026_hw_params,
};

#define RK3026_RATES (SNDRV_PCM_RATE_8000  | \
		      SNDRV_PCM_RATE_16000 | \
		      SNDRV_PCM_RATE_32000 | \
		      SNDRV_PCM_RATE_44100 | \
		      SNDRV_PCM_RATE_48000)

#define RK3026_FMTS (SNDRV_PCM_FMTBIT_S16_LE  | \
		     SNDRV_PCM_FMTBIT_S20_3LE | \
		     SNDRV_PCM_FMTBIT_S24_LE  | \
		     SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver rk3026_dai_driver = {
	.name = "rk3026-hifi",
	.playback = {
		.stream_name  = "HiFi Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates        = RK3026_RATES,
		.formats      = RK3026_FMTS,
	},
	.capture = {
		.stream_name  = "HiFi Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates        = RK3026_RATES,
		.formats      = RK3026_FMTS,
	},
	.ops            = &rk3026_dai_ops,
	.symmetric_rate = 1,
};

/* ------------------------------------------------------------------ */
/* Component probe / bias                                               */
/* ------------------------------------------------------------------ */

static void rk3026_codec_reset(struct snd_soc_component *component)
{
	snd_soc_component_write(component, RK3026_RESET, RK3026_RESET_VAL);
	snd_soc_component_write(component, RK3026_RESET, RK3026_NORMAL_VAL);
}

/*
 * Write the defaults that vendor code programs after reset.  Only the
 * registers that deviate from hardware power-on reset values are listed.
 */
static void rk3026_codec_set_defaults(struct snd_soc_component *component)
{
	snd_soc_component_write(component, RK3026_ADC_INT_CTL1,  0x50);
	snd_soc_component_write(component, RK3026_ADC_INT_CTL2,  0x0e);
	snd_soc_component_write(component, RK3026_DAC_INT_CTL1,  0x50);
	snd_soc_component_write(component, RK3026_DAC_INT_CTL2,  0x0e);
	snd_soc_component_write(component, RK3026_DAC_INT_CTL3,  0x22);
	snd_soc_component_write(component, RK3026_SELECT_CURRENT, 0x1e);
}

static int rk3026_probe(struct snd_soc_component *component)
{
	rk3026_codec_reset(component);
	rk3026_codec_set_defaults(component);
	return 0;
}

static void rk3026_remove(struct snd_soc_component *component)
{
	/* Power everything down cleanly */
	snd_soc_component_write(component, RK3026_HPOUT_CTL,   0x00);
	snd_soc_component_write(component, RK3026_HPMIX_CTL,   0x00);
	snd_soc_component_write(component, RK3026_DAC_ENABLE,  0x00);
	snd_soc_component_write(component, RK3026_ADC_ENABLE,  0x00);
	rk3026_codec_reset(component);
}

static int rk3026_set_bias_level(struct snd_soc_component *component,
				 enum snd_soc_bias_level level)
{
	switch (level) {
	case SND_SOC_BIAS_ON:
		/*
		 * Full power: enable DACs, HP mixer and HP output, unmute.
		 * The WORK bits must be set after the EN bits so the
		 * internal circuits latch properly.
		 */
		snd_soc_component_write(component, RK3026_DAC_ENABLE,
					RK3026_DACL_EN | RK3026_DACL_WORK |
					RK3026_DACR_EN | RK3026_DACR_WORK);

		snd_soc_component_write(component, RK3026_HPMIX_CTL,
					RK3026_HPMIXL_EN  | RK3026_HPMIXL_WORK1 |
					RK3026_HPMIXL_WORK2 |
					RK3026_HPMIXR_EN  | RK3026_HPMIXR_WORK1 |
					RK3026_HPMIXR_WORK2);

		snd_soc_component_write(component, RK3026_HPOUT_CTL,
					RK3026_HPVREF_EN   | RK3026_HPVREF_WORK  |
					RK3026_HPOUTL_EN   | RK3026_HPOUTL_WORK  |
					RK3026_HPOUTR_EN   | RK3026_HPOUTR_WORK  |
					RK3026_HPOUTL_UNMUTE | RK3026_HPOUTR_UNMUTE);
		break;

	case SND_SOC_BIAS_PREPARE:
		/* Nothing required; DAPM handles individual widget power. */
		break;

	case SND_SOC_BIAS_STANDBY:
		/* Mute HP outputs but keep DAC powered. */
		snd_soc_component_update_bits(component, RK3026_HPOUT_CTL,
					      RK3026_HPOUTL_UNMUTE |
					      RK3026_HPOUTR_UNMUTE,
					      0);
		snd_soc_component_write(component, RK3026_HPMIX_CTL, 0x00);
		snd_soc_component_update_bits(component, RK3026_DAC_ENABLE,
					      RK3026_DACL_WORK | RK3026_DACR_WORK,
					      0);
		break;

	case SND_SOC_BIAS_OFF:
		/* Full power down then reset. */
		snd_soc_component_write(component, RK3026_HPOUT_CTL,  0x00);
		snd_soc_component_write(component, RK3026_HPMIX_CTL,  0x00);
		snd_soc_component_write(component, RK3026_DAC_ENABLE, 0x00);
		snd_soc_component_write(component, RK3026_ADC_ENABLE, 0x00);
		rk3026_codec_reset(component);
		break;
	}

	return 0;
}

static const struct snd_soc_component_driver rk3026_soc_component_driver = {
	.probe			= rk3026_probe,
	.remove			= rk3026_remove,
	.set_bias_level		= rk3026_set_bias_level,
	.controls		= rk3026_controls,
	.num_controls		= ARRAY_SIZE(rk3026_controls),
	.dapm_widgets		= rk3026_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(rk3026_dapm_widgets),
	.dapm_routes		= rk3026_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(rk3026_dapm_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

/* ------------------------------------------------------------------ */
/* regmap                                                               */
/* ------------------------------------------------------------------ */

static bool rk3026_volatile_reg(struct device *dev, unsigned int reg)
{
	return reg == RK3026_RESET;
}

static bool rk3026_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case RK3026_RESET:
	case RK3026_ADC_INT_CTL1 ... RK3026_DAC_INT_CTL3:
	case RK3026_ADC_MIC_CTL  ... RK3026_SELECT_CURRENT:
	case RK3026_PGAL_AGC_CTL1 ... RK3026_PGAL_AGC_CTL5:
	case RK3026_PGAR_AGC_CTL1 ... RK3026_PGAR_AGC_CTL5:
		return true;
	default:
		return false;
	}
}

static const struct reg_default rk3026_reg_defaults[] = {
	{ RK3026_RESET,         0x0003 },
	{ RK3026_ADC_INT_CTL1,  0x0050 },
	{ RK3026_ADC_INT_CTL2,  0x000e },
	{ RK3026_DAC_INT_CTL1,  0x0050 },
	{ RK3026_DAC_INT_CTL2,  0x000e },
	{ RK3026_DAC_INT_CTL3,  0x0022 },
	{ RK3026_HPMIX_CTL,     0x0000 },
	{ RK3026_HPMIX_S_SELECT, 0x0000 },
	{ RK3026_HPOUT_CTL,     0x0000 },
	{ RK3026_HPOUTL_GAIN,   0x0000 },
	{ RK3026_HPOUTR_GAIN,   0x0000 },
	{ RK3026_SELECT_CURRENT, 0x001e },
};

static const struct regmap_config rk3026_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= RK3026_MAX_REGISTER,
	.volatile_reg	= rk3026_volatile_reg,
	.readable_reg	= rk3026_readable_reg,
	.reg_defaults	= rk3026_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(rk3026_reg_defaults),
	.cache_type	= REGCACHE_FLAT,
};

/* ------------------------------------------------------------------ */
/* Platform driver                                                      */
/* ------------------------------------------------------------------ */

static int rk3026_platform_probe(struct platform_device *pdev)
{
	struct rk3026_priv *priv;
	void __iomem *base;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	priv->regmap = devm_regmap_init_mmio(&pdev->dev, base,
					     &rk3026_regmap_config);
	if (IS_ERR(priv->regmap)) {
		dev_err(&pdev->dev, "failed to init regmap: %ld\n",
			PTR_ERR(priv->regmap));
		return PTR_ERR(priv->regmap);
	}

	priv->dev = &pdev->dev;
	dev_set_drvdata(&pdev->dev, priv);

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &rk3026_soc_component_driver,
					      &rk3026_dai_driver, 1);
	if (ret) {
		dev_err(&pdev->dev, "failed to register codec component: %d\n",
			ret);
		return ret;
	}

	return 0;
}

static const struct of_device_id rk3026_codec_of_match[] = {
	{ .compatible = "rockchip,rk3026-codec" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rk3026_codec_of_match);

static struct platform_driver rk3026_codec_driver = {
	.driver = {
		.name		= "rk3026-codec",
		.of_match_table	= rk3026_codec_of_match,
	},
	.probe	= rk3026_platform_probe,
};
module_platform_driver(rk3026_codec_driver);

MODULE_AUTHOR("Mario Rugiero");
MODULE_DESCRIPTION("Rockchip RK3026 internal audio codec driver");
MODULE_LICENSE("GPL");
