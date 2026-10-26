// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>

#include "slic_map.h"
#include "stages.h"

struct conv {
	struct chip *chip;
	unsigned int ch;
	FILE *log;
	uint32_t word[CW_COUNT];
};

static int32_t sign_extend(uint32_t raw)
{
	return (int32_t)(raw ^ (1u << (VBAT_BITS - 1))) - (int32_t)(1u << (VBAT_BITS - 1));
}

static int ram_get(struct conv *v, unsigned int addr, uint32_t *value)
{
	return chip_ram_read(v->chip, v->ch, addr, value);
}

/* write, then read back: a control word that does not hold means the converter is not under our control */
static int ram_put(struct conv *v, unsigned int addr, uint32_t value)
{
	uint32_t back;
	int err = chip_ram_write(v->chip, v->ch, addr, value);

	if (!err)
		err = ram_get(v, addr, &back);
	if (err)
		return err;
	if (addr == RAM_DCDC_CONTROL)
		back &= ~v->word[CW_CHIP_BITS];
	if (back != (value & ~(addr == RAM_DCDC_CONTROL ? v->word[CW_CHIP_BITS] : 0))) {
		fprintf(v->log, "  channel %u: ram %u wrote 0x%x, read 0x%x\n", v->ch, addr, value, back);
		return -EIO;
	}
	return 0;
}

static int reg_put(struct conv *v, unsigned int reg, uint8_t value)
{
	uint8_t back;
	int err = chip_reg_write(v->chip, v->ch, reg, value);

	if (!err)
		err = chip_reg_read(v->chip, v->ch, reg, &back);
	if (err)
		return err;
	if (back != value) {
		fprintf(v->log, "  channel %u: register %u wrote 0x%02x, read 0x%02x\n", v->ch, reg, value, back);
		return -EIO;
	}
	return 0;
}

static int battery_mv(struct conv *v, unsigned int addr, int32_t *mv)
{
	uint32_t raw;
	int err = ram_get(v, addr, &raw);

	if (!err)
		*mv = sign_extend(raw) / VBAT_PER_MV;
	return err;
}

static int measure(struct conv *v, int32_t *mv)
{
	return battery_mv(v, RAM_VBAT_MEASURE, mv);
}

/* The steps between "prepared" and "tracking"; the caller shuts the converter down if any of them fails. */
static int start_converter(struct conv *v, uint32_t oithresh, uint8_t enhance)
{
	struct chip *c = v->chip;
	int32_t high, now, floor;
	unsigned int polls;
	int err;

	fprintf(v->log, "  channel %u: current-limit word 0x%x\n", v->ch, oithresh);
	err = ram_put(v, RAM_DCDC_OITHRESH, oithresh);
	if (!err)
		err = reg_put(v, REG_LINEFEED, 0);			/* the line stays open: no voltage on tip and ring */
	if (!err)
		err = reg_put(v, REG_ENHANCE, enhance & ENHANCE_KEEP_MASK);
	if (!err)
		err = ram_put(v, RAM_DCDC_CONTROL, v->word[CW_PREPARE]);
	if (!err)
		err = ram_put(v, RAM_DCDC_PUMP, v->word[CW_PUMP_ON]);
	if (err)
		return err;
	c->ops->pause_ms(c->ctx, CONVERTER_SETTLE_MS);

	err = ram_put(v, RAM_DCDC_CONTROL, v->word[CW_START]);
	if (err)
		return err;
	c->ops->pause_ms(c->ctx, CONVERTER_START_MS);
	err = battery_mv(v, RAM_VBAT_HIGH_EXPECT, &high);
	if (!err)
		err = measure(v, &now);
	if (err)
		return err;
	fprintf(v->log, "  channel %u: battery %d mV after start, expected %d mV\n", v->ch, now, high);
	if (now < high / 2) {
		fprintf(v->log, "  channel %u: battery voltage did not rise (below half of the expected value)\n", v->ch);
		return -EIO;
	}

	/* writing zero clears the status word; what it reads back afterwards is the chip's own status, not the value written */
	err = chip_ram_write(c, v->ch, RAM_DCDC_STATUS, 0);
	if (!err)
		err = ram_put(v, RAM_DCDC_CONTROL, v->word[CW_TRACK]);
	if (!err)
		err = reg_put(v, REG_ENHANCE, enhance);
	if (err)
		return err;
	c->ops->pause_ms(c->ctx, CONVERTER_START_MS);

	floor = high - VBAT_MARGIN_MV;
	for (polls = 0; polls < CONVERTER_POLL_MAX; polls++) {
		err = battery_mv(v, RAM_VBAT_HIGH_EXPECT, &high);
		if (!err)
			err = measure(v, &now);
		if (err)
			return err;
		c->ops->pause_ms(c->ctx, CONVERTER_POLL_MS);
		floor = high - VBAT_MARGIN_MV;
		if (now >= floor) {
			fprintf(v->log, "  channel %u: battery %d mV, tracking after %u polls\n", v->ch, now, polls + 1);
			return 0;
		}
	}
	fprintf(v->log, "  channel %u: battery %d mV never reached %d mV\n", v->ch, now, floor);
	return -ETIMEDOUT;
}

