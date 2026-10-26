/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONE_CORE_NUMBERS_H
#define AW_PHONE_CORE_NUMBERS_H

#include <stddef.h>

/*
 * Phone numbers: one form for comparing them, and the emergency list. Pure functions (no I/O, no clock), host-tested.
 *
 * The region says how a number written for the home country is turned into the international form: the country code,
 * the trunk prefix a national number starts with, and the international call prefix. `*` and `#` strings are service
 * codes (3GPP TS 22.030) and are never rewritten, apart from dropping spaces and dashes.
 */

struct number_region {
	const char *name;		/* the UCI value of `region` ("LK") */
	const char *country_code;	/* digits, without the plus */
	const char *trunk_prefix;	/* the prefix of a national number */
	const char *intl_prefix;	/* the prefix that stands for the plus */
	const char *const *emergency;	/* the region's own emergency numbers, NULL ends the list (may be NULL) */
	size_t nsn_len;			/* digits of a national significant number (ITU-T E.164; LK 9): what the network sends without the trunk prefix */
};

/* The region called `name` (exact, upper case), or NULL. */
const struct number_region *number_region_find(const char *name);

/* The region used when none is set: LK. */
const struct number_region *number_region_default(void);

/*
 * The comparison form of `in`: spaces and dashes dropped; a leading `+` kept; the international prefix becomes `+`;
 * the trunk prefix becomes `+<country code>`; a bare national significant number (nsn_len digits, as the network
 * reports a caller) becomes `+<country code><digits>`; every other digit string is kept. Strings with `*` or `#` are service
 * codes and are returned as written (spaces and dashes dropped). `region` NULL = the default region.
 * 0, -EINVAL (empty or only a plus, a character a number cannot hold, a `+` that is not first), -ENAMETOOLONG (does not fit `cap`).
 */
int number_normalise(const char *in, const struct number_region *region, char *out, size_t cap);

/* 1 when both are numbers that are the same after normalisation; 0 otherwise (also when either is not a number). */
int number_same(const char *a, const char *b, const struct number_region *region);

/*
 * The comparison form of a number the network reported (+CLCC, TS 27.007): a leading `+` is kept (type 145, international);
 * exactly nsn_len digits with no trunk prefix are the national significant number and get `+<country code>`; a string with
 * the trunk prefix normalises as a dialled number; anything else (short codes, other lengths) is kept as it came.
 * An empty or withheld number gives "". 0, or -ENAMETOOLONG when it does not fit `cap`.
 */
int number_from_network(const char *in, const struct number_region *region, char *out, size_t cap);

/*
 * The form a line shows and dials (the caller id on the handset, ITU-T E.123 national notation): a number of the home
 * country comes as `<trunk prefix><national significant number>`, a foreign one as `<international prefix><country
 * code><number>`, anything without a plus as it is. 0, or -ENAMETOOLONG.
 */
int number_national(const char *in, const struct number_region *region, char *out, size_t cap);

/*
 * 1 when `digits`, as dialled, is an emergency number: 112 and 911 always (3GPP TS 22.101) and the region's own list.
 * An exact match of the whole string; spaces and dashes are ignored. Nothing else changes or refuses such a number.
 */
int number_is_emergency(const char *digits, const struct number_region *region);

#endif /* AW_PHONE_CORE_NUMBERS_H */
