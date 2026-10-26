// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The AT client's reply times (answer.c): make -f Makefile.host test. The port is a pipe, so nothing is sent anywhere. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "answer.h"

/* a request started at t gives up at t + its reply time: the call list quickly, an action in 2 s, and a
 * supplementary-service request (forwarding, call waiting, hold / join / release-one: TS 22.030) in 20 s,
 * because those go to the network and back (the conference join answered after more than 2 s on the line) */
static unsigned long long due_of(enum answer_kind kind, const char *cmd)
{
	struct answer a;
	int fds[2];

	memset(&a, 0, sizeof(a));
	assert(!pipe(fds));
	a.fd = fds[1];
	assert(answer_begin(&a, kind, cmd, 1000) == 0);
	close(fds[0]);
	close(fds[1]);
	return a.due_ms - 1000;
}

int main(void)
{
	assert(due_of(ANSWER_STATE, NULL) == 1500);
	assert(due_of(ANSWER_CMD, "ATA") == 2000);
	assert(due_of(ANSWER_CMD, "AT+CHUP") == 2000);
	assert(due_of(ANSWER_CMD, "ATD112;") == 2000);
	assert(due_of(ANSWER_CMD, "AT+CCFC=0,3,\"112\",129") == 20000);
	assert(due_of(ANSWER_CMD, "AT+CCWA=1,1,1") == 20000);
	assert(due_of(ANSWER_CMD, "AT+CHLD=2") == 20000);
	assert(due_of(ANSWER_CMD, "AT+CHLD=3") == 20000);
	assert(due_of(ANSWER_CMD, "AT+CHLD=11") == 20000);
	printf("answer: all tests passed\n");
	return 0;
}
