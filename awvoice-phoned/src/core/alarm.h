/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONE_CORE_ALARM_H
#define AW_PHONE_CORE_ALARM_H

#include <stdio.h>
#include <time.h>

/*
 * Alarm calls (the page's U26): the phone rings at a set time on chosen days, without a caller. Pure data and the time test; the line
 * rings it (call.c) and the front end supplies the clock. Days are numbered 1 = Monday .. 7 = Sunday (the page's numbering).
 */
#define ALARM_MAX	20
#define ALARM_LABEL_MAX	32

struct alarm_entry {
	char label[ALARM_LABEL_MAX + 1];
	unsigned int hour, minute;
	unsigned int days;			/* bit (day - 1) set for each chosen day */
	int enabled;
};

struct alarms {
	struct alarm_entry e[ALARM_MAX];
	unsigned int n;
};

void alarms_clear(struct alarms *a);

/*
 * Add one alarm. `time` is exactly "HH:MM" (00:00..23:59); `days` are the UCI list values "1".."7" (at least one); `enabled` is "0" or "1"
 * (NULL = "1"); the label is free text up to ALARM_LABEL_MAX characters (NULL = ""). 0; -EINVAL, -ENOSPC. On refusal `log` gets one line that
 * names the reason and never the label.
 */
int alarms_add(struct alarms *a, const char *label, const char *time, const char *const *days, unsigned int ndays, const char *enabled, FILE *log);

/* 1 when an enabled alarm is set for this local time (to the minute) and weekday */
int alarms_due(const struct alarms *a, const struct tm *local);

/*
 * The watch over the clock: what the periodic look at the time found. It remembers the minute an alarm last rang in (one ring per minute, whatever
 * the number of alarms set for it) and whether the clock was already reported as untrusted (said once per stretch).
 */
struct alarm_watch {
	unsigned long long minute;	/* the last minute that rang, as a count from year 0 (never 0 once set) */
	int clock_bad;
};

enum alarm_poll { ALARM_QUIET, ALARM_RING, ALARM_CLOCK_NEW_BAD };

/* `local` is the trusted local time, or NULL when the clock cannot be trusted. ALARM_RING: an alarm is due in a minute that has not rung yet. */
enum alarm_poll alarms_poll(const struct alarms *a, struct alarm_watch *w, const struct tm *local);

#endif /* AW_PHONE_CORE_ALARM_H */
