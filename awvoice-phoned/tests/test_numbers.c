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
static const struct number_region xx = { "XX", "1", "1", "011", xx_emergency, 4, NULL };

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

	/* the shipped region, as the line really is: a foreign number keeps the dialling prefix, which a handset can dial
	 * back and which a caller-id message can carry (it has no plus), while a number of the country keeps its trunk */
	assert(!number_national("+447700900123", lk, out, sizeof(out)) && !strcmp(out, "00447700900123"));
	assert(!number_national("+94771234567", lk, out, sizeof(out)) && !strcmp(out, "0771234567"));
	assert(!number_national("+94", lk, out, sizeof(out)) && !strcmp(out, "0094"));
	assert(!number_national("0771234567", lk, out, sizeof(out)) && !strcmp(out, "0771234567"));
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
		const struct number_region r = { "XX", "1", "1", "011", own, 0, NULL };

		assert(number_is_emergency("999", &r) && number_is_emergency("112", &r) && !number_is_emergency("999", lk));
	}
}

/* the plan the network's own identity points at (ITU-T E.212): three digits in, a row out, nothing for anything else */
static void test_region_by_mcc(void)
{
	assert(number_region_by_mcc("413") == number_region_find("LK"));
	assert(number_region_by_mcc("234") == number_region_find("GB"));
	assert(number_region_by_mcc("235") == number_region_find("GB"));	/* a country that reports two codes */
	assert(number_region_by_mcc("310") == number_region_find("US"));
	assert(number_region_by_mcc("316") == number_region_find("US"));
	assert(number_region_by_mcc("302") == number_region_find("CA"));	/* the same country code, another row */
	assert(number_region_by_mcc("525") == number_region_find("SG"));
	/* anything that is not three digits answers nothing: a half-read identity must not choose a numbering plan */
	assert(number_region_by_mcc(NULL) == NULL);
	assert(number_region_by_mcc("") == NULL);
	assert(number_region_by_mcc("41") == NULL);
	assert(number_region_by_mcc("4130") == NULL);
	assert(number_region_by_mcc("41x") == NULL);
	assert(number_region_by_mcc("999") == NULL);		/* a code that exists somewhere, held by no row here */
}

/*
 * A row with an empty trunk prefix must not read every number as a national one: a prefix of no digits is a prefix of
 * everything. The table holds such plans, so the branch that looks for the trunk prefix has to have something to look
 * for, and the same is asked of the international prefix.
 */
static void test_plans_without_prefix(void)
{
	const struct number_region *it = number_region_find("IT"), *us = number_region_find("US");
	char out[32];

	assert(!number_normalise("0212345678", it, out, sizeof(out)) && !strcmp(out, "0212345678"));
	assert(!number_normalise("39021234", it, out, sizeof(out)) && !strcmp(out, "39021234"));
	/* the international prefix of the same plan still turns a dialled number into the comparison form */
	assert(!number_normalise("0039021234", it, out, sizeof(out)) && !strcmp(out, "+39021234"));
	/* the NANP plan dials its own country code as the trunk, and stands at 011 rather than 00 */
	assert(!number_normalise("12125550123", us, out, sizeof(out)) && !strcmp(out, "+12125550123"));
	assert(!number_national("+12125550123", us, out, sizeof(out)) && !strcmp(out, "12125550123"));
	assert(!number_national("+447700900123", us, out, sizeof(out)) && !strcmp(out, "011447700900123"));
	assert(!number_national("+39021234", it, out, sizeof(out)) && !strcmp(out, "021234"));
	/* a bare number is never guessed at where the length of the national number is not known: only LK says nine */
	assert(!number_from_network("0212345678", it, out, sizeof(out)) && !strcmp(out, "0212345678"));
	assert(!number_from_network("771234567", lk, out, sizeof(out)) && !strcmp(out, "+94771234567"));
	/* a plan written down with neither prefix: a bare digit string is kept exactly as it came, because a prefix of no
	 * digits is a prefix of everything and nothing may be read out of the front of the number */
	{
		const struct number_region bare = { "XX", "1", "", "", NULL, 0, NULL };

		assert(!number_normalise("0212345678", &bare, out, sizeof(out)) && !strcmp(out, "0212345678"));
		assert(!number_normalise("112", &bare, out, sizeof(out)) && !strcmp(out, "112"));
		assert(!number_normalise("+12345", &bare, out, sizeof(out)) && !strcmp(out, "+12345"));
	}
}

static int digits_only(const char *s)
{
	for (; s && *s; s++)
		if (*s < '0' || *s > '9')
			return 0;
	return 1;
}

/* one code of the first list also claimed by the second */
static int both_claim(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	for (; *a; ) {
		size_t len = strcspn(a, ",");
		const char *p = b;

		while (*p) {
			size_t plen = strcspn(p, ",");

			if (plen == len && !strncmp(a, p, len))
				return 1;
			p += plen;
			if (*p == ',')
				p++;
		}
		a += len;
		if (*a == ',')
			a++;
	}
	return 0;
}

/* every code of a list is three digits */
static int codes_ok(const char *list)
{
	if (!list)
		return 1;
	for (; *list; ) {
		size_t len = strcspn(list, ",");
		size_t k;

		if (len != 3)
			return 0;
		for (k = 0; k < 3; k++)
			if (list[k] < '0' || list[k] > '9')
				return 0;
		list += len;
		if (*list == ',')
			list++;
	}
	return 1;
}

/*
 * The table itself: a row that would pick the wrong plan is worse than a missing row, so every row is checked to agree
 * with its own lookups -- two uppercase letters for the name, digits for every prefix, three digits for every mobile
 * country code, no code claimed by two rows, and the first code of a row pointing back at that row.
 */
static void test_table_holds(void)
{
	const struct number_region *r;
	size_t k, j;

	for (k = 0; (r = number_region_at(k)); k++) {
		assert(r->name && strlen(r->name) == 2 && r->name[0] >= 'A' && r->name[0] <= 'Z' && r->name[1] >= 'A' && r->name[1] <= 'Z');
		assert(digits_only(r->country_code) && r->country_code[0]);
		assert(digits_only(r->trunk_prefix) && digits_only(r->intl_prefix) && r->intl_prefix[0]);
		assert(codes_ok(r->mcc));
		assert(number_region_find(r->name) == r);
		if (r->mcc) {
			char first[4];
			size_t len = strcspn(r->mcc, ",");

			assert(len == 3);
			memcpy(first, r->mcc, 3);
			first[3] = 0;
			assert(number_region_by_mcc(first) == r);
		}
		for (j = 0; j < k; j++) {
			const struct number_region *other = number_region_at(j);

			assert(strcmp(other->name, r->name));
			assert(!both_claim(other->mcc, r->mcc));
		}
	}
	assert(k > 10);					/* the walk ran to the end of a table that is not empty */
	assert(number_region_at(k) == NULL && number_region_at(0) == number_region_default());
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
	test_region_by_mcc();
	test_plans_without_prefix();
	test_table_holds();
	puts("numbers: all tests passed");
	return 0;
}
