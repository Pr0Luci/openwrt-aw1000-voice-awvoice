// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* Number normalisation and the emergency list (pure): make -f Makefile.host test. The digit strings are made up. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "numbers.h"

static const struct number_region *lk;

static void norm(const char *in, const char *want)
{
	char out[64];

	assert(number_normalise(in, lk, out, sizeof(out)) == 0);
	assert(!strcmp(out, want));
}

static void test_normalise(void)
{
	char out[64];

	/* trunk prefix -> plus and the country code */
	norm("012345", "+9412345");
	norm("012 34 5", "+9412345");
	norm("012-34-5", "+9412345");
	/* international prefix -> plus */
	norm("009412345", "+9412345");
	norm("00 94 12 34 5", "+9412345");
	/* plus kept */
	norm("+9412345", "+9412345");
	norm("+94 12-34 5", "+9412345");
	norm("+4412345", "+4412345");
	/* other digit strings are unchanged: short codes, a number without a prefix */
	norm("1919", "1919");
	norm("112", "112");
	norm("1234567", "1234567");
	/* a prefix with nothing after it stays as written */
	norm("0", "0");
	norm("00", "00");
	/* service codes are never rewritten */
	norm("*123#", "*123#");
	norm("#31#012345", "#31#012345");
	norm("*31# 012-345", "*31#012345");
	norm("*0", "*0");
	norm("#", "#");
	/* NULL region = the default region */
	assert(number_normalise("012345", NULL, out, sizeof(out)) == 0 && !strcmp(out, "+9412345"));
}

static void test_normalise_refusals(void)
{
	char out[16];

	assert(number_normalise("", lk, out, sizeof(out)) == -EINVAL);
	assert(number_normalise(" - ", lk, out, sizeof(out)) == -EINVAL);
	assert(number_normalise("12a4", lk, out, sizeof(out)) == -EINVAL);
	assert(number_normalise("12+4", lk, out, sizeof(out)) == -EINVAL);
	assert(number_normalise("+", lk, out, sizeof(out)) == -EINVAL);
	assert(number_normalise("(011)1234", lk, out, sizeof(out)) == -EINVAL);
	assert(number_normalise(NULL, lk, out, sizeof(out)) == -EINVAL);
	assert(number_normalise("123", lk, NULL, 4) == -EINVAL);
	assert(number_normalise("123", lk, out, 0) == -EINVAL);
	/* the output size is respected, with the terminator and with the added country code */
	assert(number_normalise("1234567890", lk, out, 10) == -ENAMETOOLONG);
	assert(number_normalise("1234567890", lk, out, 11) == 0);
	assert(number_normalise("012345", lk, out, 8) == -ENAMETOOLONG);
	assert(number_normalise("012345", lk, out, 9) == 0 && !strcmp(out, "+9412345"));
	{
		char longer[200];

		memset(longer, '7', sizeof(longer) - 1);
		longer[sizeof(longer) - 1] = 0;
		assert(number_normalise(longer, lk, out, sizeof(out)) == -ENAMETOOLONG);
	}
}

static void test_same(void)
{
	/* the national, international and plus forms of one number are the same */
	assert(number_same("012345", "+9412345", lk));
	assert(number_same("009412345", "012345", lk));
	assert(number_same("012 34 5", "01-2345", lk));
	assert(number_same("1919", "1919", lk));
	/* a tail match is not a match: the whole number is compared */
	assert(!number_same("1234567", "012345", lk));
	assert(!number_same("012345", "012346", lk));
	assert(!number_same("012345", "+441234", lk));
	/* service codes are compared as written */
	assert(number_same("*123#", "*123#", lk));
	assert(!number_same("*123#", "123", lk));
	/* anything that is not a number is never the same, not even as itself */
	assert(!number_same("", "", lk));
	assert(!number_same("ab", "ab", lk));
	assert(!number_same(NULL, "1", lk));
	assert(number_same("012345", "+9412345", NULL));
}

