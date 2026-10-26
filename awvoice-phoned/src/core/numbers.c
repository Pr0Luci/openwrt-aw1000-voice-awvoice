// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "numbers.h"

/*
 * LK: country code 94, trunk prefix 0, international prefix 00 (ITU-T E.164 numbering plan data).
 * The national emergency numbers (Sri Lanka): 119 (police) and 1990 (ambulance and fire).
 */
static const char *const lk_emergency[] = { "119", "1990", NULL };
static const struct number_region regions[] = {
	{ "LK", "94", "0", "00", lk_emergency, 9 },
};

/* the numbers every region has (3GPP TS 22.101: 112 on GSM/UMTS/LTE, 911 as the second form) */
static const char *const universal[] = { "112", "911", NULL };

const struct number_region *number_region_find(const char *name)
{
	size_t k;

	for (k = 0; name && k < sizeof(regions) / sizeof(regions[0]); k++)
		if (!strcmp(name, regions[k].name))
			return &regions[k];
	return NULL;
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

		if (!strncmp(flat, region->intl_prefix, ip)) {
			/* the international prefix alone (nothing after it) stays as written */
			code = "";
			rest = n > ip ? flat + ip : NULL;
		} else if (n > tp && !strncmp(flat, region->trunk_prefix, tp)) {
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
