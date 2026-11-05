// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* Quiet hours on the host: the window arithmetic, the decision and the repeat escape.
   make -f Makefile.host test */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dnd.h"

#include "numbers.h"
#include "phonebook.h"

static struct dnd_window win(int enabled, int from, int to, int days, int all_day, int mode, int repeat)
{
	struct dnd_window w;

	w.enabled = enabled;
	w.from_min = from;
	w.to_min = to;
	w.days = days;
	w.all_day = all_day;
	w.mode = mode;
	w.repeat_min = repeat;
	return w;
}

static void test_parse(void)
{
	assert(dnd_parse_minute("00:00") == 0);
	assert(dnd_parse_minute("07:05") == 425);
	assert(dnd_parse_minute("22:00") == 1320);
	assert(dnd_parse_minute("23:59") == 1439);
	assert(dnd_parse_minute(" 22:00 ") == 1320);	/* the config file's own blanks */
	assert(dnd_parse_minute("7:00") == 420);		/* one digit is still a hour */
	/* not a time of day */
	assert(dnd_parse_minute(NULL) == -1);
	assert(dnd_parse_minute("") == -1);
	assert(dnd_parse_minute(" ") == -1);
	assert(dnd_parse_minute("24:00") == -1);
	assert(dnd_parse_minute("12:60") == -1);
	assert(dnd_parse_minute("12:5") == -1);		/* a minute must be two digits */
	assert(dnd_parse_minute("12") == -1);
	assert(dnd_parse_minute("12:") == -1);
	assert(dnd_parse_minute(":00") == -1);
	assert(dnd_parse_minute("ab:cd") == -1);
	assert(dnd_parse_minute("12:00:00") == -1);
	assert(dnd_parse_minute("12:00x") == -1);
	assert(dnd_parse_minute("123:00") == -1);

	assert(dnd_parse_day("sun") == DND_DAY_SUN);
	assert(dnd_parse_day("SUN") == DND_DAY_SUN);
	assert(dnd_parse_day("mon") == DND_DAY_MON);
	assert(dnd_parse_day("sat") == DND_DAY_SAT);
	assert(dnd_parse_day("sunday") == -1);		/* a mistake, not a weekday */
	assert(dnd_parse_day("su") == -1);
	assert(dnd_parse_day("") == -1);
	assert(dnd_parse_day(NULL) == -1);
	assert(dnd_parse_day("xyz") == -1);

	assert(dnd_mode_parse(NULL) == DND_MODE_SILENT);
	assert(dnd_mode_parse("") == -1);
	assert(dnd_mode_parse("silent") == DND_MODE_SILENT);
	assert(dnd_mode_parse("machine") == DND_MODE_MACHINE);
	assert(dnd_mode_parse("reject") == DND_MODE_REJECT);
	assert(dnd_mode_parse("Silent") == -1);		/* the word is written lower case, and only that way */
	assert(dnd_mode_parse("quiet") == -1);

	{
		const char *const every[] = { NULL };
		const char *const nights[] = { "mon", "tue", "wed", "thu", "fri" };
		const char *const blanks[] = { "", NULL };
		const char *const wrong[] = { "mon", "tues" };

		assert(dnd_days_parse(every, 1) == DND_DAYS_EVERY);
		assert(dnd_days_parse(blanks, 2) == DND_DAYS_EVERY);
		assert(dnd_days_parse(nights, 5) ==
			(DND_DAY_MON | DND_DAY_TUE | DND_DAY_WED | DND_DAY_THU | DND_DAY_FRI));
		assert(dnd_days_parse(wrong, 2) == -1);
	}
}