static void test_emergency(void)
{
	assert(number_is_emergency("112", lk));
	assert(number_is_emergency("911", lk));
	assert(number_is_emergency("1 1 2", lk));
	assert(number_is_emergency("11-2", lk));
	assert(number_is_emergency("112", NULL));
	/* the LK list: police and ambulance */
	assert(number_is_emergency("119", lk));
	assert(number_is_emergency("1990", lk));
	assert(!number_is_emergency("1190", lk));
	assert(!number_is_emergency("199", lk));
	/* an exact match only: a longer string, a prefix, a service code */
	assert(!number_is_emergency("1120", lk));
	assert(!number_is_emergency("11", lk));
	assert(!number_is_emergency("0112", lk));
	assert(!number_is_emergency("+112", lk));
	assert(!number_is_emergency("*112#", lk));
	assert(!number_is_emergency("", lk));
	assert(!number_is_emergency(NULL, lk));
	assert(!number_is_emergency("1111111111111111111", lk));
	assert(!number_is_emergency("1919", lk));
}

/* the network's forms (the modem's call list), on a made-up region (country code 1, trunk prefix 1, national numbers of 4 digits):
 * the national significant number, the international one, a dialled form, short codes */
static const char *const xx_emergency[] = { "999", NULL };
static const struct number_region xx = { "XX", "1", "1", "011", xx_emergency, 4 };

static void test_from_network(void)
{
	char out[32];

	assert(lk->nsn_len == 9);								/* LK national numbers (ITU-T E.164) */
	assert(!number_from_network("2345", &xx, out, sizeof(out)) && !strcmp(out, "+12345"));	/* NSN, type 129 */
	assert(!number_from_network("+12345", &xx, out, sizeof(out)) && !strcmp(out, "+12345"));	/* type 145 with the plus */
	assert(!number_from_network("12345", &xx, out, sizeof(out)) && !strcmp(out, "+12345"));	/* as dialled, trunk prefix */
	assert(!number_from_network("77", &xx, out, sizeof(out)) && !strcmp(out, "77"));		/* a short code stays */
	assert(!number_from_network("999", &xx, out, sizeof(out)) && !strcmp(out, "999"));
	assert(!number_from_network("", &xx, out, sizeof(out)) && !out[0]);
	assert(!number_from_network(NULL, &xx, out, sizeof(out)) && !out[0]);
	assert(!number_from_network("*43#", &xx, out, sizeof(out)) && !strcmp(out, "*43#"));
	/* the normaliser itself knows the bare national number, so a contact saved in that form matches a record in any form */
	assert(!number_normalise("2345", &xx, out, sizeof(out)) && !strcmp(out, "+12345"));
	assert(number_same("2345", "12345", &xx) && number_same("2345", "+12345", &xx) && !number_same("345", "2345", &xx));
	assert(number_from_network("2345", &xx, out, 5) == -ENAMETOOLONG);
	/* the three forms of one number compare equal afterwards */
	{
		char a[32], b[32];

		assert(!number_from_network("2345", &xx, a, sizeof(a)) && !number_from_network("12345", &xx, b, sizeof(b)));
		assert(number_same(a, b, &xx) && number_same(a, "+1 23 45", &xx));
	}
}

/* the handset's form: the home country's numbers national, foreign ones with the international prefix, the rest as they are */
static void test_national(void)
{
	char out[32];

	assert(!number_national("+12345", &xx, out, sizeof(out)) && !strcmp(out, "12345"));
	assert(!number_national("+4499", &xx, out, sizeof(out)) && !strcmp(out, "0114499"));
	assert(!number_national("77", &xx, out, sizeof(out)) && !strcmp(out, "77"));
	assert(!number_national("", &xx, out, sizeof(out)) && !out[0]);
	assert(!number_national("+1", &xx, out, sizeof(out)) && !strcmp(out, "0111"));	/* a bare country code is not a national number */
	assert(number_national("+12345", &xx, out, 4) == -ENAMETOOLONG);
}

static void test_regions(void)
{
	assert(number_region_find("LK") == lk && lk == number_region_default());
	assert(number_region_find("lk") == NULL);
	assert(number_region_find("") == NULL);
	assert(number_region_find("LKA") == NULL);
	assert(number_region_find(NULL) == NULL);
	/* a region with its own list: its numbers are emergencies there, the universal ones stay */
	{
		static const char *const own[] = { "999", NULL };
		const struct number_region r = { "XX", "1", "1", "011", own, 0 };

		assert(number_is_emergency("999", &r) && number_is_emergency("112", &r) && !number_is_emergency("999", lk));
	}
}

int main(void)
{
	lk = number_region_find("LK");
	assert(lk);
	test_normalise();
	test_normalise_refusals();
	test_same();
	test_emergency();
	test_from_network();
	test_national();
	test_regions();
	puts("numbers: all tests passed");
	return 0;
}
