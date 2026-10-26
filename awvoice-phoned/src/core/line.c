// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The line itself: the dc feed, the calibrations, the impedance and tone presets, the oscillators, the feed and release of
 * the line, the voice path and the tone detector's enables. Chip access only; no call logic. */
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#include "line.h"
#include "slic_map.h"
#include "stages.h"

#define CHANNELS	2

int32_t line_battery_mv(uint32_t raw)
{
	return (int32_t)(raw ^ (1u << (VBAT_BITS - 1))) - (int32_t)(1u << (VBAT_BITS - 1));
}

/* Read the battery voltage (now) and the converter's expected high voltage (both in millivolts). */
int read_battery(struct chip *c, unsigned int ch, int32_t *now, int32_t *expected)
{
	uint32_t raw, high;
	int err = chip_ram_read(c, ch, RAM_VBAT_HIGH_EXPECT, &high);

	if (!err)
		err = chip_ram_read(c, ch, RAM_VBAT_MEASURE, &raw);
	if (!err) {
		*expected = line_battery_mv(high) / VBAT_PER_MV;
		*now = line_battery_mv(raw) / VBAT_PER_MV;
	}
	return err;
}

int read_iloop(struct chip *c, unsigned int ch, int32_t *ua)
{
	uint32_t raw;
	int err = chip_ram_read(c, ch, RAM_MADC_ILOOP, &raw);

	*ua = err ? 0 : line_battery_mv(raw) / ILOOP_PER_UA;
	return err;
}

/* One channel's dc feed preset: preset word dc_feed_word[k].word goes to RAM location dc_feed_word[k].ram. */
static int load_dcfeed(struct chip *c, unsigned int ch, const struct board_section *feed, FILE *log)
{
	uint8_t lf;
	uint32_t want, back;
	unsigned int k, written = 0;
	int err = chip_user_mode(c, ch, 1), leave;

	if (err)
		return fail_stage(log, "enter protected mode", ch, err);
	err = chip_reg_read(c, ch, REG_LINEFEED, &lf);
	if (!err && (lf & LF_STATE_MASK) != LF_CODE_OPEN) {
		fprintf(log, "  channel %u: line feed state 0x%02x is not open\n", ch, lf);
		err = -EBUSY;
	}
	for (k = 0; !err && k < DC_FEED_WORDS; k++) {
		want = board_word(feed, DCFEED_PRESET_OFFSET + 4 * dc_feed_word[k].word);
		if (want > RAM_VALUE_MAX) {
			fprintf(log, "  channel %u: dc feed word %u does not fit the chip's 29 bits\n", ch, k);
			err = -ERANGE;
		} else {
			err = chip_ram_write(c, ch, dc_feed_word[k].ram, want);
			if (!err)
				err = chip_ram_read(c, ch, dc_feed_word[k].ram, &back);
			if (!err && back != want) {
				fprintf(log, "  channel %u: dc feed word %u wrote 0x%08x, read 0x%08x\n", ch, k, want, back);
				err = -EIO;
			}
			if (!err)
				written++;
		}
	}
	leave = chip_user_mode(c, ch, 0);
	if (err)
		return err;
	if (leave)
		return fail_stage(log, "leave protected mode", ch, leave);
	fprintf(log, "  channel %u: %u dc feed words written and read back\n", ch, written);
	return 0;
}

int stage4_dcfeed(struct chip *c, const struct board *b, FILE *log)
{
	const struct board_section *feed = board_find(b, "dcfeed");
	unsigned int ch;
	int err;

	if (!feed || feed->length < DCFEED_PRESET_OFFSET + DCFEED_PRESET_SIZE) {
		fprintf(log, "  board file has no %u-byte dc feed preset\n", DCFEED_PRESET_SIZE);
		return -EINVAL;
	}
	for (ch = 0; ch < CHANNELS; ch++) {
		err = load_dcfeed(c, ch, feed, log);
		if (err)
			return err;
	}
	return 0;
}

/*
 * Longitudinal balance calibration, the chip maker's official canned procedure: the line goes to forward active,
 * all four LB rounds start, and the busy flag is polled every 10 ms; the caller's line-feed restore follows. On the
 * timeout the line is put back open first. The caller holds protected mode.
 */
