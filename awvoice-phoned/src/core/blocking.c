// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <string.h>

#include "blocking.h"

void blocking_clear(struct blocking *b, const struct number_region *region)
{
	memset(b, 0, sizeof(*b));
	b->region = region;
}

static int rule_ok(enum block_list list, const char *v)
{
	size_t n = strlen(v), k;
	const int prefix = list == BLOCK_IN_PREFIX || list == BLOCK_OUT_PREFIX;

	if (!n || n > PHONE_NUMBER_MAX)
		return 0;
	for (k = 0; k < n; k++) {
		const char ch = v[k];

		if ((ch >= '0' && ch <= '9') || (k == 0 && ch == '+'))
			continue;
		if (!prefix && (ch == '*' || ch == '#'))
			continue;
		return 0;
	}
	return !(prefix && n == 1 && v[0] == '+');
}

int blocking_add(struct blocking *b, enum block_list list, const char *value, FILE *log)
{
	if (list >= BLOCK_LISTS || !value || !rule_ok(list, value)) {
		if (log)
			fprintf(log, "a blocking rule was not taken: it is not a valid number or prefix\n");
		return -EINVAL;
	}
	if (b->count[list] >= BLOCK_LIST_MAX) {
		if (log)
			fprintf(log, "a blocking rule was not taken: the list is full\n");
		return -ENOSPC;
	}
	snprintf(b->entry[list][b->count[list]++], PHONE_NUMBER_MAX + 1, "%s", value);
	return 0;
}

/* a prefix rule: the number starts with it, in the international form or as typed (short codes have no trunk prefix) */
static int has_prefix(const char *number, const char *prefix, const struct number_region *region)
{
	char nn[64], np[64];

	if (!strncmp(number, prefix, strlen(prefix)))
		return 1;
	return !number_normalise(number, region, nn, sizeof(nn)) && !number_normalise(prefix, region, np, sizeof(np)) &&
	       !strncmp(nn, np, strlen(np));
}

static int listed(const struct blocking *b, enum block_list exact, enum block_list prefix, const char *number)
{
	unsigned int k;

	for (k = 0; k < b->count[exact]; k++)
		if (number_same(b->entry[exact][k], number, b->region))
			return 1;
	for (k = 0; k < b->count[prefix]; k++)
		if (has_prefix(number, b->entry[prefix][k], b->region))
			return 1;
	return 0;
}

int blocking_incoming(const struct blocking *b, const char *number)
{
	if (!number || !*number)
		return b->anonymous;
	if (number_is_emergency(number, b->region))
		return 0;
	return listed(b, BLOCK_IN, BLOCK_IN_PREFIX, number);
}

int blocking_outgoing(const struct blocking *b, const char *number)
{
	if (!number || !*number || number_is_emergency(number, b->region))
		return 0;
	return listed(b, BLOCK_OUT, BLOCK_OUT_PREFIX, number);
}
