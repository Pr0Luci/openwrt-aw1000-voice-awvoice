// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forward.h"

int forward_same(const struct phone_forward *a, const struct phone_forward *b)
{
	return a->configured == b->configured && a->enabled == b->enabled && a->when == b->when && a->delay_s == b->delay_s && !strcmp(a->to, b->to);
}

int forward_reason(enum fwd_when when)
{
	static const int reason[FWD_WHEN_COUNT] = { [FWD_ALWAYS] = 0, [FWD_BUSY] = 1, [FWD_NOANSWER] = 2, [FWD_UNREACHABLE] = 3, [FWD_ALL_CONDITIONAL] = 5 };

	return (unsigned int)when < FWD_WHEN_COUNT ? reason[when] : 0;
}

int forward_when_parse(const char *text, enum fwd_when *when)
{
	static const char *const word[FWD_WHEN_COUNT] = { "always", "busy", "noanswer", "unreachable", "all_conditional" };
	unsigned int k;

	for (k = 0; text && k < FWD_WHEN_COUNT; k++)
		if (!strcmp(text, word[k])) {
			*when = (enum fwd_when)k;
			return 0;
		}
	return -EINVAL;
}

static int target_ok(const char *to)
{
	size_t n = strlen(to), k;

	if (!n || n > FWD_TO_MAX)
		return 0;
	for (k = 0; k < n; k++)
		if (!((to[k] >= '0' && to[k] <= '9') || (k == 0 && to[k] == '+')))
			return 0;
	return !(n == 1 && to[0] == '+');
}

int forward_register_cmd(const struct phone_forward *f, char *cmd, size_t cap)
{
	const int reason = forward_reason(f->when);
	const int timed = reason == 2 || reason == 5;
	int n;

	if (!target_ok(f->to) || (timed && (f->delay_s < 5 || f->delay_s > 30 || f->delay_s % 5)))
		return -EINVAL;
	if (timed)
		n = snprintf(cmd, cap, "AT+CCFC=%d,3,\"%s\",%d,,,%u", reason, f->to, f->to[0] == '+' ? 145 : 129, f->delay_s);
	else
		n = snprintf(cmd, cap, "AT+CCFC=%d,3,\"%s\",%d", reason, f->to, f->to[0] == '+' ? 145 : 129);
	return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}

const char *forward_erase_cmd(void)
{
	return "AT+CCFC=4,4";
}

int forward_query_cmd(enum fwd_when when, char *cmd, size_t cap)
{
	int n = snprintf(cmd, cap, "AT+CCFC=%d,2", forward_reason(when));

	return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}

void forward_parse(const char *reply, int *status, char *number, size_t ncap, unsigned int *time_s)
{
	const char *line = reply;

	*status = -1;
	if (ncap)
		number[0] = 0;
	*time_s = 0;
	while ((line = strstr(line, "+CCFC:"))) {
		const char *p = line + 6;
		char *end;
		long st, cls;

		line = p;
		st = strtol(p, &end, 10);
		if (end == p || *end != ',')
			continue;
		p = end + 1;
		cls = strtol(p, &end, 10);
		if (end == p || !(cls & 1) || (st != 0 && st != 1))
			continue;			/* another class, or not a status */
		*status = (int)st;
		p = end;
		if (*p == ',' && p[1] == '"') {
			const char *close = strchr(p + 2, '"');
			size_t len = close ? (size_t)(close - (p + 2)) : 0;

			if (close && len < ncap) {
				memcpy(number, p + 2, len);
				number[len] = 0;
			}
			p = close ? close + 1 : p + 1;
			/* ,<type>[,<subaddr>,<satype>[,<time>]] : the time is the fourth field after the number */
			{
				unsigned int field = 0;
				const char *q = p;

				for (; *q && *q != '\r' && *q != '\n'; q++) {
					if (*q != ',')
						continue;
					if (++field == 4) {
						long t = strtol(q + 1, &end, 10);

						if (end != q + 1 && t > 0)
							*time_s = (unsigned int)t;
						break;
					}
				}
			}
		}
		return;
	}
}
