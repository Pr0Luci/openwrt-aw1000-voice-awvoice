/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONE_CORE_PHONEBOOK_H
#define AW_PHONE_CORE_PHONEBOOK_H

#include <stdio.h>

#include "numbers.h"
#include "phone.h"

/*
 * The phone book: the contacts (UCI sections `contact`, written by the phone page) as the core uses them: the caller's name for an
 * incoming number, and the speed-dial codes. Pure data and lookups, no UCI here; the daemon fills it at the start and at every reload.
 *
 * Speed dial (the page's U28): a two-digit code followed by the send key `#` dials that contact's first number; the same two digits
 * without `#` are an ordinary number that is dialled after the pause. A code is unique: a second contact that wants a taken code is refused.
 * Numbers are compared in their normalised form (numbers.h), so a contact written as a national number is found for a call that comes in
 * with the country code.
 */
#define PB_CONTACTS_MAX	100
#define PB_NUMBERS_MAX	3		/* the page's limit per contact */
#define PB_NAME_MAX	32
#define PB_QUICK_LEN	2

struct pb_contact {
	char name[PB_NAME_MAX + 1];
	char number[PB_NUMBERS_MAX][PHONE_NUMBER_MAX + 1];
	unsigned int count;
	char quick[PB_QUICK_LEN + 1];	/* "" or two digits */
};

struct phonebook {
	struct pb_contact c[PB_CONTACTS_MAX];
	unsigned int n;
	const struct number_region *region;	/* NULL = the default region */
};

void phonebook_clear(struct phonebook *pb, const struct number_region *region);

/*
 * Add one contact. `numbers` are the UCI list values `<number>` or `<number>:<mobile|home|work>` (1..PB_NUMBERS_MAX of them), `quick` is ""
 * or two digits. 0; -EINVAL (no name, no usable number, a number that breaks the number rule, a type that is none of the three, a quick code
 * that is not two digits) and -EEXIST (the code is taken), -ENOSPC (full): the contact is then not added and `log` gets one line that names
 * the reason and never the number or the name.
 */
int phonebook_add(struct phonebook *pb, const char *name, const char *const *numbers, unsigned int count, const char *quick, FILE *log);

/* the name of the contact that has this number (normalised compare), into out; 0 or -ENOENT */
int phonebook_name(const struct phonebook *pb, const char *number, char *out, size_t cap);

/* the first number of the contact with this speed-dial code, into out; 0 or -ENOENT */
int phonebook_speed(const struct phonebook *pb, const char *code, char *out, size_t cap);

#endif /* AW_PHONE_CORE_PHONEBOOK_H */
