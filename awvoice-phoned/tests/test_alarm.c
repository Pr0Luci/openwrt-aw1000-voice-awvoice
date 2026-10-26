// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* Alarm entries and the time test (pure): make -f Makefile.host test */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm.h"

static struct tm at(int wday, int hour, int min)
{
	struct tm t;

	memset(&t, 0, sizeof(t));
	t.tm_wday = wday;			/* 0 = Sunday */
	t.tm_hour = hour;
	t.tm_min = min;
	return t;
}

static void test_due(void)
{
	struct alarms *a = calloc(1, sizeof(*a));
	const char *wd[] = { "1", "2", "3", "4", "5" };
	const char *sun[] = { "7" };
	struct tm t;

	assert(a);
	assert(alarms_add(a, "Work", "06:30", wd, 5, "1", NULL) == 0);
	assert(alarms_add(a, "", "09:00", sun, 1, NULL, NULL) == 0);		/* no label, enabled by default */
	t = at(1, 6, 30);				/* Monday */
	assert(alarms_due(a, &t));
	t = at(5, 6, 30);				/* Friday */
	assert(alarms_due(a, &t));
	t = at(6, 6, 30);				/* Saturday: not chosen */
	assert(!alarms_due(a, &t));
	t = at(0, 6, 30);				/* Sunday: only the 09:00 one */
	assert(!alarms_due(a, &t));
	t = at(0, 9, 0);
	assert(alarms_due(a, &t));
	t = at(1, 6, 31);
	assert(!alarms_due(a, &t));
	t = at(1, 6, 29);
	assert(!alarms_due(a, &t));
	t = at(1, 7, 30);
	assert(!alarms_due(a, &t));
	free(a);
}

static void test_disabled_and_edges(void)
{
	struct alarms *a = calloc(1, sizeof(*a));
	const char *all[] = { "1", "2", "3", "4", "5", "6", "7" };
	struct tm t;
	int d;

	assert(a);
	assert(alarms_add(a, "off", "00:00", all, 7, "0", NULL) == 0);
	assert(alarms_add(a, "late", "23:59", all, 7, "1", NULL) == 0);
	for (d = 0; d < 7; d++) {
		t = at(d, 0, 0);
		assert(!alarms_due(a, &t));		/* disabled */
		t = at(d, 23, 59);
		assert(alarms_due(a, &t));		/* every day */
	}
	free(a);
}

static void test_poll(void)
{
	struct alarms *a = calloc(1, sizeof(*a));
	const char *day[] = { "1" };
	struct alarm_watch w = { 0, 0 };
	struct tm t = at(1, 6, 30), t2;

	assert(a && alarms_add(a, "x", "06:30", day, 1, "1", NULL) == 0);
	t.tm_year = 126;
	t.tm_yday = 40;
	assert(alarms_poll(a, &w, &t) == ALARM_RING);			/* the first look in the minute */
	assert(alarms_poll(a, &w, &t) == ALARM_QUIET);			/* the second look in the same minute */
	t2 = t;
	t2.tm_min = 31;
	assert(alarms_poll(a, &w, &t2) == ALARM_QUIET);
	assert(alarms_poll(a, &w, &t) == ALARM_QUIET);			/* a look back at that minute does not ring again */
	t.tm_yday = 41;							/* the same time of day on another day */
	t.tm_wday = 1;
	assert(alarms_poll(a, &w, &t) == ALARM_RING);
	t.tm_year = 127;							/* ... and in another year */
	assert(alarms_poll(a, &w, &t) == ALARM_RING);
	/* an untrusted clock is reported once; trust returns and a later look is quiet until the next due minute */
	assert(alarms_poll(a, &w, NULL) == ALARM_CLOCK_NEW_BAD);
	assert(alarms_poll(a, &w, NULL) == ALARM_QUIET);
	t2.tm_min = 31;
	assert(alarms_poll(a, &w, &t2) == ALARM_QUIET && !w.clock_bad);
	assert(alarms_poll(a, &w, NULL) == ALARM_CLOCK_NEW_BAD);
	free(a);
}

static void test_refusals(void)
{
	struct alarms *a = calloc(1, sizeof(*a));
	const char *one[] = { "1" };
	const char *bad_day[] = { "8" };
	const char *zero_day[] = { "0" };
	const char *long_day[] = { "12" };
	char *buf = NULL;
	size_t len = 0;
	FILE *log = open_memstream(&buf, &len);
	char label[40];
	unsigned int k;

	assert(a);
	assert(alarms_add(a, "x", "6:30", one, 1, "1", log) == -EINVAL);
	assert(alarms_add(a, "x", "24:00", one, 1, "1", log) == -EINVAL);
	assert(alarms_add(a, "x", "12:60", one, 1, "1", log) == -EINVAL);
	assert(alarms_add(a, "x", "12-30", one, 1, "1", log) == -EINVAL);
	assert(alarms_add(a, "x", "1a:30", one, 1, "1", log) == -EINVAL);
	assert(alarms_add(a, "x", "12:30 ", one, 1, "1", log) == -EINVAL);
	assert(alarms_add(a, "x", NULL, one, 1, "1", log) == -EINVAL);
	assert(alarms_add(a, "x", "12:30", one, 0, "1", log) == -EINVAL);
	assert(alarms_add(a, "x", "12:30", bad_day, 1, "1", log) == -EINVAL);
	assert(alarms_add(a, "x", "12:30", zero_day, 1, "1", log) == -EINVAL);
	assert(alarms_add(a, "x", "12:30", long_day, 1, "1", log) == -EINVAL);
	assert(alarms_add(a, "x", "12:30", one, 1, "yes", log) == -EINVAL);
	memset(label, 'l', 33);
	label[33] = 0;
	assert(alarms_add(a, label, "12:30", one, 1, "1", log) == -EINVAL);
	assert(a->n == 0);
	fflush(log);
	assert(buf && strstr(buf, "was not taken") && !strstr(buf, "lll"));
	fclose(log);
	free(buf);
	label[32] = 0;
	assert(alarms_add(a, label, "12:30", one, 1, "1", NULL) == 0);		/* 32 characters fit */
	for (k = 1; k < ALARM_MAX; k++)
		assert(alarms_add(a, "a", "12:30", one, 1, "1", NULL) == 0);
	assert(a->n == ALARM_MAX && alarms_add(a, "a", "12:30", one, 1, "1", NULL) == -ENOSPC);
	alarms_clear(a);
	assert(a->n == 0);
	free(a);
}

int main(void)
{
	test_due();
	test_disabled_and_edges();
	test_poll();
	test_refusals();
	puts("alarm: all tests passed");
	return 0;
}
