// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * Calling-line identity. The message follows the multiple-data-message layout of the North American and the
 * European specifications (they agree on it); the signalling follows their preamble: 300 alternating bits (channel
 * seizure), about 180 mark bits, then the message with a start and a stop bit on every byte.
 */
#include <errno.h>
#include <string.h>

#include "cid.h"
#include "slic_map.h"

int cid_message(const struct cid_info *info, uint8_t *out, size_t cap)
{
	uint8_t *p = out + 2, sum = 0;
	size_t n = 0, k, len;
	const char *s;

	if (cap < CID_MESSAGE_MAX || info->month < 1 || info->month > 12 || info->day < 1 || info->day > 31 ||
	    info->hour < 0 || info->hour > 23 || info->minute < 0 || info->minute > 59)
		return -EINVAL;
	*p++ = CID_PARAM_DATETIME;
	*p++ = 8;
	*p++ = '0' + info->month / 10;
	*p++ = '0' + info->month % 10;
	*p++ = '0' + info->day / 10;
	*p++ = '0' + info->day % 10;
	*p++ = '0' + info->hour / 10;
	*p++ = '0' + info->hour % 10;
	*p++ = '0' + info->minute / 10;
	*p++ = '0' + info->minute % 10;

	if (info->number)
		for (s = info->number, k = 0; *s && k < CID_NUMBER_MAX; s++)
			if (*s >= '0' && *s <= '9')
				k++;
	if (info->number && k) {
		*p++ = CID_PARAM_NUMBER;
		*p++ = (uint8_t)k;
		for (s = info->number, n = 0; *s && n < k; s++)
			if (*s >= '0' && *s <= '9') {
				*p++ = (uint8_t)*s;
				n++;
			}
	} else {
		*p++ = CID_PARAM_REASON;
		*p++ = 1;
		*p++ = 'O';			/* out of area / not available */
	}

	if (info->name) {
		for (s = info->name, k = 0; *s && k < CID_NAME_MAX; s++)
			if (*s >= 0x20 && *s <= 0x7e)
				k++;
		if (k) {
			*p++ = CID_PARAM_NAME;
			*p++ = (uint8_t)k;
			for (s = info->name, n = 0; *s && n < k; s++)
				if (*s >= 0x20 && *s <= 0x7e) {
					*p++ = (uint8_t)*s;
					n++;
				}
		}
	}
	len = (size_t)(p - out);
	out[0] = CID_TYPE_MDMF;
	out[1] = (uint8_t)(len - 2);
	for (k = 0; k < len; k++)
		sum = (uint8_t)(sum + out[k]);
	*p++ = (uint8_t)(0x100 - sum);		/* the two's complement: the bytes and the check add up to zero */
	return (int)(p - out);
}

static int put_reg(struct chip *c, unsigned int ch, unsigned int reg, uint8_t value)
{
	return chip_reg_write(c, ch, reg, value);
}

/* one byte into the chip's FSK buffer; the armed machinery reports room for the next one in register 18 (the flag is
 * cleared by writing it back) */
static int fsk_byte(struct chip *c, unsigned int ch, uint8_t value)
{
	unsigned int wait;
	uint8_t irq;
	int err = put_reg(c, ch, REG_FSKDAT, value);

	for (wait = 0; !err && wait < FSK_ROOM_WAIT_MS; wait++) {
		err = chip_reg_read(c, ch, REG_IRQ1, &irq);
		if (!err && (irq & IRQ1_FSK_ROOM)) {
			(void)chip_reg_write(c, ch, REG_IRQ1, irq & IRQ1_FSK_ROOM);
			return 0;
		}
		if (!err)
			c->ops->pause_ms(c->ctx, 1);
	}
	return err ? err : -ETIMEDOUT;
}

/* bytes into the chip's FSK buffer; records the index of the first byte that failed. Each byte waits for the buffer
 * to take it, so the send paces itself at the signalling rate and a "sent" means every byte was consumed */