/* One calibration round on one channel: start it, then wait for the busy flag to clear. */
static int calibrate(struct conv *v, const uint8_t values[4], unsigned int max_polls, const char *name)
{
	struct chip *c = v->chip;
	unsigned int k, polls;
	uint8_t status;
	int err = 0;

	for (k = 0; !err && k < 4; k++)
		err = chip_reg_write(c, v->ch, REG_CALR0 + k, values[k]);
	for (polls = 0; !err && polls < max_polls; polls++) {
		c->ops->pause_ms(c->ctx, CAL_POLL_MS);
		err = chip_reg_read(c, v->ch, REG_CALR3, &status);
		if (!err && !(status & CAL_BUSY)) {
			fprintf(v->log, "  channel %u: calibration %s done after %u polls\n", v->ch, name, polls + 1);
			return 0;
		}
	}
	if (!err)
		fprintf(v->log, "  channel %u: calibration %s still busy after %u polls\n", v->ch, name, max_polls);
	return err ? err : -ETIMEDOUT;
}

static void shut_down(struct conv *v, uint8_t enhance)
{
	(void)chip_ram_write(v->chip, v->ch, RAM_DCDC_CONTROL, v->word[CW_SHUTDOWN]);
	(void)chip_reg_write(v->chip, v->ch, REG_ENHANCE, enhance);
	fprintf(v->log, "  channel %u: converter shut down\n", v->ch);
}

/* Check the board file's sections and the converter variant, and fill in the command words; nothing is sent to the chip. */
static int load_board(struct conv *v, const struct board *b, const struct board_section **general)
{
	const struct board_section *words = board_find(b, "converter");
	uint32_t kind;
	unsigned int k;

	*general = board_find(b, "general");
	if (!*general || (*general)->length != GENERAL_SIZE) {
		fprintf(v->log, "  board file has no 224-byte general section\n");
		return -EINVAL;
	}
	if (!words || words->length != CONVERTER_SIZE) {
		fprintf(v->log, "  board file has no 28-byte converter section\n");
		return -EINVAL;
	}
	for (k = 0; k < CW_COUNT; k++) {
		v->word[k] = board_word(words, 4 * k);
		if (v->word[k] > RAM_VALUE_MAX) {
			fprintf(v->log, "  converter section word %u does not fit the chip's 29 bits\n", k);
			return -ERANGE;
		}
	}
	if (!v->word[CW_OFF_BIT]) {
		fprintf(v->log, "  converter section has no power-down bit\n");
		return -EINVAL;
	}
	kind = board_word(*general, KIND_OFFSET);
	if (board_word(*general, SCHEME_OFFSET) != SCHEME_TRACKING || kind == KIND_NO_PUMP_A || kind == KIND_NO_PUMP_B) {
		fprintf(v->log, "  this board's converter variant is not implemented (stopped before touching the chip)\n");
		return -ENOTSUP;
	}
	return 0;
}

