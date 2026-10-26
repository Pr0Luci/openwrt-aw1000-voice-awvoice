// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * The modem side of the call answering. The modem's call list query answers with one line per call:
 *   +CLCC: <id>,<direction>,<state>,<mode>,<multiparty>,<number>,...
 * The state and the mode of a line come before the quoted number, so a plain split on commas reads them; the
 * first voice call in the list decides the state. The modem's own unsolicited lines arrive mixed into every read
 * and are skipped.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "answer.h"
#include "../daemon/port.h"

#define STATE_REPLY_MS	1500		/* the call list usually answers within a few tens of ms; a late one is dropped, not waited for */
#define ACT_REPLY_MS	2000		/* the answer, hang-up and dial commands get this long */
#define SS_REPLY_MS	20000		/* a supplementary-service request (+CCFC, +CCWA, +CHLD) goes to the network and back: TS 27.007 gives no
					   bound; on the line the conference join answered after more than 2 s, so 20 s is the working bound */

static int reply_complete(const char *text)
{
	static const char *const alone[] = { "OK", "ERROR" };

	for (size_t k = 0; k < 2; k++) {
		char line[24];

		snprintf(line, sizeof(line), "\r\n%s\r\n", alone[k]);
		if (strstr(text, line))
			return 1;
	}
	return strstr(text, "+CME ERROR") || strstr(text, "+CMS ERROR") || strstr(text, "NO CARRIER");
}

int answer_parse(const char *text, int *st)
{
	const char *p = text;

	*st = CALL_NONE;
	while ((p = strstr(p, "+CLCC:"))) {
		int v[4] = { -1, -1, -1, -1 };
		unsigned int field;

		p += 6;
		while (*p == ' ')
			p++;
		/* the first four comma-separated fields: id, direction, state, mode */
		for (field = 0; field < 4 && *p >= '0' && *p <= '9'; field++) {
			v[field] = atoi(p);
			while (*p >= '0' && *p <= '9')
				p++;
			if (*p == ',')
				p++;
		}
	if (field == 4 && v[3] == 0) {
		/* a voice call line: its state decides (the data-call lines are skipped); the states are the
		 * 3GPP TS 27.007 call-list values: 0 active, 2 dialing, 3 alerting, 4 incoming */
		if (v[2] == 4)
			*st = CALL_INCOMING;
		else if (v[2] == 0)
			*st = CALL_ACTIVE;
		else if (v[2] == 2)
			*st = CALL_DIALING;
		else if (v[2] == 3)
			*st = CALL_ALERTING;
		return 0;
	}
	}
	return 0;
}

int answer_open_path(struct answer *a, const char *path)
{
	struct termios tio;

	if (strlen(path) >= sizeof(a->port))
		return -ENAMETOOLONG;
	memcpy(a->port, path, strlen(path) + 1);
	a->fd = open(a->port, O_NONBLOCK | O_NOCTTY | O_RDWR | O_CLOEXEC);
	if (a->fd < 0)
		return -errno;
	tcgetattr(a->fd, &tio);
	cfmakeraw(&tio);
	cfsetspeed(&tio, B115200);
	tio.c_cflag |= CLOCAL | CREAD;
	tcsetattr(a->fd, TCSANOW, &tio);
	tcflush(a->fd, TCIOFLUSH);
	a->last_st = CALL_NONE;
	a->busy = 0;
	a->dead = 0;
	return 0;
}

int answer_open(struct answer *a, unsigned int iface)
{
	char path[sizeof(a->port)];
	int err = port_find(PORT_SYSFS_ROOT, iface, path, sizeof(path));

	return err ? err : answer_open_path(a, path);
}

void answer_close(struct answer *a)
{
	if (a->fd >= 0)
		close(a->fd);
	a->fd = -1;
}

void answer_number(const char *text, char *out, size_t cap)
{
	const char *p = text;

	out[0] = 0;
	while ((p = strstr(p, "+CLCC:"))) {
		long v[5];
		unsigned int field;
		char *end;
		size_t n = 0;

		p += 6;
		/* fields: id, direction, state, mode, multiparty, then the quoted number */
		for (field = 0; field < 5; field++) {
			while (*p == ' ')
				p++;
			v[field] = strtol(p, &end, 10);
			if (end == p)
				break;
			p = end;
			if (*p == ',')
				p++;
		}
		if (field < 5 || v[3] != 0)
			continue;
		/* the quoted number: its digits, a leading plus kept; then the type (TS 24.008 10.5.4.7: 145 = international) */
		for (; *p && *p != '\r' && *p != '\n' && *p != ',' && n + 1 < cap; p++)
			if ((*p >= '0' && *p <= '9') || (*p == '+' && !n))
				out[n++] = *p;
		out[n] = 0;
		if (*p == ',' && n && out[0] != '+' && strtol(p + 1, NULL, 10) == 145 && n + 1 < cap) {
			memmove(out + 1, out, n + 1);
			out[0] = '+';
		}
		return;
	}
}