static int lb_cal(struct chip *c, unsigned int ch, FILE *log)
{
	uint8_t got, lf;
	unsigned int polls;
	int err;

	err = chip_reg_write(c, ch, REG_LINEFEED, LF_CODE_FWD_ACTIVE);
	if (!err)
		err = chip_reg_read(c, ch, REG_LINEFEED, &lf);
	if (!err && (lf & LF_STATE_MASK) != LF_CODE_FWD_ACTIVE) {
		fprintf(log, "  channel %u: line feed state 0x%02x is not forward active before the LB calibration\n", ch, lf);
		err = -EIO;
	}
	if (!err)
		err = chip_reg_write(c, ch, REG_CALR0, CAL_LB_ALL);
	if (!err)
		err = chip_reg_write(c, ch, REG_CALR3, CAL_BUSY);
	for (polls = 0; !err && polls < CAL_LB_POLLS; polls++) {
		c->ops->pause_ms(c->ctx, CAL_LB_POLL_MS);
		err = chip_reg_read(c, ch, REG_CALR3, &got);
		if (!err && !(got & CAL_BUSY)) {
			fprintf(log, "  channel %u: LB calibration done after %u polls\n", ch, polls + 1);
			break;
		}
	}
	if (!err && polls == CAL_LB_POLLS) {
		fprintf(log, "  channel %u: LB calibration still busy after %u polls\n", ch, CAL_LB_POLLS);
		(void)chip_reg_write(c, ch, REG_LINEFEED, LF_CODE_OPEN);
		err = -ETIMEDOUT;
	}
	return err;
}

/*
 * One channel's impedance preset: the audio equalizers, the hybrid echo cancellor, the impedance synthesis and both
 * gain words, plus the RA register and the receive high-pass filter. The line goes open for the write (both sources'
 * practice) and the quick Z calibration runs before the line comes back; the caller holds protected mode.
 */
static int load_imped(struct chip *c, unsigned int ch, const struct board_section *imped, FILE *log)
{
	static const uint8_t round_z[4] = CAL_ROUND_Z;
	uint32_t want, back;
	uint8_t lf, byte, got, status;
	unsigned int k, polls;
	int err;

	err = chip_reg_read(c, ch, REG_LINEFEED, &lf);
	if (!err)
		err = chip_reg_write(c, ch, REG_LINEFEED, LF_CODE_OPEN);
	for (k = 0; !err && k < IMPED_RAM_WORDS; k++) {
		want = board_word(imped, IMPED_PRESET_OFFSET + imped_ram_word[k].off);
		if (want > RAM_VALUE_MAX) {
			fprintf(log, "  channel %u: impedance word %u does not fit the chip's 29 bits\n", ch, k);
			err = -ERANGE;
		} else {
			err = chip_ram_write(c, ch, imped_ram_word[k].ram, want);
			if (!err)
				err = chip_ram_read(c, ch, imped_ram_word[k].ram, &back);
			if (!err && back != want) {
				fprintf(log, "  channel %u: impedance word %u wrote 0x%08x, read 0x%08x\n", ch, k, want, back);
				err = -EIO;
			}
		}
	}
	if (!err) {
		byte = imped->data[IMPED_PRESET_OFFSET + IMPED_RA_OFFSET];
		err = chip_reg_write(c, ch, REG_RA, byte);
		if (!err)
			err = chip_reg_read(c, ch, REG_RA, &got);
		if (!err && got != byte) {
			fprintf(log, "  channel %u: impedance RA register wrote 0x%02x, read 0x%02x\n", ch, byte, got);
			err = -EIO;
		}
	}
	for (k = 0; !err && k < 4; k++)
		err = chip_reg_write(c, ch, REG_CALR0 + k, round_z[k]);
	for (polls = 0; !err && polls < CAL_Z_POLLS; polls++) {
		c->ops->pause_ms(c->ctx, CAL_Z_POLL_MS);
		err = chip_reg_read(c, ch, REG_CALR3, &status);
		if (!err && !(status & CAL_BUSY)) {
			fprintf(log, "  channel %u: z calibration done after %u polls\n", ch, polls + 1);
			break;
		}
	}
	if (!err && polls == CAL_Z_POLLS) {
		fprintf(log, "  channel %u: z calibration still busy after %u polls\n", ch, CAL_Z_POLLS);
		err = -ETIMEDOUT;
	}
	if (!err)
		err = lb_cal(c, ch, log);
	if (!err)
		err = chip_reg_write(c, ch, REG_LINEFEED, lf);
	if (!err)
		err = chip_reg_read(c, ch, REG_LINEFEED, &got);
	if (!err && (got & LF_STATE_MASK) != (lf & LF_STATE_MASK)) {
		fprintf(log, "  channel %u: line feed state 0x%02x after the impedance preset (wanted 0x%02x)\n", ch, got, lf);
		err = -EIO;
	}
	return err;
}

