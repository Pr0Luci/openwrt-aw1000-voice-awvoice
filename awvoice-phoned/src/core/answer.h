/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_SLIC_ANSWER_H
#define AW_SLIC_ANSWER_H

#include <stddef.h>

#include "stages.h"

/*
 * The modem side of the stage tool's call answering and dialing: the tool opens the modem's AT port (the same way
 * the modem probe tool does: raw, 115200, the port of USB interface 2), asks for the call list about once a second,
 * sends the answer / hang-up / dial commands and asks for the release cause of a call that never reached the
 * active state. The reply lines of other kinds (the ring announcements the modem pushes by itself) are skipped;
 * only the call state matters here.
 *
 * Nothing here waits. A request is started with answer_begin() and finished by calling answer_pump() from the
 * caller's own loop; the line loop watches the hook every few milliseconds, so a slow or silent modem must never
 * stop it. One request is open at a time; a reply that does not complete in time ends in -ETIMEDOUT.
 *
 * Only one open at a time: while the stage tool's answering port is open the modem's own arbiter must be stopped
 * (the stage tool run stops it; the revert is starting it again).
 */
enum answer_kind { ANSWER_STATE, ANSWER_CMD, ANSWER_CAUSE, ANSWER_INFO };

#define ANSWER_REPLY_MAX	2048

struct answer {
	int fd;
	char port[300];
	int last_st;		/* the last polled state, for the connected-call-just-ended edge */
	struct call_leg legs[CALL_LEGS_MAX];	/* the voice calls of the last call list */
	unsigned int nlegs;
	char number[32];	/* the number of the first voice call of the last poll (digits only, may be empty) */
	int busy;		/* a request is open */
	int dead;		/* the port was lost (the modem reset or left the bus): no request starts again */
	enum answer_kind kind;	/* of the open request */
	char tx[128];
	size_t tx_len, tx_done;
	char rx[ANSWER_REPLY_MAX];
	size_t rx_len;
	unsigned long long due_ms;	/* the reply is given up on at this time */
};

/* open the port of USB interface iface (found with port_find) / a given tty node: raw 115200, non-blocking; 0 or -errno */
int answer_open(struct answer *a, unsigned int iface);
int answer_open_path(struct answer *a, const char *path);
void answer_close(struct answer *a);

/* start one request: ANSWER_STATE asks for the call list, ANSWER_CMD sends cmd (ATA, AT+CHUP, ATD...;), ANSWER_CAUSE
 * asks for the release cause. 0 = started; -EBUSY = one is still open; -ENODEV = no port; -EINVAL = bad command */
int answer_begin(struct answer *a, enum answer_kind kind, const char *cmd, unsigned long long now_ms);

/* move the open request on (never blocks). 0 = nothing finished; 1 = it finished and *result holds its outcome:
 *   ANSWER_STATE  one of enum call_state, or -ETIMEDOUT (no verdict: keep what was known);
 *   ANSWER_CMD    0 when the modem answered OK, -EIO on ERROR, -ETIMEDOUT on no reply;
 *   ANSWER_INFO   the same as ANSWER_CMD (a read-only question; the text is in answer_body());
 *   ANSWER_CAUSE  the cause number (0 = unknown), or -ETIMEDOUT;
 * and -ENODEV for any of them when the port is lost. */
int answer_pump(struct answer *a, unsigned long long now_ms, int *result);

/* the last reply as one short line (the status the modem gave: ERROR, +CME ERROR: n, NO CARRIER, or empty), for the log; never a number */
const char *answer_reply_line(const struct answer *a, char *out, size_t cap);

/* the whole text of the last reply (until the next request starts) */
const char *answer_body(const struct answer *a);

/* the number of the first voice call line (digits only) into out; empty when there is none */
void answer_number(const char *text, char *out, size_t cap);

/* the state of the line's call from all the listed voice calls: active, else dialling, alerting, incoming (or waiting), else held; CALL_NONE for none. *which = the leg that decides. */
int answer_state_of_legs(const struct call_leg *leg, unsigned int n, unsigned int *which);

/* every voice call line (mode 0) of a call-list text, in order, at most `max`; the count. Lines of other modes (data, fax) are skipped, and so is a line that is cut short. */
unsigned int answer_legs(const char *text, struct call_leg *out, unsigned int max);

/* the release cause out of an AT+CEER reply (the digits after "+CEER:"), 0 when there is none */
int answer_cause(const char *text);

/* test helper: pick the call state(s) out of the raw reply text; *st = the state of the first voice call found */
int answer_parse(const char *text, int *st);

#endif /* AW_SLIC_ANSWER_H */