static int fsk_run(struct chip *c, unsigned int ch, uint8_t value, const uint8_t *msg, unsigned int count, unsigned int *failed,
	const struct cid_guard *guard)
{
	unsigned int k;
	int err = 0;

	for (k = 0; !err && k < count; k++) {
		err = guard && guard->stop(guard->ctx) ? -ECANCELED : fsk_byte(c, ch, msg ? msg[k] : value);
		if (err)
			*failed = k;
	}
	return err;
}

static int load_fsk(struct chip *c, const struct board *b, unsigned int ch, int standard, uint8_t *omode)
{
	static const uint16_t ram[6] = { RAM_FSK01, RAM_FSK10, RAM_FSKAMP0, RAM_FSKAMP1, RAM_FSKFREQ0, RAM_FSKFREQ1 };
	const struct board_section *fsk = board_find(b, "fsk");
	unsigned int base = (standard == CID_ETSI ? 1 : 0) * FSK_PRESET_SIZE, k;
	int err = 0;

	if (!fsk || fsk->length != FSK_PRESET_COUNT * FSK_PRESET_SIZE)
		return -EINVAL;
	err = put_reg(c, ch, REG_FSKDEPTH, FSK_DEPTH_PRIME);			/* arm the buffer machinery */
	for (k = 0; !err && k < 4; k++)
		err = put_reg(c, ch, REG_O1_TALO + k, k == 0 ? FSK_O1_TALO : 0);	/* the oscillator timers are not used for FSK */
	if (!err)
		err = put_reg(c, ch, REG_FSKDEPTH, fsk->data[base + FSK_DEPTH_OFFSET]);
	for (k = 0; !err && k < 6; k++) {
		uint32_t want = board_word(fsk, base + 4 * k), back;

		if (want > RAM_VALUE_MAX)
			return -ERANGE;
		err = chip_ram_write(c, ch, ram[k], want);
		if (!err)
			err = chip_ram_read(c, ch, ram[k], &back);
		if (!err && back != want)
			err = -EIO;
	}
	if (!err)
		err = chip_reg_read(c, ch, REG_OMODE, omode);
	return err;
}

/* the tone pairs of the digits the DTMF variant needs, as the stock module's own dialing table holds them (its
 * pairs are the ones a DTMF receiver on this board is known to accept); the two oscillators carry the low and the
 * high group of each digit. The twelve pairs are the standard DTMF matrix (the first word is the column's tone,
 * the last two the row's), so the extended frame's start and end code 'D' is composed from them the same way the
 * matrix puts it: the 1633 Hz column over the 941 Hz row */
static const struct { char digit; uint32_t f1, a1, f2, a2; } dtmf_pairs[] = {
	{ '1', 0x04a82000u, 0x0ed2000u, 0x06d4c000u, 0x0818000u },
	{ '2', 0x03fc6000u, 0x10b0000u, 0x06d4c000u, 0x0818000u },
	{ '3', 0x0331e000u, 0x12e4000u, 0x06d4c000u, 0x0818000u },
	{ '4', 0x04a82000u, 0x0ed2000u, 0x0694e000u, 0x0900000u },
	{ '5', 0x03fc6000u, 0x10b0000u, 0x0694e000u, 0x0900000u },
	{ '6', 0x0331e000u, 0x12e4000u, 0x0694e000u, 0x0900000u },
	{ '7', 0x04a82000u, 0x0ed2000u, 0x06466000u, 0x0a06000u },
	{ '8', 0x03fc6000u, 0x10b0000u, 0x06466000u, 0x0a06000u },
	{ '9', 0x0331e000u, 0x12e4000u, 0x06466000u, 0x0a06000u },
	{ '0', 0x03fc6000u, 0x10b0000u, 0x05e9c000u, 0x0b2c000u },
	{ 'A', 0x02464000u, 0x1586000u, 0x06d4c000u, 0x0818000u },
	{ 'C', 0x02464000u, 0x1586000u, 0x06466000u, 0x0a06000u },
	{ 'D', 0x02464000u, 0x1586000u, 0x05e9c000u, 0x0b2c000u },
};

#define DTMF_OMODE		0x66			/* both oscillators routed to the line, as every tone preset */
#define DTMF_DIGIT_MS		70
#define DTMF_SILENCE_MS		70