/*
 * One channel's tone preset from the board file: the oscillators' words, their timer bytes and the mode byte. The
 * caller holds protected mode.
 */
static int load_tone(struct chip *c, unsigned int ch, const struct board_section *tones, FILE *log)
{
	static const struct { uint16_t ram; unsigned int off; } osc_words[6] = {
		{ RAM_OSC1_FREQ, 0 },  { RAM_OSC1_AMP, 4 },  { RAM_OSC1_PHAS, 8 },
		{ RAM_OSC2_FREQ, 16 }, { RAM_OSC2_AMP, 20 }, { RAM_OSC2_PHAS, 24 },
	};
	/* each oscillator's timer bytes go to its four registers (50..53 and 54..57) in the preset's own order */
	static const struct { uint8_t reg; unsigned int off; } osc_timers[8] = {
		{ REG_O1_TALO, 12 },     { REG_O1_TALO + 1, 13 }, { REG_O1_TALO + 2, 14 }, { REG_O1_TALO + 3, 15 },
		{ REG_O2_TALO, 28 },     { REG_O2_TALO + 1, 29 }, { REG_O2_TALO + 2, 30 }, { REG_O2_TALO + 3, 31 },
	};
	uint32_t want, back;
	uint8_t byte, got;
	unsigned int k;
	int err = 0;

	for (k = 0; !err && k < 6; k++) {
		want = board_word(tones, TONE_PRESET_OFFSET + osc_words[k].off);
		if (want > RAM_VALUE_MAX) {
			fprintf(log, "  channel %u: tone word %u does not fit the chip's 29 bits\n", ch, k);
			err = -ERANGE;
		} else {
			err = chip_ram_write(c, ch, osc_words[k].ram, want);
			if (!err)
				err = chip_ram_read(c, ch, osc_words[k].ram, &back);
			if (!err && back != want) {
				fprintf(log, "  channel %u: tone word %u wrote 0x%08x, read 0x%08x\n", ch, k, want, back);
				err = -EIO;
			}
		}
	}
	for (k = 0; !err && k < 8; k++) {
		byte = tones->data[TONE_PRESET_OFFSET + osc_timers[k].off];
		err = chip_reg_write(c, ch, osc_timers[k].reg, byte);
		if (!err)
			err = chip_reg_read(c, ch, osc_timers[k].reg, &got);
		if (!err && got != byte) {
			fprintf(log, "  channel %u: tone timer %u wrote 0x%02x, read 0x%02x\n", ch, k, byte, got);
			err = -EIO;
		}
	}
	if (!err) {
		byte = tones->data[TONE_PRESET_OFFSET + 32];
		err = chip_reg_write(c, ch, REG_OMODE, byte);
		if (!err)
			err = chip_reg_read(c, ch, REG_OMODE, &got);
		if (!err && got != byte) {
			fprintf(log, "  channel %u: oscillator mode wrote 0x%02x, read 0x%02x\n", ch, byte, got);
			err = -EIO;
		}
	}
	return err;
}

/* Start the oscillators once and verify (no write when they already run). The caller holds protected mode. */
int osc_start_once(struct chip *c, unsigned int ch, FILE *log)
{
	uint8_t ocon, got;
	int err = chip_reg_read(c, ch, REG_OCON, &ocon);

	if (!err && !(ocon & OCON_ENABLE_SET)) {
		err = chip_reg_write(c, ch, REG_OCON, (uint8_t)(ocon | OCON_ENABLE_SET));
		if (!err)
			err = chip_reg_read(c, ch, REG_OCON, &got);
		if (!err && (got & OCON_ENABLE_SET) != OCON_ENABLE_SET) {
			fprintf(log, "  channel %u: oscillator control 0x%02x after the write (bit 0x%02x wanted)\n", ch, got, OCON_ENABLE_SET);
			err = -EIO;
		}
	}
	return err;
}

