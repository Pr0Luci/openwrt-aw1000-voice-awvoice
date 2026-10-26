// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The +CCFC commands and the read-back parser (pure): make -f Makefile.host test. The numbers are made up. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "forward.h"

static struct phone_forward fw(const char *to, enum fwd_when when, unsigned int delay)
{
	struct phone_forward f;

	memset(&f, 0, sizeof(f));
	f.configured = f.enabled = 1;
	snprintf(f.to, sizeof(f.to), "%s", to);
	f.when = when;
	f.delay_s = delay;
	return f;
}

static void test_commands(void)
{
	char cmd[64];
	struct phone_forward f;

	/* every reason: the number of TS 27.007; the delay only with 2 and 5 */
	f = fw("+9412345", FWD_ALWAYS, 20);
	assert(forward_register_cmd(&f, cmd, sizeof(cmd)) == 0 && !strcmp(cmd, "AT+CCFC=0,3,\"+9412345\",145"));
	f = fw("012345", FWD_BUSY, 20);
	assert(forward_register_cmd(&f, cmd, sizeof(cmd)) == 0 && !strcmp(cmd, "AT+CCFC=1,3,\"012345\",129"));
	f = fw("012345", FWD_NOANSWER, 25);
	assert(forward_register_cmd(&f, cmd, sizeof(cmd)) == 0 && !strcmp(cmd, "AT+CCFC=2,3,\"012345\",129,,,25"));
	f = fw("+9412345", FWD_UNREACHABLE, 5);
	assert(forward_register_cmd(&f, cmd, sizeof(cmd)) == 0 && !strcmp(cmd, "AT+CCFC=3,3,\"+9412345\",145"));
	f = fw("+9412345", FWD_ALL_CONDITIONAL, 30);
	assert(forward_register_cmd(&f, cmd, sizeof(cmd)) == 0 && !strcmp(cmd, "AT+CCFC=5,3,\"+9412345\",145,,,30"));
	assert(forward_reason(FWD_ALWAYS) == 0 && forward_reason(FWD_BUSY) == 1 && forward_reason(FWD_NOANSWER) == 2 &&
	       forward_reason(FWD_UNREACHABLE) == 3 && forward_reason(FWD_ALL_CONDITIONAL) == 5);
	/* a refused target or delay makes no command */
	f = fw("", FWD_ALWAYS, 5);
	assert(forward_register_cmd(&f, cmd, sizeof(cmd)) == -EINVAL);
	f = fw("+", FWD_ALWAYS, 5);
	assert(forward_register_cmd(&f, cmd, sizeof(cmd)) == -EINVAL);
	f = fw("12a", FWD_ALWAYS, 5);
	assert(forward_register_cmd(&f, cmd, sizeof(cmd)) == -EINVAL);
	f = fw("1+2", FWD_ALWAYS, 5);
	assert(forward_register_cmd(&f, cmd, sizeof(cmd)) == -EINVAL);
	f = fw("012345", FWD_NOANSWER, 7);
	assert(forward_register_cmd(&f, cmd, sizeof(cmd)) == -EINVAL);
	f = fw("012345", FWD_NOANSWER, 35);
	assert(forward_register_cmd(&f, cmd, sizeof(cmd)) == -EINVAL);
	f = fw("012345", FWD_NOANSWER, 0);
	assert(forward_register_cmd(&f, cmd, sizeof(cmd)) == -EINVAL);
	f = fw("012345", FWD_BUSY, 0);						/* the delay does not matter for busy */
	assert(forward_register_cmd(&f, cmd, sizeof(cmd)) == 0);
	/* the longest target still fits the modem's command buffer (48) */
	f = fw("+1234567890123456789", FWD_ALL_CONDITIONAL, 30);
	assert(forward_register_cmd(&f, cmd, 48) == 0 && strlen(cmd) < 48);
	f = fw("012345", FWD_NOANSWER, 25);
	assert(forward_register_cmd(&f, cmd, 10) == -ENAMETOOLONG);
	assert(!strcmp(forward_erase_cmd(), "AT+CCFC=4,4"));
	assert(forward_query_cmd(FWD_ALL_CONDITIONAL, cmd, sizeof(cmd)) == 0 && !strcmp(cmd, "AT+CCFC=5,2"));
	assert(forward_query_cmd(FWD_BUSY, cmd, 4) == -ENAMETOOLONG);
}