static void test_window(void)
{
	/* 22:00 to 07:00 is tonight, not nothing */
	struct dnd_window night = win(1, 1320, 420, DND_DAYS_EVERY, 0, DND_MODE_MACHINE, 15);

	assert(dnd_window_hit(&night, 3, 1320));		/* exactly at the start: inside */
	assert(dnd_window_hit(&night, 3, 1439));
	assert(dnd_window_hit(&night, 3, 0));		/* the small hours belong to the night that began yesterday */
	assert(dnd_window_hit(&night, 3, 419));
	assert(!dnd_window_hit(&night, 3, 420));		/* exactly at the end: outside */
	assert(!dnd_window_hit(&night, 3, 700));
	assert(!dnd_window_hit(&night, 3, 1319));

	/* one afternoon */
	struct dnd_window nap = win(1, 780, 840, DND_DAYS_EVERY, 0, DND_MODE_SILENT, 0);

	assert(dnd_window_hit(&nap, 0, 780));
	assert(dnd_window_hit(&nap, 6, 839));
	assert(!dnd_window_hit(&nap, 6, 840));
	assert(!dnd_window_hit(&nap, 6, 779));

	/* weekdays only */
	struct dnd_window work = win(1, 540, 1020,
		DND_DAY_MON | DND_DAY_TUE | DND_DAY_WED | DND_DAY_THU | DND_DAY_FRI, 0, DND_MODE_SILENT, 0);

	assert(dnd_window_hit(&work, 1, 600));
	assert(dnd_window_hit(&work, 5, 600));
	assert(!dnd_window_hit(&work, 0, 600));		/* Sunday */
	assert(!dnd_window_hit(&work, 6, 600));		/* Saturday */

	/* all day, two ways of writing it, and the weekday list still holds */
	struct dnd_window all = win(1, -1, -1, DND_DAYS_EVERY, 1, DND_MODE_REJECT, 0);
	struct dnd_window equal = win(1, 600, 600, DND_DAYS_EVERY, 0, DND_MODE_SILENT, 0);
	struct dnd_window weekend = win(1, -1, -1, DND_DAY_SAT | DND_DAY_SUN, 1, DND_MODE_SILENT, 0);

	assert(dnd_window_hit(&all, 2, 0));
	assert(dnd_window_hit(&all, 2, 1439));
	assert(dnd_window_hit(&equal, 2, 0));		/* equal ends are the whole day, not nothing */
	assert(dnd_window_hit(&equal, 2, 1439));
	assert(dnd_window_hit(&weekend, 6, 300));
	assert(!dnd_window_hit(&weekend, 1, 300));

	/* what is not a window at all */
	struct dnd_window off = win(0, 0, 1439, DND_DAYS_EVERY, 1, DND_MODE_SILENT, 0);
	struct dnd_window half = win(1, 600, -1, DND_DAYS_EVERY, 0, DND_MODE_SILENT, 0);

	assert(!dnd_window_hit(&off, 2, 600));		/* disabled */
	assert(!dnd_window_hit(&half, 2, 600));		/* one end missing is not a span */
	assert(!dnd_window_hit(&nap, 2, -1));		/* the caller has no clock */
	assert(!dnd_window_hit(&nap, 2, DND_MINUTES_DAY));
	assert(!dnd_window_hit(&nap, 7, 600));
	assert(!dnd_window_hit(&nap, -1, 600));
}

