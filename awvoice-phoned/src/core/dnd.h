// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#ifndef AW_PHONED_DND_H
#define AW_PHONED_DND_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "numbers.h"	/* the comparison form of an allowed number, and the region's rules */
#include "phone.h"	/* PHONE_NUMBER_MAX */

/* plain stdint types: this header sits beside front_end.h in the same translation unit, and its
   typedefs are already there */

/*
 * Quiet hours: when the line may ring.
 *
 * One decision, gathered from what the caller already knows - the local time, the windows the admin wrote, and
 * whether the handset or the page put the line on quiet by hand. Nothing here reads a clock, a config or the modem:
 * the daemon gathers those and hands them in, so the whole rule is plain arithmetic and the host tests cover it
 * (tests/test_dnd.c).
 *
 * A window is a span of minutes within the local day, optionally on chosen weekdays. `from` after `to` means the
 * span crosses midnight (22:00 to 07:00 is the night, not nothing). `all_day`, or a span whose two ends are equal,
 * is the whole day - the weekday list still applies, so a weekend window can be all day. The first enabled window
 * that matches decides; later windows never widen an answer, because two windows that disagree would leave the
 * caller with no way to say which one the router meant.
 *
 * A minute exactly at `from` is inside; a minute exactly at `to` is outside - the span belongs to the day it starts
 * in, and a window that ended and another that begins at the same minute must not both claim that minute.
 *
 * The clock is a precondition, not a detail: before the time is known the epoch is 1970 and every window would
 * match or none would, so a schedule is never honoured on an unknown clock. A hand-set quiet (the handset's *78)
 * needs no clock at all - the admin asked for quiet, not for a time.
 */

enum dnd_mode {
	DND_MODE_SILENT = 0,	/* the call arrives, nothing on this side makes a sound */
	DND_MODE_MACHINE,	/* the answering machine takes it after its own delay */
	DND_MODE_REJECT		/* the caller is released without an answer here */
};

enum dnd_manual {
	DND_MANUAL_AUTO = 0,	/* follow the windows */
	DND_MANUAL_ON,		/* *78, or the page's switch: quiet now, whatever the clock says */
	DND_MANUAL_OFF		/* *79: no window may quiet the line until this is cleared */
};

/*
 * The two codes the handset dials for itself, named after the old exchange's service codes for the same pair of
 * asks (turn "do not disturb" on, turn it off): the line answers them and the network is never sent them. A dial
 * string that is not exactly one of these four is left alone, because a code of the network's (`*78` with anything
 * after it, a forwarding string, an ordinary number) belongs to the modem, not here. A code with the send key on
 * the end counts as the same ask: a person at the phone often ends one with it, and inside a string that begins
 * with a star the key is collected as part of the string rather than acting as a send key, so both complete on the
 * pause after the last key, as every other dial does.
 */
enum dnd_code { DND_CODE_NONE = 0, DND_CODE_ON, DND_CODE_OFF };

static inline enum dnd_code dnd_code_of(const char *dial)
{
	if (!strcmp(dial, "*78") || !strcmp(dial, "*78#"))
		return DND_CODE_ON;
	if (!strcmp(dial, "*79") || !strcmp(dial, "*79#"))
		return DND_CODE_OFF;
	return DND_CODE_NONE;
}

enum dnd_cause {
	DND_CAUSE_NONE = 0,	/* the line may ring */
	DND_CAUSE_WINDOW,	/* a window matched */
	DND_CAUSE_ALL_DAY,	/* a window matched and covers the whole day */
	DND_CAUSE_MANUAL	/* asked for by hand */
};

#define DND_DAY_SUN	(1 << 0)
#define DND_DAY_MON	(1 << 1)
#define DND_DAY_TUE	(1 << 2)
#define DND_DAY_WED	(1 << 3)
#define DND_DAY_THU	(1 << 4)
#define DND_DAY_FRI	(1 << 5)
#define DND_DAY_SAT	(1 << 6)
#define DND_DAYS_EVERY	0	/* no list means every day */

