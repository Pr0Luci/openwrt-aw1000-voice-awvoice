// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "contacts.h"

#define DIGITS_MAX 40

static size_t digits_of(const char *text, size_t span, char *dest)
{
	size_t kept = 0, at;

	for (at = 0; at < span && text[at] && kept + 1 < DIGITS_MAX; at++)
		if (text[at] >= '0' && text[at] <= '9')
			dest[kept++] = text[at];
	dest[kept] = 0;
	return kept;
}

static int is_tail(const char *small, size_t ns, const char *big, size_t nb)
{
	return ns >= CONTACTS_MATCH_MIN && ns <= nb && !strcmp(big + (nb - ns), small);
}

int contacts_same(const char *a, const char *b)
{
	char da[DIGITS_MAX], db[DIGITS_MAX];
	size_t na = digits_of(a, strlen(a), da), nb = digits_of(b, strlen(b), db);

	return is_tail(da, na, db, nb) || is_tail(db, nb, da, na);
}

int contacts_find(const char *path, const char *number, char *out, size_t cap)
{
	char row[160];
	FILE *book;
	int found = -ENOENT;

	if (!path || !(book = fopen(path, "r")))
		return -ENOENT;
	while (found && fgets(row, sizeof(row), book)) {
		size_t cut = strcspn(row, " \t");
		char key[DIGITS_MAX];

		if (!digits_of(row, cut, key) || !contacts_same(key, number))
			continue;
		snprintf(out, cap, "%s", row + cut + strspn(row + cut, " \t"));
		out[strcspn(out, "\r\n")] = 0;
		found = out[0] ? 0 : -ENOENT;
	}
	fclose(book);
	return found;
}