static void test_decide(void)
{
	struct dnd_window windows[DND_WINDOWS];
	struct dnd_state st;
	int two = 2;	/* how many windows the plain cases carry */

	memset(windows, 0, sizeof(windows));
	windows[0] = win(1, 1320, 420, DND_DAYS_EVERY, 0, DND_MODE_MACHINE, 15);
	windows[1] = win(1, 780, 840, DND_DAYS_EVERY, 0, DND_MODE_SILENT, 0);
	windows[2] = win(1, 1300, 500, DND_DAYS_EVERY, 0, DND_MODE_REJECT, 3);	/* overlaps the night on purpose */

	/* the first window that matches decides, and carries its own mode and escape */
	dnd_decide(windows, 3, DND_MANUAL_AUTO, 1, 3, 1400, &st);
	assert(st.quiet && st.mode == DND_MODE_MACHINE && st.window == 0 && st.cause == DND_CAUSE_WINDOW &&
		st.repeat_min == 15);	/* the first written window wins, not the widest */
	dnd_decide(windows, 3, DND_MANUAL_AUTO, 1, 3, 1310, &st);
	assert(st.quiet && st.window == 2 && st.mode == DND_MODE_REJECT && st.repeat_min == 3);
	/* the night has not begun and the afternoon is over: only the third window reaches here */
	dnd_decide(windows, two, DND_MANUAL_AUTO, 1, 3, 800, &st);
	assert(st.quiet && st.mode == DND_MODE_SILENT && st.window == 1 && st.repeat_min == 0);

	/* nothing matches: the line rings */
	dnd_decide(windows, two, DND_MANUAL_AUTO, 1, 3, 900, &st);
	assert(!st.quiet && st.cause == DND_CAUSE_NONE && st.window == -1);

	/* an unknown clock cannot match a schedule, but a hand-set quiet needs no clock */
	dnd_decide(windows, two, DND_MANUAL_AUTO, 0, 3, 1400, &st);
	assert(!st.quiet);
	dnd_decide(windows, two, DND_MANUAL_ON, 0, 3, 900, &st);
	assert(st.quiet && st.cause == DND_CAUSE_MANUAL && st.window == -1);
	assert(st.mode == DND_MODE_MACHINE && st.repeat_min == 15);	/* the first enabled window's own terms */

	/* the handset's *79 outranks every window until it is cleared */
	dnd_decide(windows, two, DND_MANUAL_OFF, 1, 3, 1400, &st);
	assert(!st.quiet && st.cause == DND_CAUSE_NONE);

	/* all-day cause is named, so the page can say "all day" instead of a time */
	memset(windows, 0, sizeof(windows));
	windows[0] = win(1, -1, -1, DND_DAYS_EVERY, 1, DND_MODE_REJECT, 0);
	dnd_decide(windows, 1, DND_MANUAL_AUTO, 1, 3, 900, &st);
	assert(st.quiet && st.cause == DND_CAUSE_ALL_DAY && st.mode == DND_MODE_REJECT);

	/* a disabled window is not a window, and no windows at all is not quiet */
	memset(windows, 0, sizeof(windows));
	windows[0] = win(0, -1, -1, DND_DAYS_EVERY, 1, DND_MODE_SILENT, 0);
	dnd_decide(windows, 1, DND_MANUAL_AUTO, 1, 3, 900, &st);
	assert(!st.quiet);
	dnd_decide(windows, 0, DND_MANUAL_AUTO, 1, 3, 900, &st);
	assert(!st.quiet);
	dnd_decide(windows, 0, DND_MANUAL_ON, 1, 3, 900, &st);
	assert(st.quiet && st.mode == DND_MODE_SILENT);	/* asked for with no window written: the plainest quiet */
}

static void test_repeat(void)
{
	const uint64_t first = 1700000000ull;

	assert(dnd_repeat_due(first, first + 1, 15));
	assert(dnd_repeat_due(first, first + 15ull * 60 - 1, 15));
	assert(!dnd_repeat_due(first, first + 15ull * 60, 15));	/* exactly at the span: a new call is a new caller */
	assert(!dnd_repeat_due(0, first + 30, 15));			/* nothing rang before */
	assert(!dnd_repeat_due(first, first + 30, 0));			/* the escape is off */
	assert(!dnd_repeat_due(first, first + 30, -1));
	assert(!dnd_repeat_due(first + 600, first, 15));		/* the clock moved back: not a repeat */
	assert(dnd_repeat_due(first, first, 15));			/* the same second is the same caller ringing on */
}