#define DND_WINDOWS	8	/* config dnd sections honoured, in the order they are written */
#define DND_MINUTES_DAY	1440

struct dnd_window {
	int enabled;
	int from_min;		/* -1 when unset */
	int to_min;		/* -1 when unset */
	int days;		/* DND_DAYS_EVERY, or a mask of the weekday bits */
	int all_day;
	int mode;		/* enum dnd_mode */
	int repeat_min;		/* the repeat-caller escape; 0 or less turns it off */
};

/* what the line decided about one incoming call */
struct dnd_state {
	int quiet;		/* 1 when the line must not ring */
	int mode;		/* enum dnd_mode, the window's own */
	int window;		/* which window answered, -1 for a hand-set quiet */
	int cause;		/* enum dnd_cause */
	int repeat_min;		/* the escape this decision carries with it */
};

/* "22:00" -> 1320. Anything else - empty, a stray character, 24:00, a minute above 59 - is -1. Leading and
 * trailing blanks are the config file's own, so they are tolerated; a missing minute part is not. */
static inline int dnd_parse_minute(const char *text)
{
	unsigned int h = 0, m = 0;
	size_t k = 0, start;

	if (!text)
		return -1;
	while (text[k] == ' ' || text[k] == '\t')
		k++;
	if (!text[k])
		return -1;
	start = k;
	while (text[k] >= '0' && text[k] <= '9') {
		h = h * 10 + (unsigned int)(text[k] - '0');
		if (h > 23)
			return -1;
		k++;
	}
	if (k == start || text[k] != ':')
		return -1;	/* ":00" names no hour */
	k++;
	if (text[k] < '0' || text[k] > '9')
		return -1;
	start = k;
	while (text[k] >= '0' && text[k] <= '9') {
		m = m * 10 + (unsigned int)(text[k] - '0');
		if (m > 59)
			return -1;
		k++;
	}
	if (k - start != 2)
		return -1;	/* the minute is two digits: "12:5" is a half-written time, not 12:05 */
	while (text[k] == ' ' || text[k] == '\t')
		k++;
	if (text[k])
		return -1;
	return (int)(h * 60 + m);
}