/* Stop the oscillators once and verify (no write when they are already stopped). */
int osc_stop_once(struct chip *c, unsigned int ch, FILE *log)
{
	uint8_t ocon, got;
	int err = chip_reg_read(c, ch, REG_OCON, &ocon);

	if (!err && (ocon & OCON_ENABLE_CLEAR)) {
		err = chip_reg_write(c, ch, REG_OCON, (uint8_t)(ocon & ~OCON_ENABLE_CLEAR));
		if (!err)
			err = chip_reg_read(c, ch, REG_OCON, &got);
		if (!err && (got & OCON_ENABLE_SET)) {
			fprintf(log, "  channel %u: oscillator control 0x%02x is still enabled after the stop\n", ch, got);
			err = -EIO;
		}
	}
	return err;
}

static long long hold_stamp_base;	/* the transitions are stamped from the first one the tool prints */

/* the hold's decision lines carry their monotonic stamp, so a session's timing is read from the log alone */
void stamped(FILE *log, const char *fmt, ...)
{
	struct timespec ts;
	long long now, s, ms;
	va_list ap;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (long long)ts.tv_sec * 1000000ll + ts.tv_nsec / 1000;
	if (!hold_stamp_base)
		hold_stamp_base = now;
	now -= hold_stamp_base;
	s = now / 1000000ll;
	ms = (now % 1000000ll) / 1000ll;
	fprintf(log, "  [+%lld.%03lld]", s, ms);
	va_start(ap, fmt);
	vfprintf(log, fmt, ap);
	va_end(ap);
}

unsigned long long clock_ms(struct chip *c)
{
	struct timespec ts;

	if (c->ops->now_ms)
		return c->ops->now_ms(c->ctx);
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000u + (unsigned long long)ts.tv_nsec / 1000000u;
}

/* the time from `since` to `now`; a time taken a moment after the step began is not before it (never a negative time) */
unsigned long long elapsed_ms(unsigned long long now, unsigned long long since)
{
	return now > since ? now - since : 0;
}

/* Take the line from open to forward active; the caller holds protected mode. 0 or -EAGAIN/-EBUSY/-EIO. */
int fed_line(struct chip *c, unsigned int ch, uint32_t off_bit, FILE *log)
{
	uint32_t control;
	int32_t now, expected, ua = 0;
	uint8_t lf;
	int err = chip_ram_read(c, ch, RAM_DCDC_CONTROL, &control);

	if (!err && (control & off_bit)) {
		fprintf(log, "  channel %u: the converter is not running, the line feed needs it\n", ch);
		return -EAGAIN;
	}
	err = read_battery(c, ch, &now, &expected);
	if (!err) {
		fprintf(log, "  channel %u: battery %d mV before the line feed, expected %d mV\n", ch, now, expected);
		err = chip_reg_read(c, ch, REG_LINEFEED, &lf);
	}
	if (!err && (lf & LF_STATE_MASK) != LF_CODE_OPEN) {
		fprintf(log, "  channel %u: line feed state 0x%02x is not open\n", ch, lf);
		return -EBUSY;
	}
	if (!err) {
		err = chip_reg_write(c, ch, REG_LINEFEED, LF_CODE_FWD_ACTIVE);
		if (!err)
			err = chip_reg_read(c, ch, REG_LINEFEED, &lf);
		if (!err && (lf & LF_STATE_MASK) != LF_CODE_FWD_ACTIVE) {
			fprintf(log, "  channel %u: line feed state 0x%02x after the write, the line stays open\n", ch, lf);
			err = -EIO;
		}
		if (err) {
			(void)chip_reg_write(c, ch, REG_LINEFEED, LF_CODE_OPEN);
		} else if (!read_battery(c, ch, &now, &expected) && !read_iloop(c, ch, &ua)) {
			fprintf(log, "  channel %u: battery %d mV, loop current %d uA on the fed line\n", ch, now, ua);
		}
	}
	return err;
}

/* Put the line back to open and verify it (the caller releases the oscillators before this for the tone stages). */
int release_line(struct chip *c, unsigned int ch, int err, FILE *log)
{
	uint8_t lf;
	int released;

	(void)chip_reg_write(c, ch, REG_LINEFEED, LF_CODE_OPEN);
	fprintf(log, "  channel %u: line feed released (open)\n", ch);
	released = chip_reg_read(c, ch, REG_LINEFEED, &lf);
	if (released && !err)
		err = released;
	if (!released && (lf & LF_STATE_MASK) != LF_CODE_OPEN) {
		fprintf(log, "  channel %u: line feed state 0x%02x is not open after the release\n", ch, lf);
		err = -EIO;
	}
	return err;
}


