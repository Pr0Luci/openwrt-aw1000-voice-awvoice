// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>

#include "slic_map.h"
#include "stages.h"

#define CHANNELS	2
#define PROBE_VALUE	0x5a

int fail_stage(FILE *log, const char *what, unsigned int ch, int err)
{
	fprintf(log, "  FAILED on channel %u: %s (%d)\n", ch, what, err);
	return err ? err : -EIO;
}

int stage0_identify(struct chip *c, FILE *log)
{
	unsigned int ch;
	uint8_t id, status, saved, back;
	int err;

	for (ch = 0; ch < CHANNELS; ch++) {
		err = chip_reg_read(c, ch, REG_ID, &id);
		if (err)
			return fail_stage(log, "read id", ch, err);
		fprintf(log, "  channel %u: id 0x%02x\n", ch, id);
		if (id != CHIP_ID_VALUE)
			return fail_stage(log, "unexpected id", ch, -ENODEV);
	}
	err = chip_reg_read(c, 0, REG_MSTRSTAT, &status);
	if (err)
		return fail_stage(log, "read master status", 0, err);
	fprintf(log, "  master status 0x%02x\n", status);
	if (status != MSTRSTAT_RUNNING)
		return fail_stage(log, "the chip is not in its running state: its own firmware did not start - the chip's flash may need the original firmware once to provision it", 0, -ENODEV);

	/* a register with no side effect: write a pattern, read it back, put the old value back */
	for (ch = 0; ch < CHANNELS; ch++) {
		err = chip_reg_read(c, ch, REG_PCM_RX_LO, &saved);
		if (!err)
			err = chip_reg_write(c, ch, REG_PCM_RX_LO, PROBE_VALUE);
		if (!err)
			err = chip_reg_read(c, ch, REG_PCM_RX_LO, &back);
		if (err)
			return fail_stage(log, "probe write", ch, err);
		if (chip_reg_write(c, ch, REG_PCM_RX_LO, saved))
			return fail_stage(log, "probe restore", ch, -EIO);
		if (back != PROBE_VALUE)
			return fail_stage(log, "probe read-back differs", ch, -EIO);
		fprintf(log, "  channel %u: write path ok (probe 0x%02x read back, 0x%02x restored)\n", ch, back, saved);
	}
	return 0;
}

static int load_channel(struct chip *c, unsigned int ch, const struct board_section *general, FILE *log)
{
	unsigned int k, written = 0;
	uint8_t reg;
	int err;

	err = chip_user_mode(c, ch, 1);
	if (err)
		return fail_stage(log, "enter protected mode", ch, err);
	err = chip_reg_write(c, ch, REG_ENHANCE, ENHANCE_INIT);
	if (!err)
		err = chip_reg_write(c, ch, REG_AUTO, AUTO_INIT);
	if (!err)
		err = chip_reg_read(c, ch, REG_AUTO, &reg);
	if (err || reg != AUTO_INIT)
		return fail_stage(log, "fixed registers", ch, err);

	for (k = 0; k < GENERAL_WORDS; k++) {
		uint32_t want = board_word(general, general_words[k].offset), got;

		err = want > RAM_VALUE_MAX ? -ERANGE : chip_ram_write(c, ch, general_words[k].ram, want);
		if (!err)
			err = chip_ram_read(c, ch, general_words[k].ram, &got);
		if (err)
			return fail_stage(log, "ram access", ch, err);
		if (got != want) {
			fprintf(log, "  channel %u: ram %u wrote 0x%08x, read 0x%08x\n", ch, general_words[k].ram, want, got);
			return fail_stage(log, "read-back differs", ch, -EIO);
		}
		written++;
	}
	err = chip_user_mode(c, ch, 0);
	if (err)
		return fail_stage(log, "leave protected mode", ch, err);
	fprintf(log, "  channel %u: %u words written and read back\n", ch, written);
	return 0;
}

int stage1_general(struct chip *c, const struct board *b, FILE *log)
{
	const struct board_section *general = board_find(b, "general");
	unsigned int ch;
	int err;

	if (!general || general->length != GENERAL_SIZE) {
		fprintf(log, "  board file has no 224-byte general section\n");
		return -EINVAL;
	}
	for (ch = 0; ch < CHANNELS; ch++) {
		err = load_channel(c, ch, general, log);
		if (err)
			return err;
	}
	return 0;
}
