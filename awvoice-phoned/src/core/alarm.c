// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <string.h>

#include "alarm.h"

void alarms_clear(struct alarms *a)
{
	memset(a, 0, sizeof(*a));
}

static int refuse(FILE *log, int err, const char *why)
{
	if (log)
		fprintf(log, "an alarm was not taken: %s\n", why);
	return err;
}

static int two_digits(const char *s, unsigned int *out)
{
	if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9')
		return 0;
	*out = (unsigned int)((s[0] - '0') * 10 + (s[1] - '0'));
	return 1;
}

int alarms_add(struct alarms *a, const char *label, const char *time, const char *const *days, unsigned int ndays, const char *enabled, FILE *log)
{
	struct alarm_entry e;
	unsigned int k;

	memset(&e, 0, sizeof(e));
	if (label) {
		if (strlen(label) > ALARM_LABEL_MAX)
			return refuse(log, -EINVAL, "the label is too long");
		snprintf(e.label, sizeof(e.label), "%s", label);
	}
	if (!time || strlen(time) != 5 || time[2] != ':' || !two_digits(time, &e.hour) || !two_digits(time + 3, &e.minute) || e.hour > 23 || e.minute > 59)
		return refuse(log, -EINVAL, "the time is not HH:MM");
	if (!ndays)
		return refuse(log, -EINVAL, "no day is chosen");
	for (k = 0; k < ndays; k++) {
		if (!days[k] || days[k][0] < '1' || days[k][0] > '7' || days[k][1])
			return refuse(log, -EINVAL, "a day is not 1 to 7");
		e.days |= 1u << (days[k][0] - '1');
	}
	if (enabled && strcmp(enabled, "0") && strcmp(enabled, "1"))
		return refuse(log, -EINVAL, "enabled is not 0 or 1");
	e.enabled = !enabled || !strcmp(enabled, "1");
	if (a->n >= ALARM_MAX)
		return refuse(log, -ENOSPC, "too many alarms");
	a->e[a->n++] = e;
	return 0;
}

int alarms_due(const struct alarms *a, const struct tm *local)
{
	const unsigned int day = local->tm_wday == 0 ? 7 : (unsigned int)local->tm_wday;	/* tm_wday: Sunday is 0 */
	unsigned int k;

	for (k = 0; k < a->n; k++)
		if (a->e[k].enabled && (a->e[k].days & (1u << (day - 1))) && a->e[k].hour == (unsigned int)local->tm_hour &&
		    a->e[k].minute == (unsigned int)local->tm_min)
			return 1;
	return 0;
}

enum alarm_poll alarms_poll(const struct alarms *a, struct alarm_watch *w, const struct tm *local)
{
	unsigned long long minute;

	if (!local) {
		const int fresh = !w->clock_bad;

		w->clock_bad = 1;
		return fresh ? ALARM_CLOCK_NEW_BAD : ALARM_QUIET;
	}
	w->clock_bad = 0;
	if (!alarms_due(a, local))
		return ALARM_QUIET;
	minute = ((unsigned long long)local->tm_year * 366 + (unsigned int)local->tm_yday) * 1440 +
		 (unsigned int)local->tm_hour * 60 + (unsigned int)local->tm_min + 1;
	if (minute == w->minute)
		return ALARM_QUIET;
	w->minute = minute;
	return ALARM_RING;
}
