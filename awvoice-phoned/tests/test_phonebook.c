// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The phone book: contacts, name lookup, speed-dial codes (pure): make -f Makefile.host test. The digit strings are made up. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phonebook.h"

static struct phonebook *new_book(void)
{
	struct phonebook *pb = malloc(sizeof(*pb));

	assert(pb);
	phonebook_clear(pb, number_region_find("LK"));
	return pb;
}

static void test_add_and_lookup(void)
{
	struct phonebook *pb = new_book();
	const char *one[] = { "012345:mobile" };
	const char *two[] = { "0777:home", "+9488:work" };
	const char *plain[] = { "0999" };
	char name[40];

	assert(phonebook_add(pb, "Ann", one, 1, "", NULL) == 0);
	assert(phonebook_add(pb, "Bob", two, 2, "07", NULL) == 0);
	assert(phonebook_add(pb, "Cy", plain, 1, NULL, NULL) == 0);	/* a number without a type is taken */
	assert(pb->n == 3 && pb->c[0].count == 1 && !strcmp(pb->c[0].number[0], "012345") && !strcmp(pb->c[1].quick, "07"));
	/* the same number in any written form finds the contact */
	assert(phonebook_name(pb, "012345", name, sizeof(name)) == 0 && !strcmp(name, "Ann"));
	assert(phonebook_name(pb, "+9412345", name, sizeof(name)) == 0 && !strcmp(name, "Ann"));
	assert(phonebook_name(pb, "0094 12345", name, sizeof(name)) == 0 && !strcmp(name, "Ann"));
	assert(phonebook_name(pb, "0777", name, sizeof(name)) == 0 && !strcmp(name, "Bob"));
	assert(phonebook_name(pb, "088", name, sizeof(name)) == 0 && !strcmp(name, "Bob"));	/* the second number: +9488 */
	assert(phonebook_name(pb, "0999", name, sizeof(name)) == 0 && !strcmp(name, "Cy"));
	/* a tail is not a match, an unknown, empty or withheld number finds nothing */
	assert(phonebook_name(pb, "12345", name, sizeof(name)) == -ENOENT);
	assert(phonebook_name(pb, "0888", name, sizeof(name)) == -ENOENT);
	assert(phonebook_name(pb, "", name, sizeof(name)) == -ENOENT && phonebook_name(pb, NULL, name, sizeof(name)) == -ENOENT);
	/* a short output cuts the name, never overruns */
	assert(phonebook_name(pb, "0777", name, 2) == 0 && !strcmp(name, "B"));
	free(pb);
}

static void test_speed(void)
{
	struct phonebook *pb = new_book();
	const char *a[] = { "012345:mobile", "0777:home" };
	const char *b[] = { "0888" };
	char out[40];

	assert(phonebook_add(pb, "Ann", a, 2, "05", NULL) == 0);
	assert(phonebook_add(pb, "Bob", b, 1, "12", NULL) == 0);
	assert(phonebook_speed(pb, "05", out, sizeof(out)) == 0 && !strcmp(out, "012345"));	/* the first number */
	assert(phonebook_speed(pb, "12", out, sizeof(out)) == 0 && !strcmp(out, "0888"));
	assert(phonebook_speed(pb, "06", out, sizeof(out)) == -ENOENT);
	assert(phonebook_speed(pb, "5", out, sizeof(out)) == -ENOENT);		/* one digit or three are not codes */
	assert(phonebook_speed(pb, "051", out, sizeof(out)) == -ENOENT);
	assert(phonebook_speed(pb, "", out, sizeof(out)) == -ENOENT && phonebook_speed(pb, NULL, out, sizeof(out)) == -ENOENT);
	/* codes are unique: the second contact with a taken code is refused and not added */
	assert(phonebook_add(pb, "Cy", b, 1, "05", NULL) == -EEXIST && pb->n == 2);
	assert(phonebook_speed(pb, "05", out, sizeof(out)) == 0 && !strcmp(out, "012345"));
	free(pb);
}

static void test_refusals(void)
{
	struct phonebook *pb = new_book();
	const char *ok[] = { "0123" };
	const char *bad_num[] = { "01a3" };
	const char *bad_type[] = { "0123:fax" };
	const char *plus_late[] = { "01+23" };
	const char *four[] = { "1", "2", "3", "4" };
	const char *toolong[] = { "123456789012345678901234567890123" };
	const char *edge[] = { "12345678901234567890123456789012" };	/* 32: allowed */
	char *log_buf = NULL;
	size_t log_len = 0;
	FILE *log = open_memstream(&log_buf, &log_len);
	char longname[64];
	unsigned int k;

	assert(phonebook_add(pb, "", ok, 1, "", log) == -EINVAL);
	assert(phonebook_add(pb, NULL, ok, 1, "", log) == -EINVAL);
	assert(phonebook_add(pb, "Ann", ok, 0, "", log) == -EINVAL);
	assert(phonebook_add(pb, "Ann", four, 4, "", log) == -EINVAL);
	assert(phonebook_add(pb, "Ann", bad_num, 1, "", log) == -EINVAL);
	assert(phonebook_add(pb, "Ann", bad_type, 1, "", log) == -EINVAL);
	assert(phonebook_add(pb, "Ann", plus_late, 1, "", log) == -EINVAL);
	assert(phonebook_add(pb, "Ann", toolong, 1, "", log) == -EINVAL);
	assert(phonebook_add(pb, "Ann", ok, 1, "1", log) == -EINVAL);		/* a code is two digits */
	assert(phonebook_add(pb, "Ann", ok, 1, "123", log) == -EINVAL);
	assert(phonebook_add(pb, "Ann", ok, 1, "ab", log) == -EINVAL);
	memset(longname, 'n', 33);
	longname[33] = 0;
	assert(phonebook_add(pb, longname, ok, 1, "", log) == -EINVAL);
	assert(pb->n == 0);
	assert(phonebook_add(pb, "Ann", edge, 1, "", log) == 0 && pb->n == 1);
	/* the log names the reason and holds no name and no number */
	fflush(log);
	assert(log_buf && strstr(log_buf, "was not taken") && !strstr(log_buf, "Ann") && !strstr(log_buf, "01a3") && !strstr(log_buf, "fax"));
	fclose(log);
	free(log_buf);
	/* the book holds PB_CONTACTS_MAX contacts, then refuses */
	for (k = 1; k < PB_CONTACTS_MAX; k++)
		assert(phonebook_add(pb, "x", ok, 1, "", NULL) == 0);
	assert(pb->n == PB_CONTACTS_MAX && phonebook_add(pb, "y", ok, 1, "", NULL) == -ENOSPC);
	free(pb);
}

int main(void)
{
	test_add_and_lookup();
	test_speed();
	test_refusals();
	puts("phonebook: all tests passed");
	return 0;
}