/* one row of the call list after its tag: "idx,dir,stat,mode,mpty,"number",type" (the number may be empty) */
static int leg_of_row(const char *row, struct call_leg *leg)
{
	int mode, type = 0;
	char quoted[40] = "";
	int got = sscanf(row, " %d,%d,%d,%d,%d,\"%39[^\"]\",%d", &leg->idx, &leg->dir, &leg->stat, &mode, &leg->mpty, quoted, &type);
	size_t used = 0, k;

	if (got < 5 || mode != 0 || leg->stat < LEG_ACTIVE || leg->stat > LEG_WAITING)
		return 0;
	/* the digits with a leading plus kept; the international type (TS 24.008 10.5.4.7: 145) gets one when the modem left it out */
	if (type == 145 && quoted[0] && quoted[0] != '+')
		leg->number[used++] = '+';
	for (k = 0; quoted[k] && used + 1 < sizeof(leg->number); k++)
		if ((quoted[k] >= '0' && quoted[k] <= '9') || (quoted[k] == '+' && !used))
			leg->number[used++] = quoted[k];
	leg->number[used] = 0;
	return 1;
}

unsigned int answer_legs(const char *text, struct call_leg *out, unsigned int max)
{
	unsigned int count = 0;
	const char *at = text;

	for (; count < max && (at = strstr(at, "+CLCC:")); at += 6) {
		char row[128];
		size_t len = strcspn(at + 6, "\r\n");

		if (len >= sizeof(row))
			continue;
		memcpy(row, at + 6, len);
		row[len] = 0;
		memset(&out[count], 0, sizeof(out[count]));
		if (leg_of_row(row, &out[count]))
			count++;
	}
	return count;
}

/* write what is left of the command; the port is non-blocking, so a full buffer leaves the rest for the next pump */
static int flush_tx(struct answer *a)
{
	while (a->tx_done < a->tx_len) {
		ssize_t put = write(a->fd, a->tx + a->tx_done, a->tx_len - a->tx_done);

		if (put > 0) {
			a->tx_done += (size_t)put;
			continue;
		}
		if (put < 0 && (errno == EAGAIN || errno == EINTR))
			return 0;
		return -errno;
	}
	return 0;
}

int answer_begin(struct answer *a, enum answer_kind kind, const char *cmd, unsigned long long now_ms)
{
	int len, err;

	if (a->fd < 0 || a->dead)
		return -ENODEV;
	if (a->busy)
		return -EBUSY;
	if (kind == ANSWER_STATE)
		len = snprintf(a->tx, sizeof(a->tx), "AT+CLCC\r");
	else if (kind == ANSWER_CAUSE)
		len = snprintf(a->tx, sizeof(a->tx), "AT+CEER\r");
	else
		len = cmd ? snprintf(a->tx, sizeof(a->tx), "%s\r", cmd) : -1;
	if (len < 0 || (size_t)len >= sizeof(a->tx))
		return -EINVAL;
	/* what the modem said before this request (its own announcements) is not the reply */
	if (isatty(a->fd) && tcflush(a->fd, TCIFLUSH))
		return -errno;
	a->tx_len = (size_t)len;
	a->tx_done = 0;
	a->rx_len = 0;
	a->rx[0] = 0;
	a->kind = kind;
	a->due_ms = now_ms + (kind == ANSWER_STATE ? STATE_REPLY_MS
		: cmd && (!strncmp(cmd, "AT+CCFC=", 8) || !strncmp(cmd, "AT+CCWA=", 8) || !strncmp(cmd, "AT+CHLD=", 8)) ? SS_REPLY_MS
		: ACT_REPLY_MS);
	a->busy = 1;
	err = flush_tx(a);		/* the request goes out now; what does not fit follows with the next pump */
	if (err) {
		a->busy = 0;
		a->dead = 1;
		return -ENODEV;
	}
	return 0;
}