/* a pause in slices of CID_SLICE_MS with the guard asked between them: the hook is read every few ms during the send too */
#define CID_SLICE_MS	5

static int guarded_pause(struct chip *c, unsigned int ms, const struct cid_guard *guard)
{
	while (ms) {
		unsigned int step = ms < CID_SLICE_MS ? ms : CID_SLICE_MS;

		if (guard && guard->stop(guard->ctx))
			return -ECANCELED;
		c->ops->pause_ms(c->ctx, step);
		ms -= step;
	}
	return 0;
}

static int dtmf_digit(struct chip *c, unsigned int ch, char digit, const struct cid_guard *guard, FILE *log)
{
	static const uint16_t ram[6] = { RAM_OSC1_FREQ, RAM_OSC1_AMP, RAM_OSC1_PHAS, RAM_OSC2_FREQ, RAM_OSC2_AMP, RAM_OSC2_PHAS };
	uint32_t values[6];
	unsigned int k, found = 0;
	int err = 0;

	for (k = 0; k < sizeof(dtmf_pairs) / sizeof(dtmf_pairs[0]); k++)
		if (dtmf_pairs[k].digit == digit) {
			values[0] = dtmf_pairs[k].f1;
			values[1] = dtmf_pairs[k].a1;
			values[2] = 0;
			values[3] = dtmf_pairs[k].f2;
			values[4] = dtmf_pairs[k].a2;
			values[5] = 0;
			found = 1;
		}
	if (!found)
		return 0;					/* a character with no tone pair is skipped */
	for (k = 0; !err && k < 6; k++) {
		uint32_t back;

		err = chip_ram_write(c, ch, ram[k], values[k]);
		if (!err)
			err = chip_ram_read(c, ch, ram[k], &back);
		if (!err && back != values[k])
			err = -EIO;
	}
	if (!err)
		err = chip_reg_write(c, ch, REG_OMODE, DTMF_OMODE);
	if (!err)
		err = chip_reg_write(c, ch, REG_OCON, OCON_ENABLE_SET);
	if (!err) {
		err = guarded_pause(c, DTMF_DIGIT_MS, guard);
		/* the tone is stopped whatever ended the wait */
		if (chip_reg_write(c, ch, REG_OCON, 0) && !err)
			err = -EIO;
	}
	if (!err)
		err = guarded_pause(c, DTMF_SILENCE_MS, guard);
	if (err && err != -ECANCELED)
		fprintf(log, "  channel %u: dtmf digit %c failed (%d)\n", ch, digit, err);
	return err;
}