/*
 * One channel's voice path: the chip's own dialing controls stay off (we dial with AT commands), one enhancement bit
 * the voice path does not want is cleared, and the codec comes up with its samples at our frame bit positions; every write is read back.
 */
int voice_up(struct chip *c, const struct board *board, unsigned int ch, FILE *log)
{
	uint8_t got;
	int err;

	err = chip_reg_read(c, ch, REG_DIGCON, &got);
	if (!err)
		err = chip_reg_write(c, ch, REG_DIGCON, (uint8_t)(got & ~DIGCON_VOICE_MASK));
	if (!err)
		err = chip_reg_read(c, ch, REG_DIGCON, &got);
	if (!err && (got & DIGCON_VOICE_MASK)) {
		fprintf(log, "  channel %u: dialing control 0x%02x still has the voice-path bits\n", ch, got);
		return -EIO;
	}

	err = chip_reg_read(c, ch, REG_ENHANCE, &got);
	if (!err)
		err = chip_reg_write(c, ch, REG_ENHANCE, (uint8_t)(got & ~ENHANCE_VOICE_BIT));
	if (!err)
		err = chip_reg_read(c, ch, REG_ENHANCE, &got);
	if (!err && (got & ENHANCE_VOICE_BIT)) {
		fprintf(log, "  channel %u: enhancement 0x%02x still has the voice-path bit\n", ch, got);
		return -EIO;
	}

	err = chip_reg_write(c, ch, REG_PCMMODE, PCMMODE_SETUP);
	if (!err)
		err = chip_reg_read(c, ch, REG_PCMMODE, &got);
	if (!err)
		err = chip_reg_write(c, ch, REG_PCMTXLO, PCM_TX_START_PCLK);
	if (!err)
		err = chip_reg_write(c, ch, REG_PCMTXHI, PCTXHI_TAG | ((PCM_TX_START_PCLK >> 8) & 3));
	if (!err)
		err = chip_reg_write(c, ch, REG_PCM_RX_LO, PCM_RX_START_PCLK);
	if (!err) {
		err = chip_reg_read(c, ch, REG_PCMRXHI, &got);
		if (!err)
			err = chip_reg_write(c, ch, REG_PCMRXHI, (uint8_t)((got & ~0x03) | ((PCM_RX_START_PCLK >> 8) & 3)));
		if (!err)
			err = chip_reg_read(c, ch, REG_PCMRXHI, &got);
	}
	if (!err && (got & 0x03) != ((PCM_RX_START_PCLK >> 8) & 3)) {
		fprintf(log, "  channel %u: receive start count 0x%02x (upper bits wanted 0x%02x)\n", ch, got, (PCM_RX_START_PCLK >> 8) & 3);
		return -EIO;
	}
	/* the transmit and receive IIR chains from the board's private section */
	{
		const struct board_section *iir = board_find(board, "iir");

		if (!iir || iir->length != IIR_SECTION_SIZE) {
			fprintf(log, "  channel %u: no %u-byte iir section\n", ch, IIR_SECTION_SIZE);
			return -EINVAL;
		}
		for (unsigned int k = 0; !err && k < 2 * IIR_WORDS; k++) {
			uint32_t want = board_word(iir, 4 * k), back;
			unsigned int addr = k < IIR_WORDS ? RAM_TXIIR + k : RAM_RXIIR + (k - IIR_WORDS);

			err = chip_ram_write(c, ch, addr, want);
			if (!err)
				err = chip_ram_read(c, ch, addr, &back);
			if (!err && back != want) {
				fprintf(log, "  channel %u: voice filter word %u wrote 0x%08x, read 0x%08x\n", ch, k, want, back);
				err = -EIO;
			}
		}
	}
	if (!err) {
		err = chip_reg_write(c, ch, REG_PCMMODE, PCMMODE_RUN);
		if (!err)
			err = chip_reg_read(c, ch, REG_PCMMODE, &got);
		if (!err && got != PCMMODE_RUN) {
			fprintf(log, "  channel %u: codec mode 0x%02x after the start\n", ch, got);
			err = -EIO;
		}
	}
	return err;
}

