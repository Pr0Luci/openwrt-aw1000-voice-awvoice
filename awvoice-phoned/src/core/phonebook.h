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
 * Speed dial: a two-digit code followed by the send key `#` dials that contact's first number; the same two digits
 * without `#` are an ordinary number that is dialled after the pause. A code is unique: a second contact that wants a taken code is refused.
 * Numbers are compared in their normalised form (numbers.h), so a contact written as a national number is found for a call that comes in
 * with the country code.
 *
 * A contact can be marked to ring through quiet hours (dnd.h). The mark belongs to the contact, not to the quiet hours: one name, one
 * answer, and a contact that is edited or deleted takes its mark with it instead of leaving a number behind in a second list.
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
	int ring_through;		/* 1 = this caller rings through quiet hours (dnd.h) */
};

struct phonebook {
	struct pb_contact c[PB_CONTACTS_MAX];
	unsigned int n;
	const struct number_region *region;	/* NULL = the default region */
};

void phonebook_clear(struct phonebook *pb, const struct number_region *region);

/*
 * Add one contact. `numbers` are the UCI list values `<number>` or `<number>:<mobile|home|work>` (1..PB_NUMBERS_MAX of them), `quick` is ""
 * or two digits, `ring_through` is the contact's own quiet-hours ask (0 or 1, see dnd.h). 0; -EINVAL (no name, no usable number, a number that breaks the number rule, a type that is none of the three, a quick code
 * that is not two digits) and -EEXIST (the code is taken), -ENOSPC (full): the contact is then not added and `log` gets one line that names
 * the reason and never the number or the name.
 */
int phonebook_add(struct phonebook *pb, const char *name, const char *const *numbers, unsigned int count, const char *quick,
	int ring_through, FILE *log);

/* the name of the contact that has this number (normalised compare), into out; 0 or -ENOENT */
int phonebook_name(const struct phonebook *pb, const char *number, char *out, size_t cap);

/* 1 when a contact holding this number (normalised compare) asks to ring through quiet hours; a withheld caller never does */
int phonebook_ring_through(const struct phonebook *pb, const char *number);

/* the first number of the contact with this speed-dial code, into out; 0 or -ENOENT */
int phonebook_speed(const struct phonebook *pb, const char *code, char *out, size_t cap);

#endif /* AW_PHONE_CORE_PHONEBOOK_H */