/* Calibration round A with the converter off, then the start; a failed start powers the converter down again. */
static int power_up(struct conv *v, const struct board_section *general)
{
	static const uint8_t round_a[4] = CAL_ROUND_A;
	uint8_t enhance = ENHANCE_INIT;
	int err = calibrate(v, round_a, CAL_ROUND_A_POLLS, "A");

	if (err) {
		fprintf(v->log, "  FAILED on channel %u: calibration A (%d), the converter was not started\n", v->ch, err);
		return err;
	}
	err = chip_reg_read(v->chip, v->ch, REG_ENHANCE, &enhance);
	if (!err)
		err = start_converter(v, board_word(general, OITHRESH_OFFSET), enhance);
	if (err) {
		fprintf(v->log, "  FAILED on channel %u: converter start (%d)\n", v->ch, err);
		shut_down(v, enhance);
	}
	return err;
}

int stage2_converter(struct chip *c, const struct board *b, unsigned int ch, FILE *log)
{
	const struct board_section *general;
	struct conv v = { .chip = c, .ch = ch, .log = log };
	uint32_t control;
	int err = load_board(&v, b, &general), leave;

	if (err)
		return err;
	err = chip_user_mode(c, ch, 1);
	if (err)
		return fail_stage(log, "enter protected mode", ch, err);
	err = ram_get(&v, RAM_DCDC_CONTROL, &control);
	if (!err)
		fprintf(log, "  channel %u: converter control word at start 0x%x\n", ch, control);
	if (!err && !(control & v.word[CW_OFF_BIT])) {
		int32_t now = 0;

		/* a running converter is left alone: calibration round A must not run on a powered converter */
		err = measure(&v, &now);
		if (!err)
			fprintf(log, "  channel %u: converter already running (battery %d mV), nothing changed\n", ch, now);
	} else if (!err) {
		err = power_up(&v, general);
	}
	leave = chip_user_mode(c, ch, 0);
	return err ? err : leave;
}

int stage3_calibrate(struct chip *c, const struct board *b, unsigned int ch, FILE *log)
{
	static const uint8_t round_b[4] = CAL_ROUND_B;
	const struct board_section *general;
	struct conv v = { .chip = c, .ch = ch, .log = log };
	uint32_t control;
	int err = load_board(&v, b, &general), leave;

	if (err)
		return err;
	err = chip_user_mode(c, ch, 1);
	if (err)
		return fail_stage(log, "enter protected mode", ch, err);
	err = ram_get(&v, RAM_DCDC_CONTROL, &control);
	if (!err && (control & v.word[CW_OFF_BIT])) {
		fprintf(log, "  channel %u: the converter is not running, calibration round B needs it\n", ch);
		err = -EAGAIN;
	} else if (!err) {
		err = calibrate(&v, round_b, CAL_ROUND_B_POLLS, "B");
		if (err)
			fprintf(log, "  FAILED on channel %u: calibration B (%d)\n", ch, err);
		/* final settings from the board's general section */
		if (!err)
			err = reg_put(&v, REG_ENHANCE, general->data[FINAL_ENHANCE_OFFSET]);
		if (!err)
			err = reg_put(&v, REG_AUTO, general->data[FINAL_AUTO_OFFSET]);
		if (!err)
			err = reg_put(&v, REG_ZCAL_EN, general->data[FINAL_ZCAL_OFFSET]);
		if (err && err != -ETIMEDOUT)
			fprintf(log, "  FAILED on channel %u: final register settings (%d)\n", ch, err);
	}
	leave = chip_user_mode(c, ch, 0);
	return err ? err : leave;
}
