/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#include <errno.h>
#include <string.h>

#include "wideband.h"
#include "slic_map.h"

/* The mode registers (the addresses and the bit meanings are in the private reference collection,
 * REFERENCES.md: the API's register map, corroborated by the reference implementation's notes):
 *   DIGCON 0x2C, bits 2 and 3: the transmit/receive double-rate selects,
 *   ENHANCE 0x2F, bit 0: the wideband enhance select.
 * The chip then carries a second sample per frame (half a frame later) over the same 8 kHz FSYNC, and its
 * audio band widens toward the G.712 wideband band (the Si32260/61 brief p.30).
 *
 * The ACIIR blocks (TXACIIR 518-530, RXACIIR 550-562) are the audio filters of that path: the double-rate
 * mode changes the rate they run at, and their previous design may no longer give the band the standard
 * asks. Deliberately no replacement coefficients are written: the mode is enabled first and the
 * line's own response is measured over the loopback and in calls; a dedicated design (our own)
 * is written only if the measurement asks for it. Until then the pre-change snapshot below is both the
 * disable path and the baseline of the measurement.
 */
#define WB_DIGCON		0x2C
#define WB_ENHANCE		0x2F
#define WB_DIGCON_DOUBLE	0x0C
#define WB_ENHANCE_WIDE		0x01

#define WB_TXACIIR		518
#define WB_RXACIIR		550

int wideband_active(const struct wideband *w)
{
	return w->applied;
}

/* set or clear the masked bits (bits = the wanted value), then read back; 0, or -errno on the first mismatch */
static int put_reg(struct chip *c, unsigned int ch, unsigned int reg, uint8_t mask, uint8_t bits, FILE *log)
{
	uint8_t v;
	int err = chip_reg_read(c, ch, reg, &v);

	if (err)
		return err;
	err = chip_reg_write(c, ch, reg, (uint8_t)((v & ~mask) | bits));
	if (!err)
		err = chip_reg_read(c, ch, reg, &v);
	if (!err && (v & mask) != bits) {
		fprintf(log, "  channel %u: the wideband register 0x%02x did not take (read 0x%02x)\n", ch, reg, v);
		return -EIO;
	}
	return err;
}

static int put_ram(struct chip *c, unsigned int ch, unsigned int addr, uint32_t *save, unsigned int n, FILE *log)
{
	unsigned int k;
	int err = 0;

	for (k = 0; !err && k < n; k++) {
		uint32_t v = 0;

		err = chip_ram_read(c, ch, addr + k, save ? &save[k] : &v);
		if (!err)
			err = chip_ram_write(c, ch, addr + k, save ? save[k] : v);
		if (!err)
			err = chip_ram_read(c, ch, addr + k, &v);
		if (!err && v != (save ? save[k] : v)) {
			fprintf(log, "  channel %u: the wideband RAM %u did not take\n", ch, addr + k);
			err = -EIO;
		}
	}
	return err;
}

static int restore_ram(struct chip *c, unsigned int ch, unsigned int addr, const uint32_t *save, unsigned int n)
{
	unsigned int k;
	int err = 0;

	for (k = 0; !err && k < n; k++) {
		uint32_t v = 0;

		err = chip_ram_write(c, ch, addr + k, save[k]);
		if (!err)
			err = chip_ram_read(c, ch, addr + k, &v);
		if (!err && v != save[k])
			err = -EIO;
	}
	return err;
}

int wideband_apply(struct chip *c, unsigned int ch, struct wideband *w, int on, FILE *log)
{
	int err;

	if (on && w->applied)
		return 0;
	if (!on && !w->applied)
		return 0;
	if (on) {
		/* the snapshot of the line's own narrowband filters: the disable path and the measurement baseline */
		if (!w->snapshotted) {
			err = put_ram(c, ch, WB_TXACIIR, w->narrow_tx, WIDEBAND_NCOEFF, log);
			if (!err)
				err = put_ram(c, ch, WB_RXACIIR, w->narrow_rx, WIDEBAND_NCOEFF, log);
			if (err)
				return err;
			w->snapshotted = 1;
		}
		err = put_reg(c, ch, WB_DIGCON, WB_DIGCON_DOUBLE, WB_DIGCON_DOUBLE, log);
		if (!err)
			err = put_reg(c, ch, WB_ENHANCE, WB_ENHANCE_WIDE, WB_ENHANCE_WIDE, log);
		if (!err) {
			w->applied = 1;
			fprintf(log, "  channel %u: the wideband mode is on: the second sample per frame, the band toward G.712 wideband\n", ch);
		}
		return err;
	}
	err = put_reg(c, ch, WB_DIGCON, WB_DIGCON_DOUBLE, 0, log);
	if (!err)
		err = put_reg(c, ch, WB_ENHANCE, WB_ENHANCE_WIDE, 0, log);
	if (!err && w->snapshotted) {
		err = restore_ram(c, ch, WB_TXACIIR, w->narrow_tx, WIDEBAND_NCOEFF);
		if (!err)
			err = restore_ram(c, ch, WB_RXACIIR, w->narrow_rx, WIDEBAND_NCOEFF);
	}
	if (!err) {
		w->applied = 0;
		fprintf(log, "  channel %u: the wideband mode is off: the line's own narrowband filters are back\n", ch);
	}
	return err;
}
