/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_SLIC_CID_H
#define AW_SLIC_CID_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "board.h"
#include "chip.h"

/*
 * Calling-line identity for the handset's display: the multiple-data-message format (message type 0x80) with the
 * date and time (parameter 0x01), the number (0x02) or the reason it is missing (0x04), and the name (0x07), sent as
 * frequency-shift keying on the line between the first and the second ring burst. Two signalling variants exist:
 * the North American one (1200 baud, 1200/2200 Hz) and the European one (1200 baud, 1300/2100 Hz); the board file's
 * "fsk" section holds the chip's words for both, preset 0 and preset 1.
 */
enum cid_standard { CID_OFF, CID_BELLCORE, CID_ETSI, CID_DTMF, CID_DTMF_TIME };

#define CID_MESSAGE_MAX		80
#define CID_NUMBER_MAX		20
#define CID_NAME_MAX		15

struct cid_info {
	int month, day, hour, minute;	/* month 1..12 */
	const char *number;		/* digits are taken, anything else is dropped; empty = the number is not available */
	const char *name;		/* printable characters only, cut at CID_NAME_MAX; may be NULL or empty */
};

/* the message bytes (type, length, parameters, checksum); returns the length or a negative errno */
int cid_message(const struct cid_info *info, uint8_t *out, size_t cap);

/*
 * A send takes up to a few seconds, during which the hook would go unwatched. The guard is asked between the units of a
 * send (a data byte, a tone pair); a non-zero answer (the handset was lifted: the call is being answered, the ring
 * trips) stops the send at once with -ECANCELED, the chip's registers put back as after a finished send. NULL = none.
 */
struct cid_guard {
	int (*stop)(void *ctx);
	void *ctx;
};

/* send one message: the seizure, the mark and the data bytes; the oscillator registers are put back afterwards. The
 * line goes back to the state it was found in, a ringing one as forward active (the chip starts a burst whenever the
 * line enters the ringing state; the caller puts the ring back on at the cadence's own time) */
int cid_send(struct chip *c, const struct board *b, unsigned int ch, int standard, const uint8_t *msg, size_t len,
	const struct cid_guard *guard, FILE *log);

/* the DTMF variant: the start code 'A', the number's digits and the end code 'C', each a 70 ms tone pair from the
 * two oscillators with 70 ms of silence, sent while the line keeps ringing; the oscillator registers go back after.
 * With tm (the router's local time) the extended frame instead: the start code 'D', the date and time as the eight
 * digits MMDDHHMM, the number's digits and the end code 'D' — the frame handsets that show the date and time decode */
int cid_dtmf_send(struct chip *c, const struct board *b, unsigned int ch, const struct tm *tm, const char *number,
	const struct cid_guard *guard, FILE *log);

#endif
