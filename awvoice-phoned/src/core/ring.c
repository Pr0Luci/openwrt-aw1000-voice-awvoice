// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * The ring generator. The chip makes the ring voltage, frequency and cadence by itself once the line feed is set to
 * ringing; this file loads its words and switches the state. The hook is watched by the caller (stages), which
 * takes the line back to forward active the moment the handset is lifted.
 */
#include <errno.h>

#include "ring.h"
#include "slic_map.h"

/* the preset's RAM words in the order they are written: address and byte offset in the preset */
static const struct { uint16_t ram; uint8_t off; } ring_word[] = {
	{ RAM_RTPER, 0 },        { RAM_RINGFR, 4 },       { RAM_RINGAMP, 8 },       { RAM_RINGPHAS, 12 },
	{ RAM_RINGOF, 16 },      { RAM_SLOPE_RING, 20 },  { RAM_IRING_LIM, 24 },    { RAM_RTACTH, 28 },
	{ RAM_RTDCTH, 32 },      { RAM_RTACDB, 36 },      { RAM_RTDCDB, 40 },       { RAM_VOV_RING_BAT, 44 },
	{ RAM_VOV_RING_GND, 48 }, { RAM_VBATR_EXPECT, RING_VBATR_OFFSET },
	{ RAM_VBAT_TRACK_MIN_RNG, 96 }, { RAM_VCM_RING, 104 }, { RAM_VCM_RING_FIXED, 108 }, { RAM_DELTA_VCM, 112 },
	{ RAM_VOV_DCDC_SLOPE, 120 }, { RAM_VOV_DCDC_OS, 124 }, { RAM_VOV_RING_BAT_MAX, 128 },
	{ RAM_DCDC_RNGTYPE, 116 },
};

static int put_word(struct chip *c, unsigned int ch, unsigned int ram, uint32_t want, FILE *log)
{
	uint32_t back;
	int err;

	if (want > RAM_VALUE_MAX) {
		fprintf(log, "  channel %u: ring word %u does not fit the chip's 29 bits\n", ch, ram);
		return -ERANGE;
	}
	err = chip_ram_write(c, ch, ram, want);
	if (!err)
		err = chip_ram_read(c, ch, ram, &back);
	if (!err && back != want) {
		fprintf(log, "  channel %u: ring word %u wrote 0x%08x, read 0x%08x\n", ch, ram, want, back);
		err = -EIO;
	}
	return err;
}

static int put_reg(struct chip *c, unsigned int ch, unsigned int reg, uint8_t want, FILE *log)
{
	uint8_t back;
	int err = chip_reg_write(c, ch, reg, want);

	if (!err)
		err = chip_reg_read(c, ch, reg, &back);
	if (!err && back != want) {
		fprintf(log, "  channel %u: ring register %u wrote 0x%02x, read 0x%02x\n", ch, reg, want, back);
		err = -EIO;
	}
	return err;
}

int ring_load(struct chip *c, const struct board *b, unsigned int ch, FILE *log)
{
	const struct board_section *ring = board_find(b, "ring");
	const struct board_section *general = board_find(b, "general");
	const unsigned int base = RING_PRESET_USED * RING_PRESET_SIZE;
	uint32_t on_ticks = RING_ON_MS * 1000u / RING_TIMER_US, off_ticks = RING_OFF_MS * 1000u / RING_TIMER_US;
	uint8_t timers[4] = { on_ticks & 0xff, on_ticks >> 8, off_ticks & 0xff, off_ticks >> 8 };
	unsigned int k;
	int err = 0;

	if (!ring || ring->length != RING_PRESET_COUNT * RING_PRESET_SIZE || !general || general->length != GENERAL_SIZE) {
		fprintf(log, "  board file has no %u-byte ring section or no general section\n", RING_PRESET_COUNT * RING_PRESET_SIZE);
		return -EINVAL;
	}
	for (k = 0; !err && k < sizeof(ring_word) / sizeof(ring_word[0]); k++) {
		uint32_t want = board_word(ring, base + ring_word[k].off);

		/* the battery rail for ringing never exceeds the general configuration's */
		if (ring_word[k].ram == RAM_VBATR_EXPECT && want > board_word(general, GENERAL_VBATR_OFFSET))
			want = board_word(general, GENERAL_VBATR_OFFSET);
		err = put_word(c, ch, ring_word[k].ram, want, log);
	}
	for (k = 0; !err && k < 4; k++)
		err = put_reg(c, ch, REG_RING_TALO + k, timers[k], log);
	if (!err)
		err = put_reg(c, ch, REG_RINGCON, ring->data[base + RING_CON_OFFSET], log);
	if (!err)
		err = put_reg(c, ch, REG_USERSTAT, ring->data[base + RING_USERSTAT_OFFSET], log);
	if (!err)
		fprintf(log, "  channel %u: ring generator loaded (preset %u, %u ms on, %u ms off)\n", ch, RING_PRESET_USED, RING_ON_MS, RING_OFF_MS);
	return err;
}

int ring_set(struct chip *c, unsigned int ch, int on, FILE *log)
{
	uint8_t want = on ? LF_CODE_RING : LF_CODE_FWD_ACTIVE, got;
	int err = chip_reg_write(c, ch, REG_LINEFEED, want);

	if (!err)
		err = chip_reg_read(c, ch, REG_LINEFEED, &got);
	if (!err && (got & LF_STATE_MASK) != want) {
		fprintf(log, "  channel %u: line feed state 0x%02x after the ring %s (wanted 0x%02x)\n", ch, got, on ? "start" : "stop", want);
		err = -EIO;
	}
	return err;
}