static void test_when_and_same(void)
{
	enum fwd_when w;
	struct phone_forward a = fw("012345", FWD_BUSY, 10), b = a;

	assert(forward_when_parse("always", &w) == 0 && w == FWD_ALWAYS);
	assert(forward_when_parse("all_conditional", &w) == 0 && w == FWD_ALL_CONDITIONAL);
	assert(forward_when_parse("noanswer", &w) == 0 && w == FWD_NOANSWER);
	assert(forward_when_parse("Busy", &w) == -EINVAL && forward_when_parse("", &w) == -EINVAL && forward_when_parse(NULL, &w) == -EINVAL);
	assert(forward_same(&a, &b));
	b.delay_s = 15;
	assert(!forward_same(&a, &b));
	b = a;
	b.enabled = 0;
	assert(!forward_same(&a, &b));
	b = a;
	snprintf(b.to, sizeof(b.to), "%s", "012346");
	assert(!forward_same(&a, &b));
	b = a;
	b.configured = 0;
	assert(!forward_same(&a, &b));
}

static void test_parse(void)
{
	int st;
	char num[32];
	unsigned int t;

	forward_parse("AT+CCFC=2,2\r\r\n+CCFC: 1,1,\"+9412345\",145,,,20\r\n\r\nOK\r\n", &st, num, sizeof(num), &t);
	assert(st == 1 && !strcmp(num, "+9412345") && t == 20);
	/* an active forwarding without a time (not a no-reply reason) */
	forward_parse("+CCFC: 1,1,\"012345\",129\r\nOK\r\n", &st, num, sizeof(num), &t);
	assert(st == 1 && !strcmp(num, "012345") && t == 0);
	/* not active */
	forward_parse("+CCFC: 0,1\r\nOK\r\n", &st, num, sizeof(num), &t);
	assert(st == 0 && !num[0] && t == 0);
	/* the voice class among others: the data class line before it is skipped */
	forward_parse("+CCFC: 0,2\r\n+CCFC: 1,1,\"+9499\",145,,,10\r\n+CCFC: 0,4\r\nOK\r\n", &st, num, sizeof(num), &t);
	assert(st == 1 && !strcmp(num, "+9499") && t == 10);
	/* class 255 and 7 include voice */
	forward_parse("+CCFC: 1,255,\"+9499\",145\r\nOK\r\n", &st, num, sizeof(num), &t);
	assert(st == 1);
	forward_parse("+CCFC: 0,7\r\nOK\r\n", &st, num, sizeof(num), &t);
	assert(st == 0);
	/* nothing usable */
	forward_parse("OK\r\n", &st, num, sizeof(num), &t);
	assert(st == -1 && !num[0]);
	forward_parse("+CME ERROR: 30\r\n", &st, num, sizeof(num), &t);
	assert(st == -1);
	forward_parse("+CCFC: 0,2\r\nOK\r\n", &st, num, sizeof(num), &t);	/* only a data class */
	assert(st == -1);
	forward_parse("+CCFC: x,1\r\n", &st, num, sizeof(num), &t);
	assert(st == -1);
	forward_parse("+CCFC: 5,1\r\n", &st, num, sizeof(num), &t);		/* a status outside 0/1 */
	assert(st == -1);
	/* a number that does not fit is dropped, the status stays */
	forward_parse("+CCFC: 1,1,\"+94123456789012345\",145\r\n", &st, num, 8, &t);
	assert(st == 1 && !num[0]);
	/* a time that is not a number */
	forward_parse("+CCFC: 1,1,\"1\",129,,,x\r\n", &st, num, sizeof(num), &t);
	assert(st == 1 && t == 0);
}

int main(void)
{
	test_commands();
	test_when_and_same();
	test_parse();
	puts("forward: all tests passed");
	return 0;
}