/* the exchange ended: the result by kind */
/*
 * The state of the line's call from all the voice calls listed: an active call decides, else a call being dialled, then one alerting, then an
 * incoming or waiting one, and a list of held calls only is its own state. (With one call this is the state of that call.)
 */
int answer_state_of_legs(const struct call_leg *leg, unsigned int n, unsigned int *which)
{
	static const struct { int stat, state; } order[] = {
		{ LEG_ACTIVE, CALL_ACTIVE }, { LEG_DIALING, CALL_DIALING }, { LEG_ALERTING, CALL_ALERTING },
		{ LEG_INCOMING, CALL_INCOMING }, { LEG_WAITING, CALL_INCOMING }, { LEG_HELD, CALL_HELD },
	};
	unsigned int rank, k;

	for (rank = 0; rank < sizeof(order) / sizeof(order[0]); rank++)
		for (k = 0; k < n; k++)
			if (leg[k].stat == order[rank].stat) {
				*which = k;
				return order[rank].state;
			}
	return CALL_NONE;
}

static int finish(struct answer *a, int complete, int *result)
{
	a->busy = 0;
	if (a->kind == ANSWER_STATE) {
		int st;

		if (!complete) {
			*result = -ETIMEDOUT;		/* no verdict: the caller keeps what it knew */
			return 1;
		}
		answer_parse(a->rx, &st);
		answer_number(a->rx, a->number, sizeof(a->number));
		a->nlegs = answer_legs(a->rx, a->legs, CALL_LEGS_MAX);
		if (a->nlegs) {
			unsigned int which = 0;

			st = answer_state_of_legs(a->legs, a->nlegs, &which);
			snprintf(a->number, sizeof(a->number), "%s", a->legs[which].number);	/* the number of the call that decides */
		}
		/* a call that was dialing, ringing or connected and is no longer listed has ended */
		if ((a->last_st == CALL_ACTIVE || a->last_st == CALL_DIALING || a->last_st == CALL_ALERTING) && st == CALL_NONE)
			st = CALL_GONE;
		a->last_st = st;
		*result = st;
	} else if (a->kind == ANSWER_CAUSE) {
		*result = complete ? answer_cause(a->rx) : -ETIMEDOUT;
	} else {
		*result = !complete ? -ETIMEDOUT : strstr(a->rx, "\r\nOK\r\n") ? 0 : -EIO;
	}
	return 1;
}

int answer_pump(struct answer *a, unsigned long long now_ms, int *result)
{
	int err;

	if (!a->busy)
		return 0;
	err = flush_tx(a);
	for (;;) {
		ssize_t got;

		if (err)
			break;
		got = read(a->fd, a->rx + a->rx_len, sizeof(a->rx) - 1 - a->rx_len);
		if (got > 0) {
			a->rx_len += (size_t)got;
			a->rx[a->rx_len] = 0;
			if (reply_complete(a->rx) || a->rx_len + 1 >= sizeof(a->rx))
				return finish(a, 1, result);
			continue;
		}
		if (got == 0 && isatty(a->fd))
			break;			/* a terminal with nothing to read (VMIN 0 reads answer 0) is not a closed port */
		if (got == 0 || (errno != EAGAIN && errno != EINTR))
			err = got == 0 ? -EIO : -errno;
		break;
	}
	if (err) {
		a->busy = 0;
		a->dead = 1;			/* the port is gone (the modem reset or the USB device left) */
		*result = -ENODEV;
		return 1;
	}
	if (now_ms >= a->due_ms)
		return finish(a, 0, result);
	return 0;
}

const char *answer_body(const struct answer *a)
{
	return a->rx;
}

const char *answer_reply_line(const struct answer *a, char *out, size_t cap)
{
	static const char *const status[] = { "+CME ERROR", "+CMS ERROR", "NO CARRIER", "NO ANSWER", "BUSY", "ERROR", "OK" };
	size_t k;

	out[0] = 0;
	for (k = 0; k < sizeof(status) / sizeof(status[0]); k++) {
		const char *p = strstr(a->rx, status[k]);
		size_t n = 0;

		if (!p)
			continue;
		for (; p[n] && p[n] != '\r' && p[n] != '\n' && n + 1 < cap; n++)
			out[n] = p[n];
		out[n] = 0;
		break;
	}
	return out;
}

int answer_cause(const char *text)
{
	const char *p = strstr(text, "+CEER:");

	if (!p)
		return 0;
	p += 6;
	while (*p == ' ')
		p++;
	return (*p >= '0' && *p <= '9') ? atoi(p) : 0;
}