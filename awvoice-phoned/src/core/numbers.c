// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "numbers.h"

/*
 * The numbering plans. Country codes and international prefixes are ITU-T E.164 data, the trunk prefix is what a
 * national call needs in that country, and the three-digit codes are the E.212 mobile country codes the networks of
 * that country report -- the line asks the modem which network it is on and finds its plan without being told.
 *
 * Only what is certain is written down: the prefixes are the ones each country's own plan lists, and a country whose
 * trunk prefix is not settled is left out rather than guessed - a row that picks the wrong plan is worse than a
 * missing one, and a country left out simply keeps the region the settings name. Several countries offer more than
 * one carrier-selection code for dialling abroad, so the prefix kept here is the one the plan itself names, not an
 * arbitrary member of that set. The national significant number is known for LK alone, so no other row turns a bare
 * number of that length into a home number (number_from_network); that inference is the one place where a wrong guess
 * would silently rewrite a caller.
 *
 * LK: country code 94, trunk prefix 0, international prefix 00. The national emergency numbers (Sri Lanka): 119
 * (police) and 1990 (ambulance and fire).
 */
static const char *const lk_emergency[] = { "119", "1990", NULL };
static const struct number_region regions[] = {
	{ "LK", "94", "0", "00", lk_emergency, 9, "413" },
	{ "IN", "91", "0", "00", NULL, 0, "404,405,406" },
	{ "PK", "92", "0", "00", NULL, 0, "410" },
	{ "BD", "880", "0", "00", NULL, 0, "470" },
	{ "NP", "977", "0", "00", NULL, 0, "429" },
	{ "MY", "60", "0", "00", NULL, 0, "502" },
	{ "SG", "65", "", "001", NULL, 0, "525" },
	{ "AU", "61", "0", "0011", NULL, 0, "505" },
	{ "NZ", "64", "0", "00", NULL, 0, "530" },
	{ "CN", "86", "0", "00", NULL, 0, "460" },
	{ "GB", "44", "0", "00", NULL, 0, "234,235" },
	{ "IE", "353", "0", "00", NULL, 0, "272" },
	{ "FR", "33", "0", "00", NULL, 0, "208" },
	{ "DE", "49", "0", "00", NULL, 0, "262" },
	{ "AT", "43", "0", "00", NULL, 0, "232" },
	{ "CH", "41", "0", "00", NULL, 0, "228" },
	{ "NL", "31", "0", "00", NULL, 0, "204" },
	{ "BE", "32", "0", "00", NULL, 0, "206" },
	{ "LU", "352", "", "00", NULL, 0, "270" },
	{ "IT", "39", "", "00", NULL, 0, "222" },
	{ "ES", "34", "", "00", NULL, 0, "214" },
	{ "PT", "351", "", "00", NULL, 0, "268" },
	{ "SE", "46", "0", "00", NULL, 0, "240" },
	{ "NO", "47", "", "00", NULL, 0, "242" },
	{ "FI", "358", "0", "00", NULL, 0, "244" },
	{ "DK", "45", "", "00", NULL, 0, "238" },
	{ "PL", "48", "", "00", NULL, 0, "260" },
	{ "CZ", "420", "", "00", NULL, 0, "230" },
	{ "SK", "421", "0", "00", NULL, 0, "231" },
	{ "HU", "36", "06", "00", NULL, 0, "216" },
	{ "RO", "40", "0", "00", NULL, 0, "226" },
	{ "UA", "380", "0", "00", NULL, 0, "255" },
	{ "RU", "7", "8", "810", NULL, 0, "250" },
	{ "TR", "90", "0", "00", NULL, 0, "286" },
	{ "US", "1", "1", "011", NULL, 0, "310,311,312,313,314,315,316" },
	{ "CA", "1", "1", "011", NULL, 0, "302" },
	{ "MX", "52", "", "00", NULL, 0, "334" },
	{ "ZA", "27", "0", "00", NULL, 0, "655" },
	{ "AE", "971", "0", "00", NULL, 0, "424" },
	{ "SA", "966", "0", "00", NULL, 0, "420" },
	{ "QA", "974", "", "00", NULL, 0, "427" },
	{ "KW", "965", "", "00", NULL, 0, "419" },
	{ "OM", "968", "", "00", NULL, 0, "422" },
};

/* the numbers every region has (3GPP TS 22.101: 112 on GSM/UMTS/LTE, 911 as the second form) */
static const char *const universal[] = { "112", "911", NULL };

/* one entry of the comma separated mobile country code list, matched as a whole entry */
static int holds_mcc(const char *list, const char *mcc)
{
	const char *p = list;
	size_t n = strlen(mcc);

	while (p) {
		const char *end = strchr(p, ',');
		size_t len = end ? (size_t)(end - p) : strlen(p);

		if (len == n && !strncmp(p, mcc, n))
			return 1;
		p = end ? end + 1 : NULL;
	}
	return 0;
}

const struct number_region *number_region_find(const char *name)
{
	size_t k;

	for (k = 0; name && k < sizeof(regions) / sizeof(regions[0]); k++)
		if (!strcmp(name, regions[k].name))
			return &regions[k];
	return NULL;
}

const struct number_region *number_region_by_mcc(const char *mcc)
{
	size_t k;

	if (!mcc || strlen(mcc) != 3)
		return NULL;
	for (k = 0; k < 3; k++)
		if (mcc[k] < '0' || mcc[k] > '9')
			return NULL;
	for (k = 0; k < sizeof(regions) / sizeof(regions[0]); k++)
		if (regions[k].mcc && holds_mcc(regions[k].mcc, mcc))
			return &regions[k];
	return NULL;
}

