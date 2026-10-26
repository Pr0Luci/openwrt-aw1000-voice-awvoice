// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modemq.h"

const char *const modemq_command[] = { "AT+QDAI?", "AT+CLVL=?", "AT+CPIN?", "AT+CEREG?", "AT+QCFG=\"ims\"", "AT+QCSQ", "AT+QNWINFO", "AT+CLIR?" };
const unsigned int modemq_count = sizeof(modemq_command) / sizeof(modemq_command[0]);

/* the text after the tag, skipping the blanks; NULL when the tag is absent */
static const char *after(const char *reply, const char *tag)
{
	const char *p = strstr(reply, tag);

	if (!p)
		return NULL;
	p += strlen(tag);
	while (*p == ' ')
		p++;
	return p;
}

/* the n-th (0-based) comma-separated field of the line at p as an integer; MQ_UNKNOWN when it is not one */
static int field_int(const char *p, unsigned int n)
{
	char *end;
	long v;

	for (; n && *p && *p != '\r' && *p != '\n'; p++)
		if (*p == ',')
			n--;
	if (n)
		return MQ_UNKNOWN;
	v = strtol(p, &end, 10);
	if (end == p || (*end != ',' && *end != '\r' && *end != '\n' && *end))
		return MQ_UNKNOWN;
	return (int)v;
}

int modemq_sim_ready(const char *reply)
{
	const char *p = after(reply, "+CPIN:");

	if (!p)
		return MQ_UNKNOWN;
	return !strncmp(p, "READY", 5);
}

int modemq_qdai_head(const char *reply, char *out, unsigned int cap)
{
	const char *p = after(reply, "+QDAI:");
	unsigned int n = 0;

	out[0] = 0;
	if (!p)
		return 0;
	while (*p == ' ')
		p++;
	while (p[n] && p[n] != ',' && p[n] != ' ' && n < cap - 1)
		n++;
	if (!n)
		return 0;
	memcpy(out, p, n);
	out[n] = 0;
	return 1;
}

int modemq_registered(const char *reply)
{
	const char *p = after(reply, "+CEREG:");
	int stat, fields = 1;
	const char *q;

	if (!p)
		return MQ_UNKNOWN;
	/* the answer to the query is <n>,<stat>[,...]; an unsolicited report is <stat>[,...] */
	for (q = p; *q && *q != '\r' && *q != '\n'; q++)
		fields += *q == ',';
	stat = field_int(p, fields > 1 ? 1 : 0);
	if (stat == MQ_UNKNOWN || stat < 0 || stat > 9)
		return MQ_UNKNOWN;
	return stat == 1 || stat == 5;
}

int modemq_ims(const char *reply)
{
	const char *p = after(reply, "+QCFG: \"ims\",");
	int state;

	if (!p)
		return MQ_UNKNOWN;
	state = field_int(p, 1);
	return state == 0 || state == 1 ? state : MQ_UNKNOWN;
}

int modemq_rsrp_dbm(const char *reply)
{
	const char *p = after(reply, "+QCSQ:");
	int v;

	if (!p || *p != '"' || !strncmp(p, "\"NOSERVICE\"", 11) || !strncmp(p, "\"GSM\"", 5))
		return MQ_UNKNOWN;
	/* "LTE",<rssi>,<rsrp>,<sinr>,<rsrq> and the 5G forms put the reference signal power second */
	v = field_int(p, 2);
	return v == MQ_UNKNOWN || v < -160 || v > 0 ? MQ_UNKNOWN : v;
}

/* the n-th (0-based) quoted text of the line at p into out; false when there is none or it does not fit */
static int quoted(const char *p, unsigned int n, char *out, size_t cap)
{
	for (; *p && *p != '\r' && *p != '\n'; p++) {
		const char *close;

		if (*p != '"')
			continue;
		close = strchr(p + 1, '"');
		if (!close)
			return 0;
		if (n--) {
			p = close;
			continue;
		}
		if ((size_t)(close - p - 1) >= cap)
			return 0;
		memcpy(out, p + 1, (size_t)(close - p - 1));
		out[close - p - 1] = 0;
		return 1;
	}
	return 0;
}

void modemq_network(const char *reply, char *network, size_t ncap, char *band, size_t bcap)
{
	const char *p = after(reply, "+QNWINFO:");
	char tech[32];

	network[0] = band[0] = 0;
	if (!p || *p != '"')
		return;
	if (quoted(p, 0, tech, sizeof(tech))) {
		/* "FDD LTE", "TDD LTE", "NR5G-SA", "WCDMA", "GSM", "EDGE", "GPRS", "TDSCDMA" */
		if (strstr(tech, "NR5G"))
			snprintf(network, ncap, "5G");
		else if (strstr(tech, "LTE"))
			snprintf(network, ncap, "4G");
		else if (strstr(tech, "WCDMA") || strstr(tech, "TDSCDMA"))
			snprintf(network, ncap, "3G");
		else if (strstr(tech, "GSM") || strstr(tech, "EDGE") || strstr(tech, "GPRS"))
			snprintf(network, ncap, "2G");
	}
	/* "<tech>","<operator>","<band>",<channel> */
	if (!quoted(p, 2, band, bcap))
		band[0] = 0;
}

int modemq_audio_slave(const char *reply)
{
	const char *p = after(reply, "+QDAI:");
	int mode;

	if (!p)
		return MQ_UNKNOWN;
	/* <io>,<mode>,...: the first field prints as "x" on this modem; the second is 1 for slave, 0 for master */
	mode = field_int(p, 1);
	return mode == 0 || mode == 1 ? mode : MQ_UNKNOWN;
}

int modemq_range(const char *reply, const char *tag, int *low, int *high)
{
	const char *p = after(reply, tag);
	char *end;
	long lo, hi;

	if (!p || *p != '(')
		return 0;
	lo = strtol(p + 1, &end, 10);
	if (end == p + 1 || *end != '-')
		return 0;
	hi = strtol(end + 1, &end, 10);
	if (*end != ')' || hi < lo)
		return 0;
	*low = (int)lo;
	*high = (int)hi;
	return 1;
}

int modemq_clir(const char *reply, int *status)
{
	const char *p = after(reply, "+CLIR:");
	int n, m;

	if (status)
		*status = MQ_UNKNOWN;
	if (!p)
		return MQ_UNKNOWN;
	n = field_int(p, 0);
	m = field_int(p, 1);
	if (n < 0 || n > 2)
		return MQ_UNKNOWN;
	if (status && m >= 0 && m <= 4)
		*status = m;
	return n;
}

int modemq_ccwa(const char *reply)
{
	const char *at = reply;

	while ((at = strstr(at, "+CCWA:"))) {
		char *end;
		long status, cls;

		at += 6;
		status = strtol(at, &end, 10);
		if (end == at || *end != ',')
			continue;
		cls = strtol(end + 1, &end, 10);
		if ((cls & 1) && (status == 0 || status == 1))
			return (int)status;
	}
	return MQ_UNKNOWN;
}