/*
 * Load the impedance and the board's tone preset, but leave the oscillators off: the tone goes on when the caller
 * takes the handset up (detected off-hook during the hold). The caller holds protected mode.
 */
int tone_up(struct chip *c, unsigned int ch, const struct board_section *imped,
	const struct board_section *tones, FILE *log)
{
	int err;

	err = load_imped(c, ch, imped, log);
	if (err) {
		fprintf(log, "  FAILED on channel %u: impedance preset (%d)\n", ch, err);
		return err;
	}
	err = load_tone(c, ch, tones, log);
	if (err)
		fprintf(log, "  FAILED on channel %u: tone setup (%d)\n", ch, err);
	return err;
}

/* Stop the oscillators and release the line; the caller's error carries through. */
int tone_down(struct chip *c, unsigned int ch, int err, FILE *log)
{
	int stop_err;

	stop_err = osc_stop_once(c, ch, log);
	if (stop_err)
		fprintf(log, "  FAILED on channel %u: oscillator stop (%d)\n", ch, stop_err);
	if (!err)
		err = stop_err;
	return release_line(c, ch, err, log);
}

/*
 * Group 2 of the chip's event registers (register 19: the tone detector's event and the loop-change flag). Enabling
 * clears the pending events of all four groups first (reading them clears them) and then sets the enable bits;
 * both directions read the enable back. The caller holds protected mode.
 */
int irq2_enable(struct chip *c, unsigned int ch, uint8_t bits, FILE *log)
{
	uint8_t got, irq;
	unsigned int k;
	int err = 0;

	for (k = 0; !err && k < 4; k++)
		err = chip_reg_read(c, ch, REG_IRQ1 + k, &irq);
	if (!err)
		err = chip_reg_read(c, ch, REG_IRQEN2, &got);
	if (!err)
		err = chip_reg_write(c, ch, REG_IRQEN2, (uint8_t)(got | bits));
	if (!err)
		err = chip_reg_read(c, ch, REG_IRQEN2, &got);
	if (!err && (got & bits) != bits) {
		fprintf(log, "  channel %u: interrupt enable 0x%02x after the write (bits 0x%02x wanted)\n", ch, got, bits);
		err = -EIO;
	}
	return err;
}

int irq2_disable(struct chip *c, unsigned int ch, uint8_t bits, FILE *log)
{
	uint8_t got;
	int err = chip_reg_read(c, ch, REG_IRQEN2, &got);

	if (!err)
		err = chip_reg_write(c, ch, REG_IRQEN2, (uint8_t)(got & ~bits));
	if (!err)
		err = chip_reg_read(c, ch, REG_IRQEN2, &got);
	if (!err && (got & bits)) {
		fprintf(log, "  channel %u: interrupt enable 0x%02x still has an event bit\n", ch, got);
		err = -EIO;
	}
	return err;
}

/* the digit watch's chip setup: the detector register gets its verified enable value, and the tone-detector event and
 * the loop-change flag are enabled in group 2 */
int dtmf_enable(struct chip *c, unsigned int ch, FILE *log)
{
	uint8_t tones, got;
	int err = chip_reg_read(c, ch, REG_TONEN, &tones);

	if (!err)
		fprintf(log, "  channel %u: tone detector enable 0x%02x -> 0x%02x\n", ch, tones, TONEN_DTMF_DETECT);
	if (!err)
		err = chip_reg_write(c, ch, REG_TONEN, TONEN_DTMF_DETECT);
	if (!err)
		err = chip_reg_read(c, ch, REG_TONEN, &got);
	if (!err && got != TONEN_DTMF_DETECT) {
		fprintf(log, "  channel %u: tone detector enable wrote 0x%02x, read 0x%02x\n", ch, TONEN_DTMF_DETECT, got);
		err = -EIO;
	}
	return err ? err : irq2_enable(c, ch, IRQEN2_DTMF | IRQEN2_LCR, log);
}

/* the event enables go back (the detector register keeps its value: the chip is reset when the tool ends) */
int dtmf_disable(struct chip *c, unsigned int ch, FILE *log)
{
	uint8_t got;

	if (!chip_reg_read(c, ch, REG_TONEN, &got))
		fprintf(log, "  channel %u: tone detector back to 0x%02x\n", ch, got);
	return irq2_disable(c, ch, IRQEN2_DTMF | IRQEN2_LCR, log);
}