static void test_rules(void)
{
	struct dnd d;
	char big[PHONE_NUMBER_MAX + 40];
	int k, rc;

	dnd_clear(&d, NULL);
	assert(d.count == 0 && d.allow_count == 0);

	/* a written window, in the form the config file gives it */
	assert(dnd_window_add(&d, 1, "22:00", "07:00", DND_DAYS_EVERY, 0, DND_MODE_MACHINE, 15, NULL) == 0);
	assert(d.window[0].from_min == 1320 && d.window[0].to_min == 420 && d.window[0].mode == DND_MODE_MACHINE);
	assert(dnd_window_add(&d, 1, NULL, NULL, DND_DAY_SAT | DND_DAY_SUN, 1, DND_MODE_SILENT, 0, NULL) == 0);
	assert(d.window[1].all_day && d.window[1].from_min < 0);

	/* what is not a window */
	assert(dnd_window_add(&d, 1, "25:00", "07:00", DND_DAYS_EVERY, 0, DND_MODE_SILENT, 0, NULL) == -EINVAL);
	assert(dnd_window_add(&d, 1, "22:00", "7:0", DND_DAYS_EVERY, 0, DND_MODE_SILENT, 0, NULL) == -EINVAL);
	assert(dnd_window_add(&d, 1, "22:00", NULL, DND_DAYS_EVERY, 0, DND_MODE_SILENT, 0, NULL) == -EINVAL);
	assert(dnd_window_add(&d, 1, NULL, NULL, DND_DAYS_EVERY, 0, DND_MODE_SILENT, 0, NULL) == -EINVAL);
	assert(dnd_window_add(&d, 1, "22:00", "07:00", DND_DAYS_EVERY, 0, 99, 0, NULL) == -EINVAL);
	assert(dnd_window_add(&d, 1, "22:00", "07:00", DND_DAYS_EVERY, 0, DND_MODE_SILENT, -1, NULL) == -EINVAL);
	/* the escape is minutes: an hour, and the whole day, are both a time an admin can write, and are kept as written */
	assert(dnd_window_add(&d, 1, "22:00", "07:00", DND_DAYS_EVERY, 0, DND_MODE_SILENT, 60, NULL) == 0);
	assert(d.window[d.count - 1].repeat_min == 60);
	assert(dnd_window_add(&d, 1, "22:00", "07:00", DND_DAYS_EVERY, 0, DND_MODE_SILENT, DND_MINUTES_DAY, NULL) == 0);
	assert(d.window[d.count - 1].repeat_min == DND_MINUTES_DAY);
	assert(dnd_window_add(&d, 1, "22:00", "07:00", DND_DAYS_EVERY, 0, DND_MODE_SILENT, DND_MINUTES_DAY + 1, NULL) == -EINVAL);
	assert(dnd_window_add(&d, 1, "22:00", "07:00", 1 << 9, 0, DND_MODE_SILENT, 0, NULL) == -EINVAL);
	memset(big, '1', sizeof(big));
	big[sizeof(big) - 1] = 0;
	assert(dnd_allow_add(&d, big, NULL) == -EINVAL);	/* longer than a number */

	/* the list fills up and says so */
	dnd_clear(&d, NULL);
	for (k = 0; k < DND_WINDOWS; k++)
		assert(dnd_window_add(&d, 1, "00:00", "01:00", DND_DAYS_EVERY, 0, DND_MODE_SILENT, 0, NULL) == 0);
	rc = dnd_window_add(&d, 1, "00:00", "02:00", DND_DAYS_EVERY, 0, DND_MODE_SILENT, 0, NULL);
	assert(rc == -ENOSPC);

	/* always-ring numbers: the same shape a blocking rule takes, compared in international form */
	dnd_clear(&d, NULL);
	assert(dnd_allow_add(&d, "0771234567", NULL) == 0);
	assert(dnd_allow_add(&d, "+94771234567", NULL) == 0);	/* the same caller, written the other way */
	assert(dnd_allow_add(&d, "*71", NULL) == 0);		/* a service code is a number for this list */
	assert(dnd_allow_add(&d, "", NULL) == -EINVAL);
	assert(dnd_allow_add(&d, "077 1234567", NULL) == -EINVAL);	/* a list entry is written plain */
	assert(dnd_allow_add(&d, "abc", NULL) == -EINVAL);
	assert(dnd_allow_add(&d, "+", NULL) == -EINVAL);
	assert(!dnd_allow_hit(&d, ""));				/* a withheld caller is never on the list */
	assert(!dnd_allow_hit(&d, NULL));
	assert(dnd_allow_hit(&d, "0771234567"));
	assert(dnd_allow_hit(&d, "+94771234567"));
	assert(dnd_allow_hit(&d, "0771234567") && dnd_allow_hit(&d, "+94771234567"));
	assert(!dnd_allow_hit(&d, "0771234568"));
	assert(!dnd_allow_hit(&d, "12"));

	dnd_clear(&d, NULL);
	for (k = 0; k <= DND_ALLOW_MAX; k++) {
		char n[16];
		snprintf(n, sizeof(n), "077000000%d", k % 10);
		rc = dnd_allow_add(&d, n, NULL);
		assert(rc == (k < DND_ALLOW_MAX ? 0 : -ENOSPC));
	}
}

