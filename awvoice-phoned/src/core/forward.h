/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONE_CORE_FORWARD_H
#define AW_PHONE_CORE_FORWARD_H

#include <stddef.h>

/*
 * Call forwarding is the network's service, set through the modem (3GPP TS 27.007 +CCFC, TS 22.082): the commands and the read-back parser. Pure
 * functions; the call core runs the exchange (call.c).
 *
 *   AT+CCFC=<reason>,<mode>[,<number>[,<type>[,<class>[,<subaddr>[,<satype>[,<time>]]]]]]
 *   reason: 0 unconditional, 1 busy, 2 no reply, 3 not reachable, 4 all call forwarding, 5 all conditional forwarding
 *   mode:   0 disable, 1 enable, 2 query, 3 registration, 4 erasure          type: 145 international (leading +), 129 otherwise
 *   time:   seconds before the no-reply forwarding applies (the reasons 2 and 5), TS 27.007 1..30
 */
#define FWD_TO_MAX	20	/* characters of the target: the command must fit the modem command buffer */

enum fwd_when { FWD_ALWAYS, FWD_BUSY, FWD_NOANSWER, FWD_UNREACHABLE, FWD_ALL_CONDITIONAL, FWD_WHEN_COUNT };

struct phone_forward {
	int configured;			/* the section exists: without it the daemon never touches the network's forwarding */
	int enabled;
	char to[FWD_TO_MAX + 1];
	enum fwd_when when;
	unsigned int delay_s;		/* 5..30 in steps of 5 (the page's choices); used by noanswer and all_conditional only */
};

/* 1 when both describe the same service */
int forward_same(const struct phone_forward *a, const struct phone_forward *b);

/* the +CCFC <reason> of a `when` */
int forward_reason(enum fwd_when when);

/* "always" | "busy" | "noanswer" | "unreachable" | "all_conditional" -> 0; anything else -> -EINVAL */
int forward_when_parse(const char *text, enum fwd_when *when);

/* The registration command for the settings (mode 3). 0 or -EINVAL (no usable target, a delay off the 5 step) / -ENAMETOOLONG. */
int forward_register_cmd(const struct phone_forward *f, char *cmd, size_t cap);

/* The command that erases every forwarding the network holds for the line (reason 4, mode 4), and the query of one reason (mode 2). */
const char *forward_erase_cmd(void);
int forward_query_cmd(enum fwd_when when, char *cmd, size_t cap);

/*
 * The voice-class answer of a query: +CCFC: <status>,<class>[,<number>,<type>[,<subaddr>,<satype>[,<time>]]]. *status = 1 active, 0 not active,
 * -1 when the reply has no voice-class line; `number` and `*time_s` are the network's own values (empty / 0 when absent). Other classes are ignored.
 */
void forward_parse(const char *reply, int *status, char *number, size_t ncap, unsigned int *time_s);

#endif /* AW_PHONE_CORE_FORWARD_H */
