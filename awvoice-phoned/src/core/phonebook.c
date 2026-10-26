// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <string.h>

#include "phonebook.h"

void phonebook_clear(struct phonebook *pb, const struct number_region *region)
{
	memset(pb, 0, sizeof(*pb));
	pb->region = region;
}

static int refuse(FILE *log, int err, const char *why)
{
	if (log)
		fprintf(log, "a contact was not taken: %s\n", why);
	return err;
}

/* the number rule of the page: digits, '*', '#', a leading '+', at most PHONE_NUMBER_MAX characters */
static int number_ok(const char *text, size_t n)
{
	size_t k;

	if (!n || n > PHONE_NUMBER_MAX)
		return 0;
	for (k = 0; k < n; k++)
		if (!((text[k] >= '0' && text[k] <= '9') || text[k] == '*' || text[k] == '#' || (k == 0 && text[k] == '+')))
			return 0;
	return 1;
}

static int type_ok(const char *t)
{
	return !strcmp(t, "mobile") || !strcmp(t, "home") || !strcmp(t, "work");
}

int phonebook_add(struct phonebook *pb, const char *name, const char *const *numbers, unsigned int count, const char *quick, FILE *log)
{
	struct pb_contact ct;
	unsigned int k;

	memset(&ct, 0, sizeof(ct));
	if (!name || !*name || strlen(name) > PB_NAME_MAX)
		return refuse(log, -EINVAL, "the name is empty or too long");
	snprintf(ct.name, sizeof(ct.name), "%s", name);
	if (!count || count > PB_NUMBERS_MAX)
		return refuse(log, -EINVAL, "a contact has one to three numbers");
	for (k = 0; k < count; k++) {
		const char *colon = strchr(numbers[k], ':');
		size_t n = colon ? (size_t)(colon - numbers[k]) : strlen(numbers[k]);

		if (!number_ok(numbers[k], n) || (colon && !type_ok(colon + 1)))
			return refuse(log, -EINVAL, "a number or its type is not valid");
		memcpy(ct.number[k], numbers[k], n);
		ct.number[k][n] = 0;
	}
	ct.count = count;
	if (quick && *quick) {
		if (strlen(quick) != PB_QUICK_LEN || quick[0] < '0' || quick[0] > '9' || quick[1] < '0' || quick[1] > '9')
			return refuse(log, -EINVAL, "the speed-dial code is not two digits");
		for (k = 0; k < pb->n; k++)
			if (!strcmp(pb->c[k].quick, quick))
				return refuse(log, -EEXIST, "the speed-dial code is already taken");
		snprintf(ct.quick, sizeof(ct.quick), "%s", quick);
	}
	if (pb->n >= PB_CONTACTS_MAX)
		return refuse(log, -ENOSPC, "the phone book is full");
	pb->c[pb->n++] = ct;
	return 0;
}

int phonebook_name(const struct phonebook *pb, const char *number, char *out, size_t cap)
{
	unsigned int k, j;

	if (!number || !*number || !cap)
		return -ENOENT;
	for (k = 0; k < pb->n; k++)
		for (j = 0; j < pb->c[k].count; j++)
			if (number_same(pb->c[k].number[j], number, pb->region)) {
				snprintf(out, cap, "%s", pb->c[k].name);
				return 0;
			}
	return -ENOENT;
}

int phonebook_speed(const struct phonebook *pb, const char *code, char *out, size_t cap)
{
	unsigned int k;

	if (!code || strlen(code) != PB_QUICK_LEN || !cap)
		return -ENOENT;
	for (k = 0; k < pb->n; k++)
		if (pb->c[k].quick[0] && !strcmp(pb->c[k].quick, code)) {
			snprintf(out, cap, "%s", pb->c[k].number[0]);
			return 0;
		}
	return -ENOENT;
}