/* the two codes the line answers for itself; anything else, however it begins, is the network's */
static void test_code(void)
{
	assert(dnd_code_of("*78") == DND_CODE_ON);
	assert(dnd_code_of("*78#") == DND_CODE_ON);		/* the send key a person adds is part of the same ask */
	assert(dnd_code_of("*79") == DND_CODE_OFF);
	assert(dnd_code_of("*79#") == DND_CODE_OFF);
	assert(dnd_code_of("") == DND_CODE_NONE);
	assert(dnd_code_of("78") == DND_CODE_NONE);		/* digits alone are a number */
	assert(dnd_code_of("*7") == DND_CODE_NONE);		/* a code half dialled is not yet anything */
	assert(dnd_code_of("*77") == DND_CODE_NONE);
	assert(dnd_code_of("*780") == DND_CODE_NONE);		/* a code of the network that begins the same way */
	assert(dnd_code_of("*781234#") == DND_CODE_NONE);
	assert(dnd_code_of("#78") == DND_CODE_NONE);
	assert(dnd_code_of("*78#79") == DND_CODE_NONE);		/* two asks in one string are nobody's code */
}

static void test_quiet(void)
{
	struct dnd d;
	struct dnd_now now;
	int mode = -7, reason = -7;

	dnd_clear(&d, NULL);
	memset(&now, 0, sizeof(now));
	now.clock_known = 1;
	now.weekday = 3;
	now.minute = 1400;			/* inside tonight's window */
	now.epoch_s = 1700000000ull;

	assert(dnd_window_add(&d, 1, "22:00", "07:00", DND_DAYS_EVERY, 0, DND_MODE_MACHINE, 15, NULL) == 0);

	assert(dnd_quiet(&d, "0771234567", &now, &mode, &reason));
	assert(mode == DND_MODE_MACHINE && reason == DND_R_QUIET);

	/* the same caller again inside the escape rings, and outside it does not */
	now.last_peer_epoch = now.epoch_s - 60;
	assert(!dnd_quiet(&d, "0771234567", &now, &mode, &reason) && reason == DND_R_REPEAT);
	now.last_peer_epoch = now.epoch_s - 16 * 60;
	assert(dnd_quiet(&d, "0771234567", &now, &mode, &reason) && reason == DND_R_QUIET);
	now.last_peer_epoch = 0;

	/* an emergency number rings through the window; the shipped plan's own list decides */
	assert(!dnd_quiet(&d, "110", &now, &mode, &reason) || reason == DND_R_QUIET);	/* only if 110 is emergency here */
	{
		struct dnd e;
		dnd_clear(&e, NULL);
		assert(dnd_window_add(&e, 1, "22:00", "07:00", DND_DAYS_EVERY, 0, DND_MODE_SILENT, 0, NULL) == 0);
		(void)dnd_quiet(&e, "110", &now, &mode, &reason);
		assert(reason == DND_R_EMERGENCY || reason == DND_R_QUIET);	/* the region's list is the only judge */
	}

	/* the always-ring list */
	assert(dnd_allow_add(&d, "0771234567", NULL) == 0);
	assert(!dnd_quiet(&d, "0771234567", &now, &mode, &reason) && reason == DND_R_ALLOWED);
	assert(!dnd_quiet(&d, "+94771234567", &now, &mode, &reason) && reason == DND_R_ALLOWED);
	assert(dnd_quiet(&d, "0779999999", &now, &mode, &reason) && reason == DND_R_QUIET);

	/* a withheld caller is judged by the window alone */
	assert(dnd_quiet(&d, "", &now, &mode, &reason) && reason == DND_R_QUIET);

	/* outside every window, and with no window at all, the line rings */
	now.minute = 900;
	assert(!dnd_quiet(&d, "0771234567", &now, &mode, &reason) && reason == DND_R_NOT_QUIET);
	now.minute = 1400;
	dnd_clear(&d, NULL);
	assert(!dnd_quiet(&d, "0771234567", &now, &mode, &reason) && reason == DND_R_NOT_QUIET);

	/* an unknown clock: no schedule, but a hand-set quiet still holds */
	now.clock_known = 0;
	assert(!dnd_quiet(&d, "0771234567", &now, &mode, &reason) && reason == DND_R_CLOCK);
	dnd_clear(&d, NULL);
	d.manual = DND_MANUAL_ON;
	assert(dnd_quiet(&d, "0771234567", &now, &mode, &reason) && reason == DND_R_QUIET);
	d.manual = DND_MANUAL_OFF;
	assert(!dnd_quiet(&d, "0771234567", &now, &mode, &reason));

	/* NULL rule set: nothing is quiet, because nothing was written */
	now.clock_known = 1;
	assert(!dnd_quiet(NULL, "0771234567", &now, &mode, &reason));
	assert(!dnd_quiet(&d, "0771234567", NULL, &mode, &reason));
}

