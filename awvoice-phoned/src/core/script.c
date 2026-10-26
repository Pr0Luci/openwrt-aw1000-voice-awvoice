// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>

#include "script.h"
#include "slic_map.h"
#include "stages.h"

#define CHANNELS	2
#define RAM_ADDR_MAX	0x7ff

static int record_ok(const uint8_t *r)
{
	unsigned int addr = r[2] | r[3] << 8;
	uint32_t value = board_word_at(r, 4);

	if (r[1] >= CHANNELS)
		return 0;
	switch (r[0]) {
	case 'r':
	case 'R':
		return addr != REG_USER_MODE && addr < 128 && value <= 0xff;
	case 'm':
	case 'M':
		return addr <= RAM_ADDR_MAX && value <= RAM_VALUE_MAX;
	case 'u':
		return value <= 1;
	}
	return 0;
}

static int run_record(struct chip *c, const uint8_t *r, FILE *log, unsigned int n)
{
	unsigned int ch = r[1], addr = r[2] | r[3] << 8;
	uint32_t value = board_word_at(r, 4), got;
	uint8_t byte;
	int err = 0;

	switch (r[0]) {
	case 'r':
		return chip_reg_write(c, ch, addr, (uint8_t)value);
	case 'm':
		return chip_ram_write(c, ch, addr, value);
	case 'u':
		return chip_user_mode(c, ch, (int)value);
	case 'R':
		err = chip_reg_read(c, ch, addr, &byte);
		got = byte;
		break;
	default:
		err = chip_ram_read(c, ch, addr, &got);
		break;
	}
	if (!err && got != value) {
		fprintf(log, "  record %u: read-back differs\n", n);
		return -EIO;
	}
	return err;
}

int script_run(struct chip *c, const struct board_section *script, FILE *log)
{
	unsigned int n, count, ch;
	int err = 0;

	if (script->length % SCRIPT_RECORD_SIZE) {
		fprintf(log, "  script length is not a whole number of records\n");
		return -EINVAL;
	}
	count = script->length / SCRIPT_RECORD_SIZE;
	for (n = 0; n < count; n++) {
		if (!record_ok(script->data + n * SCRIPT_RECORD_SIZE)) {
			fprintf(log, "  record %u is not valid\n", n);
			return -EINVAL;
		}
	}
	for (n = 0; !err && n < count; n++) {
		err = run_record(c, script->data + n * SCRIPT_RECORD_SIZE, log, n);
		if (err)
			fprintf(log, "  FAILED at record %u of %u (%d)\n", n, count, err);
	}
	for (ch = 0; ch < CHANNELS; ch++) {
		int off = chip_user_mode(c, ch, 0);

		err = err ? err : off;
	}
	if (!err)
		fprintf(log, "  %u records done\n", count);
	return err;
}

int stage_script(struct chip *c, const struct board *b, const char *id, FILE *log)
{
	const struct board_section *script = board_find(b, id);

	if (!script) {
		fprintf(log, "  board file has no %s section\n", id);
		return -EINVAL;
	}
	return script_run(c, script, log);
}