const struct number_region *number_region_at(size_t k)
{
	return k < sizeof(regions) / sizeof(regions[0]) ? &regions[k] : NULL;
}

const struct number_region *number_region_default(void)
{
	return &regions[0];
}

static int is_service_char(char ch)
{
	return ch == '*' || ch == '#';
}

int number_normalise(const char *in, const struct number_region *region, char *out, size_t cap)
{
	char flat[64];
	size_t n = 0, at;
	int service = 0;

	if (!in || !out || !cap)
		return -EINVAL;
	if (!region)
		region = number_region_default();
	for (at = 0; in[at]; at++) {
		char ch = in[at];

		if (ch == ' ' || ch == '-')
			continue;
		if (ch == '+') {
			if (n)
				return -EINVAL;
		} else if (is_service_char(ch)) {
			service = 1;
		} else if (ch < '0' || ch > '9') {
			return -EINVAL;
		}
		if (n + 1 >= sizeof(flat))
			return -ENAMETOOLONG;
		flat[n++] = ch;
	}
	flat[n] = 0;
	if (!n || (n == 1 && flat[0] == '+'))
		return -EINVAL;

	if (!service && flat[0] != '+') {
		size_t ip = strlen(region->intl_prefix), tp = strlen(region->trunk_prefix);
		const char *code = NULL, *rest = NULL;

		/* an empty prefix matches anything as a prefix, so a plan without one is only ever compared when it has
		 * something to compare (the table now holds countries with no trunk prefix at all) */
		if (ip && !strncmp(flat, region->intl_prefix, ip)) {
			/* the international prefix alone (nothing after it) stays as written */
			code = "";
			rest = n > ip ? flat + ip : NULL;
		} else if (tp && n > tp && !strncmp(flat, region->trunk_prefix, tp)) {
			code = region->country_code;
			rest = flat + tp;
		}
		if (rest) {
			int w = snprintf(out, cap, "+%s%s", code, rest);

			return w < 0 || (size_t)w >= cap ? -ENAMETOOLONG : 0;
		}
		if (region->nsn_len && n == region->nsn_len) {
			/* the national significant number alone, as the network reports a caller (TS 24.008 type 129) and as a
			 * contact saved from such a record holds it: the home country's number */
			int w = snprintf(out, cap, "+%s%s", region->country_code, flat);

			return w < 0 || (size_t)w >= cap ? -ENAMETOOLONG : 0;
		}
	}
	if (n >= cap)
		return -ENAMETOOLONG;
	memcpy(out, flat, n + 1);
	return 0;
}

int number_same(const char *a, const char *b, const struct number_region *region)
{
	char na[64], nb[64];

	return !number_normalise(a, region, na, sizeof(na)) && !number_normalise(b, region, nb, sizeof(nb)) && !strcmp(na, nb);
}

int number_from_network(const char *in, const struct number_region *region, char *out, size_t cap)
{
	size_t n, tp;
	int w;

	if (!out || !cap)
		return -EINVAL;
	out[0] = 0;
	if (!in || !*in)
		return 0;
	if (!region)
		region = number_region_default();
	n = strlen(in);
	tp = strlen(region->trunk_prefix);
	if (in[0] == '+')
		return number_normalise(in, region, out, cap);
	for (w = 0; in[w]; w++)
		if (in[w] < '0' || in[w] > '9')
			break;
	if (in[w]) {
		/* not a digit string (a service code, say): kept as it came */
		if (n >= cap)
			return -ENAMETOOLONG;
		memcpy(out, in, n + 1);
		return 0;
	}
	if (region->nsn_len && n == region->nsn_len && !(tp && !strncmp(in, region->trunk_prefix, tp))) {
		w = snprintf(out, cap, "+%s%s", region->country_code, in);
		return w < 0 || (size_t)w >= cap ? -ENAMETOOLONG : 0;
	}
	if (tp && n > tp && !strncmp(in, region->trunk_prefix, tp))
		return number_normalise(in, region, out, cap);
	if (n >= cap)
		return -ENAMETOOLONG;
	memcpy(out, in, n + 1);
	return 0;
}

int number_national(const char *in, const struct number_region *region, char *out, size_t cap)
{
	size_t cc, n;
	int w;

	if (!out || !cap)
		return -EINVAL;
	out[0] = 0;
	if (!in || !*in)
		return 0;
	if (!region)
		region = number_region_default();
	n = strlen(in);
	if (in[0] != '+') {
		if (n >= cap)
			return -ENAMETOOLONG;
		memcpy(out, in, n + 1);
		return 0;
	}
	cc = strlen(region->country_code);
	if (!strncmp(in + 1, region->country_code, cc) && in[1 + cc])
		w = snprintf(out, cap, "%s%s", region->trunk_prefix, in + 1 + cc);
	else
		/* the dialling prefix stands where the plus would: a caller-id message carries digits alone (numbers.h) */
		w = snprintf(out, cap, "%s%s", region->intl_prefix, in + 1);
	return w < 0 || (size_t)w >= cap ? -ENAMETOOLONG : 0;
}

static int listed(const char *const *list, const char *text)
{
	for (; list && *list; list++)
		if (!strcmp(*list, text))
			return 1;
	return 0;
}

int number_is_emergency(const char *digits, const struct number_region *region)
{
	char flat[16];
	size_t n = 0, at;

	if (!digits)
		return 0;
	if (!region)
		region = number_region_default();
	for (at = 0; digits[at]; at++) {
		if (digits[at] == ' ' || digits[at] == '-')
			continue;
		if (n + 1 >= sizeof(flat))
			return 0;
		flat[n++] = digits[at];
	}
	flat[n] = 0;
	return listed(universal, flat) || listed(region->emergency, flat);
}