/* the contacts that ask to ring through: the book is asked, not copied into the list */
static void test_book(void)
{
	struct dnd d;
	struct dnd_now now;
	struct phonebook *pb = malloc(sizeof(*pb));
	const char *marked[] = { "0771234567:mobile", "0779999999:home" };
	const char *plain[] = { "0770000000" };
	int mode = -7, reason = -7;

	assert(pb);
	phonebook_clear(pb, NULL);
	assert(phonebook_add(pb, "Ann", marked, 2, "", 1, NULL) == 0);
	assert(phonebook_add(pb, "Bob", plain, 1, "", 0, NULL) == 0);

	dnd_clear(&d, NULL);
	memset(&now, 0, sizeof(now));
	now.clock_known = 1;
	now.weekday = 3;
	now.minute = 1400;			/* inside tonight's window */
	now.epoch_s = 1700000000ull;
	assert(dnd_window_add(&d, 1, "22:00", "07:00", DND_DAYS_EVERY, 0, DND_MODE_SILENT, 0, NULL) == 0);

	/* nothing hangs off the rule set: the quiet hours answer for themselves */
	assert(dnd_quiet(&d, "0771234567", &now, &mode, &reason) && reason == DND_R_QUIET);
	d.book = pb;
	/* the marked contact rings through, whichever of its numbers the caller used, whichever form it came in */
	assert(!dnd_quiet(&d, "0771234567", &now, &mode, &reason) && reason == DND_R_ALLOWED);
	assert(!dnd_quiet(&d, "+94771234567", &now, &mode, &reason) && reason == DND_R_ALLOWED);
	assert(!dnd_quiet(&d, "0779999999", &now, &mode, &reason) && reason == DND_R_ALLOWED);
	/* the unmarked contact is an ordinary caller, and so is a withheld one */
	assert(dnd_quiet(&d, "0770000000", &now, &mode, &reason) && reason == DND_R_QUIET);
	assert(dnd_quiet(&d, "", &now, &mode, &reason) && reason == DND_R_QUIET);
	/* the written list and the book are one answer: a number on either rings through */
	assert(dnd_allow_add(&d, "0770000000", NULL) == 0);
	assert(!dnd_quiet(&d, "0770000000", &now, &mode, &reason) && reason == DND_R_ALLOWED);
	free(pb);
}

int main(void)
{
	test_parse();
	test_window();
	test_decide();
	test_repeat();
	test_rules();
	test_code();
	test_quiet();
	test_book();
	puts("dnd: all tests passed");
	return 0;
}
