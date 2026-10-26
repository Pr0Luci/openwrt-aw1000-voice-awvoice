// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* Call blocking decisions (pure): make -f Makefile.host test. The digit strings are made up. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blocking.h"

static struct blocking *fresh(void)
{
	struct blocking *b = malloc(sizeof(*b));

	assert(b);
	blocking_clear(b, number_region_find("LK"));
	return b;
}

static void test_exact(void)
{
	struct blocking *b = fresh();

	assert(blocking_add(b, BLOCK_IN, "012345", NULL) == 0 && blocking_add(b, BLOCK_OUT, "+94777", NULL) == 0);
	/* national and international forms of one number match each other */
	assert(blocking_incoming(b, "012345") && blocking_incoming(b, "+9412345") && blocking_incoming(b, "0094 12345"));
	assert(!blocking_incoming(b, "012346") && !blocking_incoming(b, "12345"));
	/* the lists are separate: an outgoing rule does not block an incoming call and the other way round */
	assert(!blocking_incoming(b, "0777") && !blocking_outgoing(b, "012345"));
	assert(blocking_outgoing(b, "0777") && blocking_outgoing(b, "+94777") && !blocking_outgoing(b, "07771"));
	/* service codes are compared as written */
	assert(blocking_add(b, BLOCK_OUT, "*123#", NULL) == 0);
	assert(blocking_outgoing(b, "*123#") && !blocking_outgoing(b, "*124#"));
	free(b);
}

static void test_prefix(void)
{
	struct blocking *b = fresh();

	assert(blocking_add(b, BLOCK_IN_PREFIX, "077", NULL) == 0 && blocking_add(b, BLOCK_OUT_PREFIX, "+441", NULL) == 0);
	assert(blocking_add(b, BLOCK_OUT_PREFIX, "190", NULL) == 0);
	/* a national prefix matches the national and the international form */
	assert(blocking_incoming(b, "077123") && blocking_incoming(b, "+9477123") && blocking_incoming(b, "077"));
	assert(!blocking_incoming(b, "076123") && !blocking_incoming(b, "+9476123") && !blocking_incoming(b, "07"));
	assert(blocking_outgoing(b, "+44123") && blocking_outgoing(b, "0044123") && !blocking_outgoing(b, "+4423"));
	/* a short code without a trunk prefix is matched as typed */
	assert(blocking_outgoing(b, "19012") && !blocking_outgoing(b, "1802"));
	free(b);
}

static void test_anonymous(void)
{
	struct blocking *b = fresh();

	assert(!blocking_incoming(b, "") && !blocking_incoming(b, NULL));	/* off: a withheld number is let through */
	b->anonymous = 1;
	assert(blocking_incoming(b, "") && blocking_incoming(b, NULL));
	assert(!blocking_incoming(b, "012345"));				/* a caller with a number is not anonymous */
	assert(!blocking_outgoing(b, ""));
	free(b);
}

static void test_emergency(void)
{
	struct blocking *b = fresh();

	/* the rules would match, the emergency exemption wins in both directions */
	assert(blocking_add(b, BLOCK_IN, "112", NULL) == 0 && blocking_add(b, BLOCK_OUT, "112", NULL) == 0);
	assert(blocking_add(b, BLOCK_IN_PREFIX, "9", NULL) == 0 && blocking_add(b, BLOCK_OUT_PREFIX, "9", NULL) == 0);
	assert(!blocking_incoming(b, "112") && !blocking_outgoing(b, "112"));
	assert(!blocking_incoming(b, "911") && !blocking_outgoing(b, "911"));
	assert(blocking_outgoing(b, "9111") && blocking_incoming(b, "9111"));	/* only the exact emergency string is exempt */
	free(b);
}

static void test_add(void)
{
	struct blocking *b = fresh();
	char *buf = NULL;
	size_t len = 0;
	FILE *log = open_memstream(&buf, &len);
	unsigned int k;

	assert(blocking_add(b, BLOCK_IN, "", log) == -EINVAL);
	assert(blocking_add(b, BLOCK_IN, "12a", log) == -EINVAL);
	assert(blocking_add(b, BLOCK_IN, "1+2", log) == -EINVAL);
	assert(blocking_add(b, BLOCK_IN, NULL, log) == -EINVAL);
	assert(blocking_add(b, BLOCK_IN, "123456789012345678901234567890123", log) == -EINVAL);
	assert(blocking_add(b, BLOCK_IN_PREFIX, "*12", log) == -EINVAL);	/* a prefix is digits */
	assert(blocking_add(b, BLOCK_OUT_PREFIX, "+", log) == -EINVAL);
	assert(blocking_add(b, BLOCK_LISTS, "12", log) == -EINVAL);
	assert(blocking_add(b, BLOCK_IN, "*12#", log) == 0 && blocking_add(b, BLOCK_IN_PREFIX, "+94", log) == 0);
	fflush(log);
	assert(buf && strstr(buf, "not taken") && !strstr(buf, "12a") && !strstr(buf, "1+2"));
	fclose(log);
	free(buf);
	for (k = 0; k < BLOCK_LIST_MAX; k++)
		assert(blocking_add(b, BLOCK_OUT, "55", NULL) == 0);
	assert(b->count[BLOCK_OUT] == BLOCK_LIST_MAX && blocking_add(b, BLOCK_OUT, "56", NULL) == -ENOSPC);
	blocking_clear(b, NULL);
	assert(!b->count[BLOCK_OUT] && !b->anonymous);
	free(b);
}

int main(void)
{
	test_exact();
	test_prefix();
	test_anonymous();
	test_emergency();
	test_add();
	puts("blocking: all tests passed");
	return 0;
}
