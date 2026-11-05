// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* tests/test_region_follow.c - which numbering plan the line uses now: host-only, plain asserts. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "region_follow.h"

/* a setting that names a plan is not overruled by the mast the modem is on */
static void test_named_plan_wins(void)
{
	assert(!strcmp(region_choice("LK", "404", "IN"), "LK"));
	assert(!strcmp(region_choice("IN", "", "LK"), "IN"));
	assert(!strcmp(region_choice("US", "413", "LK"), "US"));
	/* an empty or missing setting is the same as asking for the network's plan */
	assert(!strcmp(region_choice("", "404", NULL), "IN"));
	assert(!strcmp(region_choice(NULL, "404", "LK"), "IN"));
}

/* auto takes the plan the network's three digits point at */
static void test_auto_follows_the_network(void)
{
	assert(!strcmp(region_choice(REGION_AUTO, "413", NULL), "LK"));
	assert(!strcmp(region_choice(REGION_AUTO, "404", "LK"), "IN"));
	assert(!strcmp(region_choice(REGION_AUTO, "234", "IN"), "GB"));
	assert(!strcmp(region_choice(REGION_AUTO, "235", "GB"), "GB"));	/* the country's other code */
	assert(!strcmp(region_choice(REGION_AUTO, "310", "GB"), "US"));
	assert(!strcmp(region_choice(REGION_AUTO, "302", "US"), "CA"));	/* the same country code, the other country */
	assert(!strcmp(region_choice(REGION_AUTO, "505", "CA"), "AU"));
	assert(!strcmp(region_choice(REGION_AUTO, "655", "AU"), "ZA"));
}

/*
 * Nothing to go by: the plan in force stands. A lost signal, a network no row carries, and digits that are not a code
 * at all must not move the line onto another plan - and the answer is always a name the table holds.
 */
static void test_auto_with_nothing_to_read(void)
{
	assert(!strcmp(region_choice(REGION_AUTO, "", NULL), "LK"));		/* nothing in force yet: the shipped plan */
	assert(!strcmp(region_choice(REGION_AUTO, "", "IN"), "IN"));		/* the network went away: the plan stays */
	assert(!strcmp(region_choice(REGION_AUTO, "999", "IN"), "IN"));	/* a real code with no row */
	assert(!strcmp(region_choice(REGION_AUTO, "41", "GB"), "GB"));
	assert(!strcmp(region_choice(REGION_AUTO, "4130", "GB"), "GB"));
	assert(!strcmp(region_choice(REGION_AUTO, "4xx", "GB"), "GB"));
	assert(number_region_find(region_choice(REGION_AUTO, "9999", NULL)));
	assert(number_region_find(region_choice(REGION_AUTO, "", NULL)));
}

/* the walk the daemon makes: the line comes up, the network answers, the network changes, the subscriber overrides */
static void test_the_walk(void)
{
	const char *in_force = NULL;
	const char *want;

	want = region_choice(REGION_AUTO, "", in_force);			/* up before the first answer */
	assert(!strcmp(want, "LK") && region_changed(in_force, want));
	in_force = want;
	want = region_choice(REGION_AUTO, "413", in_force);			/* the home network reports itself */
	assert(!strcmp(want, "LK") && !region_changed(in_force, want));
	in_force = want;
	want = region_choice(REGION_AUTO, "404", in_force);			/* a SIM from another country */
	assert(!strcmp(want, "IN") && region_changed(in_force, want));
	in_force = want;
	want = region_choice(REGION_AUTO, "", in_force);			/* the signal is lost: nothing moves */
	assert(!strcmp(want, "IN") && !region_changed(in_force, want));
	in_force = want;
	want = region_choice("LK", "234", in_force);				/* the subscriber names a plan */
	assert(!strcmp(want, "LK") && region_changed(in_force, want));
	in_force = want;
	want = region_choice("LK", "404", in_force);				/* and is not overruled by the network */
	assert(!strcmp(want, "LK") && !region_changed(in_force, want));
}

static void test_changed(void)
{
	assert(region_changed(NULL, "LK"));
	assert(!region_changed("LK", "LK"));
	assert(region_changed("LK", "IN"));
	assert(region_changed("LK", NULL));					/* nothing to hold: act on it */
}

int main(void)
{
	test_named_plan_wins();
	test_auto_follows_the_network();
	test_auto_with_nothing_to_read();
	test_the_walk();
	test_changed();
	puts("region_follow: all tests passed");
	return 0;
}
