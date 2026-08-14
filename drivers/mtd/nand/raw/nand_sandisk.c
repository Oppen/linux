// SPDX-License-Identifier: GPL-2.0-or-later

#include "internals.h"

static int
sdtnqgama_choose_interface_config(struct nand_chip *chip,
				  struct nand_interface_config *iface)
{
	onfi_fill_interface_config(chip, iface, NAND_SDR_IFACE, 0);

	return nand_choose_best_sdr_timings(chip, iface, NULL);
}

/*
 * SDTNQFAMA answers neither ONFI nor JEDEC, so parameters.model is
 * empty and the hook must key off the raw ID. Same dist3 (LSB_2)
 * pairing as its SDTNQGAMA/SDTNRGAMA siblings; required for
 * SLC-on-MLC emulation (MTD_SLC_ON_MLC_EMULATION is gated on the
 * master having a pairing scheme).
 */
static const u8 sdtnqfama_id[] = { 0x45, 0xd7, 0x84, 0x93, 0x72 };

static int sandisk_nand_init(struct nand_chip *chip)
{
	if (!strncmp("SDTNQGAMA", chip->parameters.model,
		     sizeof("SDTNQGAMA") - 1))
		chip->ops.choose_interface_config =
			&sdtnqgama_choose_interface_config;

	if (chip->id.len >= (int)sizeof(sdtnqfama_id) &&
	    !memcmp(chip->id.data, sdtnqfama_id, sizeof(sdtnqfama_id)))
		mtd_set_pairing_scheme(nand_to_mtd(chip),
				       &dist3_pairing_scheme);

	return 0;
}

const struct nand_manufacturer_ops sandisk_nand_manuf_ops = {
	.init = sandisk_nand_init,
};