/* sun/mon/... in any case, the three-letter form only: a config that says "sunday" is a mistake, not a weekday */
static inline int dnd_parse_day(const char *text)
{
	static const char *const names[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
	char low[4];
	size_t k;

	if (!text)
		return -1;
	for (k = 0; k < 3 && text[k]; k++)
		low[k] = (text[k] >= 'A' && text[k] <= 'Z') ? (char)(text[k] + ('a' - 'A')) : text[k];
	if (k != 3 || text[3])
		return -1;
	low[3] = 0;
	for (k = 0; k < 7; k++)
		if (!strcmp(low, names[k]))
			return 1 << (int)k;
	return -1;
}

/*
 * A clock that has never been told the real date reads 1970, and no schedule means anything then. The floor is a
 * date a device cannot reach by counting up from its own boot - an epoch this small is only ever a missing clock -
 * and every date the network, the admin or a time server can hand the router sits above it.
 */
#define DND_EPOCH_FLOOR	1577836800ull	/* 2020-01-01 00:00:00 UTC */

static inline int dnd_clock_known(uint64_t epoch_s)
{
	return epoch_s >= DND_EPOCH_FLOOR;
}

/* what the daemon worked out about the present moment: it owns the clock and remembers the caller it last quieted,
 * this module owns the rules */
struct dnd_now {
	int clock_known;
	int weekday;		/* 0 = Sunday */
	int minute;		/* minutes past the local midnight */
	uint64_t epoch_s;
	uint64_t last_peer_epoch;	/* the call the line quieted last, in whole seconds; 0 when there is none */
};

/*
 * The quiet hours the admin wrote: windows in the order they were written, and the numbers that always ring through
 * them. The daemon fills this from the UCI sections of type `dnd`; like the blocking rules, it is read during a step
 * and changed only between two steps.
 *
 * A contact the phone book marks to ring through (phonebook.h) answers with the same `DND_R_ALLOWED` as a written
 * number: the list is the admin's own, the book is the same ask held by the name it belongs to, and a caller does not
 * care which of the two the router read.
 */
#define DND_ALLOW_MAX	20

struct phonebook;

struct dnd {
	struct dnd_window window[DND_WINDOWS];
	int count;
	char allow[DND_ALLOW_MAX][PHONE_NUMBER_MAX + 1];
	unsigned int allow_count;
	const struct number_region *region;	/* NULL = the default region */
	const struct phonebook *book;		/* the contacts that ring through (NULL = none); the daemon owns it */
	int manual;		/* enum dnd_manual: the handset's *78 and *79, and the page's switch */
	struct dnd_now now;	/* filled by the daemon between steps, read by the core during one */
};

void dnd_clear(struct dnd *d, const struct number_region *region);

/*
 * One window. `from` and `to` are "22:00" strings and both must read as a time unless `all_day` is set; `days` is the
 * weekday mask (DND_DAYS_EVERY for no list); `mode` is an enum dnd_mode; `repeat_min` is 0..1440. 0, -EINVAL (any
 * field that cannot mean what it is written as), -ENOSPC (the eighth window is already there). A refusal says which
 * field in the log and never holds a number - there is none in a window.
 */
int dnd_window_add(struct dnd *d, int enabled, const char *from, const char *to, int days, int all_day,
	int mode, int repeat_min, FILE *log);

/* One always-ring number, in the same form the blocking rules take (digits, an optional leading +, * and #). */
int dnd_allow_add(struct dnd *d, const char *value, FILE *log);

/* 1 when `number` ("" = withheld) is on the always-ring list or on a contact that asks to ring through; a withheld
 * caller is neither. */
int dnd_allow_hit(const struct dnd *d, const char *number);

/* the mode word the config file writes: silent, machine or reject; -1 for anything else */
static inline int dnd_mode_parse(const char *text)
{
	if (!text)
		return DND_MODE_SILENT;		/* unset means the plainest quiet */
	if (!strcmp(text, "silent"))
		return DND_MODE_SILENT;
	if (!strcmp(text, "machine"))
		return DND_MODE_MACHINE;
	if (!strcmp(text, "reject"))
		return DND_MODE_REJECT;
	return -1;
}

/* the weekday list the config file writes; an empty list is every day, a name that is not a weekday is -1 */
static inline int dnd_days_parse(const char *const *names, int count)
{
	int mask = DND_DAYS_EVERY, k, one;

	for (k = 0; k < count; k++) {
		if (!names[k] || !names[k][0])
			continue;
		one = dnd_parse_day(names[k]);
		if (one < 0)
			return -1;
		mask |= one;
	}
	return mask;
}

/* one weekday, one minute of the local day: is this window open now? */
static inline int dnd_window_hit(const struct dnd_window *w, int weekday, int minute)
{
	if (!w->enabled || minute < 0 || minute >= DND_MINUTES_DAY || weekday < 0 || weekday > 6)
		return 0;
	if (w->days != DND_DAYS_EVERY && !(w->days & (1 << weekday)))
		return 0;
	if (w->all_day)
		return 1;
	if (w->from_min < 0 || w->to_min < 0)
		return 0;			/* an incomplete span is not a window */
	if (w->from_min == w->to_min)
		return 1;			/* equal ends mean the whole day, not nothing */
	if (w->from_min < w->to_min)
		return minute >= w->from_min && minute < w->to_min;
	return minute >= w->from_min || minute < w->to_min;	/* across midnight */
}

/* the windows in order, the clock as the caller found it, and the handset's own switch */
static inline void dnd_decide(const struct dnd_window *windows, int count, int manual, int clock_known,
	int weekday, int minute, struct dnd_state *out)
{
	int k;

	memset(out, 0, sizeof(*out));
	out->mode = DND_MODE_SILENT;
	out->window = -1;

	if (manual == DND_MANUAL_OFF)
		return;				/* the line was taken off quiet by hand: no window may override it */
	if (manual == DND_MANUAL_ON) {
		out->quiet = 1;
		out->cause = DND_CAUSE_MANUAL;
		for (k = 0; k < count; k++)
			if (windows[k].enabled) {
				out->mode = windows[k].mode;
				out->repeat_min = windows[k].repeat_min;
				break;
			}
		return;
	}
	if (!clock_known)
		return;				/* an unknown clock cannot match a schedule */
	for (k = 0; k < count; k++) {
		if (!dnd_window_hit(&windows[k], weekday, minute))
			continue;
		out->quiet = 1;
		out->mode = windows[k].mode;
		out->window = k;
		out->cause = (windows[k].all_day || windows[k].from_min == windows[k].to_min) ?
			DND_CAUSE_ALL_DAY : DND_CAUSE_WINDOW;
		out->repeat_min = windows[k].repeat_min;
		return;
	}
}

/*
 * The repeat-caller escape: a second call from the same number inside the window rings anyway, because a caller who
 * tries twice is usually one who must be heard. Both times are whole seconds since the epoch, the shape the call log
 * keeps its own times in, but the pair lives in the line's own state: a service that restarts remembers nothing, and
 * the escape begins again from the first call the new one quiets. A clock that moved backwards is not a repeat: it is
 * the network's time arriving late, and the last call may not even have happened yet.
 */
static inline int dnd_repeat_due(uint64_t last_epoch, uint64_t now_epoch, int repeat_min)
{
	uint64_t span;

	if (repeat_min <= 0 || last_epoch == 0 || now_epoch < last_epoch)
		return 0;
	span = now_epoch - last_epoch;
	return span < (uint64_t)repeat_min * 60;
}

/*
 * Why the line did or did not ring for one caller. The words are the page's and the log's, so a quieted call can be
 * told apart from a blocked one and from a call nobody offered the handset to.
 */
enum dnd_reason {
	DND_R_NOT_QUIET = 0,	/* no window is open: the line rings */
	DND_R_CLOCK,		/* the time is not known yet: no schedule is honoured */
	DND_R_EMERGENCY,	/* an emergency number rings through any window */
	DND_R_ALLOWED,		/* the caller is on the always-ring list */
	DND_R_REPEAT,		/* the same caller again inside the escape: it rings */
	DND_R_QUIET		/* quiet: the handset is not driven */
};

/*
 * The whole answer for one incoming caller: whether the line must stay quiet, and in what mode. The bypasses are
 * checked only once a window is open, because a caller who rings through a window is not the same case as a caller
 * no window covers. `peer` is the comparison form the core already holds ("" = withheld).
 */
static inline int dnd_quiet(const struct dnd *d, const char *peer, const struct dnd_now *now,
	int *mode, int *reason)
{
	struct dnd_state st;

	if (mode)
		*mode = DND_MODE_SILENT;
	if (reason)
		*reason = DND_R_NOT_QUIET;
	if (!d || !now)
		return 0;
	if (!now->clock_known && d->manual != DND_MANUAL_ON) {	/* a quiet asked for by hand needs no clock */
		if (reason)
			*reason = DND_R_CLOCK;
		return 0;
	}
	dnd_decide(d->window, d->count, d->manual, now->clock_known, now->weekday, now->minute, &st);
	if (!st.quiet)
		return 0;
	if (mode)
		*mode = st.mode;
	if (number_is_emergency(peer ? peer : "", d->region)) {
		if (reason)
			*reason = DND_R_EMERGENCY;
		return 0;
	}
	if (dnd_allow_hit(d, peer)) {
		if (reason)
			*reason = DND_R_ALLOWED;
		return 0;
	}
	if (dnd_repeat_due(now->last_peer_epoch, now->epoch_s, st.repeat_min)) {
		if (reason)
			*reason = DND_R_REPEAT;
		return 0;
	}
	if (reason)
		*reason = DND_R_QUIET;
	return 1;
}

#endif /* AW_PHONED_DND_H */