int cid_dtmf_send(struct chip *c, const struct board *b, unsigned int ch, const struct tm *tm, const char *number,
	const struct cid_guard *guard, FILE *log)
{
	uint8_t omode, ocon;
	uint32_t osc[6];
	char frame[2 + 8 + CID_NUMBER_MAX + 1];
	unsigned int k, n = 0;
	int err, back = 0;

	(void)b;
	frame[n++] = tm ? 'D' : 'A';			/* the start code */
	if (tm)
		n += (unsigned int)snprintf(frame + n, sizeof(frame) - n, "%02d%02d%02d%02d",
		    tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
	for (const char *p = number; *p && n < sizeof(frame) - 2; p++)
		if (*p >= '0' && *p <= '9')
			frame[n++] = *p;
	frame[n++] = tm ? 'D' : 'C';			/* the end code */

	err = chip_reg_read(c, ch, REG_OMODE, &omode);
	if (!err)
		err = chip_reg_read(c, ch, REG_OCON, &ocon);
	for (k = 0; !err && k < 6; k++)			/* the dial tone's own oscillator words, put back afterwards */
		err = chip_ram_read(c, ch, RAM_OSC1_FREQ + k, &osc[k]);
	for (k = 0; !err && k < n; k++)
		err = guard && guard->stop(guard->ctx) ? -ECANCELED : dtmf_digit(c, ch, frame[k], guard, log);
	if (!err)
		err = guarded_pause(c, DTMF_SILENCE_MS, guard);

	for (k = 0; !back && k < 6; k++)
		back = chip_ram_write(c, ch, RAM_OSC1_FREQ + k, osc[k]);
	if (!back)
		back = chip_reg_write(c, ch, REG_OMODE, omode);
	if (!back)
		back = chip_reg_write(c, ch, REG_OCON, ocon);
	if (err == -ECANCELED)
		fprintf(log, "  channel %u: dtmf caller id stopped at %u of %u digits: the handset was lifted\n", ch, k - 1, n);
	else if (err)
		fprintf(log, "  channel %u: dtmf caller id failed at %u of %u digits (error %d)\n", ch, k, n, err);
	else
		fprintf(log, "  channel %u: dtmf caller id sent (%u tones%s)\n", ch, n, tm ? ", with the date and time" : "");
	return err ? err : back;
}

int cid_send(struct chip *c, const struct board *b, unsigned int ch, int standard, const uint8_t *msg, size_t len,
	const struct cid_guard *guard, FILE *log)
{
	uint8_t omode, ocon, timers[4], mode, irqen = 0;
	unsigned int k, at_byte = 0;
	const char *at;
	int err, back = 0;

	if (standard != CID_BELLCORE && standard != CID_ETSI)
		return -EINVAL;
	err = chip_reg_read(c, ch, REG_OCON, &ocon);
	for (k = 0; !err && k < 4; k++)
		err = chip_reg_read(c, ch, REG_O1_TALO + k, &timers[k]);
	if (!err)
		err = chip_reg_read(c, ch, REG_IRQEN1, &irqen);
	if (!err) {
		at = "room flag enable";
		err = put_reg(c, ch, REG_IRQEN1, (uint8_t)(irqen | IRQEN1_FSK_ROOM));
	}
	if (!err)
		err = load_fsk(c, b, ch, standard, &omode);
	if (err) {
		fprintf(log, "  channel %u: caller-id setup failed (%d)\n", ch, err);
		return err;
	}

	mode = (uint8_t)((omode & ~OMODE_STARTSTOP) | OMODE_FSK);
	at = "oscillator stop";
	err = put_reg(c, ch, REG_OCON, 0);
	if (!err) {
		uint8_t irq;

		(void)chip_reg_read(c, ch, REG_IRQ1, &irq);			/* start with no stale room flag */
		at = "fsk mode";
		err = put_reg(c, ch, REG_OMODE, mode);
	}
	if (!err) {
		at = "carrier start";
		err = put_reg(c, ch, REG_OCON, OCON_FSK);
	}
	if (!err) {
		at = "seizure";
		err = fsk_run(c, ch, 0x55, NULL, FSK_SEIZURE_BYTES, &at_byte, guard);
	}
	if (!err) {
		at = "mark";
		err = fsk_run(c, ch, 0xff, NULL, FSK_MARK_BYTES, &at_byte, guard);
	}
	if (!err) {
		at = "start-stop flag";					/* from here every byte carries its start and stop bit */
		err = put_reg(c, ch, REG_OMODE, (uint8_t)(mode | OMODE_STARTSTOP));
	}
	if (!err) {
		at = "message";
		err = fsk_run(c, ch, 0, msg, (unsigned int)len, &at_byte, guard);
	}
	if (!err)
		c->ops->pause_ms(c->ctx, FSK_DRAIN_MS);					/* the last byte's room already paced the drain */

	back = put_reg(c, ch, REG_OCON, 0);
	if (!back)
		back = put_reg(c, ch, REG_OMODE, omode);
	for (k = 0; !back && k < 4; k++)
		back = put_reg(c, ch, REG_O1_TALO + k, timers[k]);
	if (!back)
		back = put_reg(c, ch, REG_OCON, ocon);
	if (!back)
		back = put_reg(c, ch, REG_IRQEN1, irqen);
	if (err == -ECANCELED)
		fprintf(log, "  channel %u: caller id stopped at %s (byte %u): the handset was lifted\n", ch, at, at_byte);
	else if (err)
		fprintf(log, "  channel %u: caller id failed at %s (byte %u, error %d)\n", ch, at, at_byte, err);
	else
		fprintf(log, "  channel %u: caller id sent (%s, %zu message bytes)\n", ch, standard == CID_ETSI ? "european" : "north american", len);
	return err ? err : back;
}
