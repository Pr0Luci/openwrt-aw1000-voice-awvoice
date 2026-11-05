// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The quiet hours the admin wrote: what is accepted into the rule set, and who rings through it.
   The window arithmetic itself is in dnd.h, where the host tests reach it. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "dnd.h"
#include "phonebook.h"	/* the contacts that ring through quiet hours are asked here, in the book's own numbers */

/* the same shape a blocking rule takes: digits, an optional leading +, and * or # for a service code */
static int allow_ok(const char *v)
{
	size_t n = strlen(v), k;

	if (!n || n > PHONE_NUMBER_MAX)
		return 0;
	for (k = 0; k < n; k++) {
		const char ch = v[k];

		if ((ch >= '0' && ch <= '9') || (k == 0 && ch == '+'))
			continue;
		if (ch == '*' || ch == '#')
			continue;
		return 0;
	}
	return !(n == 1 && v[0] == '+');
}

void dnd_clear(struct dnd *d, const struct number_region *region)
{
	memset(d, 0, sizeof(*d));
	d->region = region;
}

int dnd_window_add(struct dnd *d, int enabled, const char *from, const char *to, int days, int all_day,
	int mode, int repeat_min, FILE *log)
{
	struct dnd_window *w;
	int f = -1, t = -1;

	if (mode < DND_MODE_SILENT || mode > DND_MODE_REJECT) {
		if (log)
			fprintf(log, "a quiet-hours window was not taken: the mode is not silent, machine or reject\n");
		return -EINVAL;
	}
	/* the escape is measured in minutes, and a window is never longer than the day it belongs to */
	if (repeat_min < 0 || repeat_min > DND_MINUTES_DAY) {
		if (log)
			fprintf(log, "a quiet-hours window was not taken: the repeat time is not 0..1440 minutes\n");
		return -EINVAL;
	}
	if (days != DND_DAYS_EVERY && (days < 0 || days & ~0x7f)) {
		if (log)
			fprintf(log, "a quiet-hours window was not taken: the day list holds a name that is not a weekday\n");
		return -EINVAL;
	}
	if (from && *from) {
		if ((f = dnd_parse_minute(from)) < 0) {
			if (log)
				fprintf(log, "a quiet-hours window was not taken: its start is not HH:MM\n");
			return -EINVAL;
		}
	}
	if (to && *to) {
		if ((t = dnd_parse_minute(to)) < 0) {
			if (log)
				fprintf(log, "a quiet-hours window was not taken: its end is not HH:MM\n");
			return -EINVAL;
		}
	}
	/* a span needs both ends; without them the window is only all day when that is what was asked for */
	if (!all_day && (f < 0 || t < 0)) {
		if (log)
			fprintf(log, "a quiet-hours window was not taken: it has no span and is not marked all day\n");
		return -EINVAL;
	}
	if (d->count >= DND_WINDOWS) {
		if (log)
			fprintf(log, "a quiet-hours window was not taken: %d windows is the most the daemon keeps\n",
				DND_WINDOWS);
		return -ENOSPC;
	}
	w = &d->window[d->count++];
	w->enabled = enabled ? 1 : 0;
	w->from_min = f;
	w->to_min = t;
	w->days = days;
	w->all_day = all_day ? 1 : 0;
	w->mode = mode;
	w->repeat_min = repeat_min;
	return 0;
}

int dnd_allow_add(struct dnd *d, const char *value, FILE *log)
{
	if (!value || !allow_ok(value)) {
		if (log)
			fprintf(log, "a quiet-hours number that always rings was not taken: it is not a number\n");
		return -EINVAL;
	}
	if (d->allow_count >= DND_ALLOW_MAX) {
		if (log)
			fprintf(log, "a quiet-hours number that always rings was not taken: the list is full\n");
		return -ENOSPC;
	}
	snprintf(d->allow[d->allow_count++], PHONE_NUMBER_MAX + 1, "%s", value);
	return 0;
}

int dnd_allow_hit(const struct dnd *d, const char *number)
{
	unsigned int k;

	if (!number || !*number)
		return 0;			/* a withheld number cannot be on a list of numbers */
	for (k = 0; k < d->allow_count; k++)
		if (number_same(d->allow[k], number, d->region))
			return 1;
	return d->book ? phonebook_ring_through(d->book, number) : 0;
}
