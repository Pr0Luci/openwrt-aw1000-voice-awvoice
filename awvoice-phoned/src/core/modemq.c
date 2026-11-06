// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modemq.h"

const char *const modemq_command[] = { "AT+QDAI?", "AT+CLVL=?", "AT+CPIN?", "AT+CEREG?", "AT+QCFG=\"ims\"", "AT+QCSQ", "AT+QNWINFO", "AT+CLIR?", "AT+COPS?", "AT+QENG=\"servingcell\"", "AT+QSPN" };
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

/* the n-th (0-based) field of the reply line at p, counting commas; NULL when the line has fewer fields */
static const char *field_at(const char *p, unsigned int n)
{
	for (; n; n--) {
		p = strchr(p, ',');
		if (!p)
			return NULL;
		p++;
	}
	return p;
}

int modemq_operator(const char *reply, const char *tag, unsigned int at, char *mcc, size_t cap, char *name, size_t ncap)
{
	const char *p = after(reply, tag);
	char field[MQ_NETWORK_MAX];
	const char *f;
	size_t n;

	if (cap)
		mcc[0] = 0;
	if (ncap)
		name[0] = 0;
	if (!p || !cap || cap < MQ_MCC_MAX)
		return -1;					/* nothing was answered */
	/* the operator field of the reply: quoted in the name formats, plain digits in the numeric one (TS 27.007 +COPS
	 * read after AT+COPS=3,0, and the modem's own +QNWINFO, which prints the PLMN it is camped on as digits) */
	f = field_at(p, at);
	if (!f || !*f || *f == '\r' || *f == '\n')
		return -1;					/* the reply names no operator: nothing selected yet */
	n = strcspn(f, "\r\n,");
	if (n >= sizeof(field))
		n = sizeof(field) - 1;
	memcpy(field, f, n);
	field[n] = 0;
	/* the field as the modem printed it, without the quotes a name form carries */
	if (field[0] == '"') {
		f = field + 1;
		n = strcspn(f, "\"");
	} else
		f = field;
	if (ncap) {
		size_t copy = n < ncap - 1 ? n : ncap - 1;

		memcpy(name, f, copy);
		name[copy] = 0;
	}
	if (n != 5 && n != 6)
		return 0;					/* a name, or digits of another length: no country code in it */
	if (f[0] < '0' || f[0] > '9' || f[1] < '0' || f[1] > '9' || f[2] < '0' || f[2] > '9')
		return 0;
	memcpy(mcc, f, 3);
	mcc[3] = 0;
	if (ncap)
		name[0] = 0;					/* digits were read: nothing to say about a name */
	return 1;
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

int modemq_audio_rate(const char *reply)
{
	const char *p = after(reply, "+QDAI:");
	int mode;

	if (!p)
		return MQ_UNKNOWN;
	/* <id>,<mode>,<mclk>,<fs>,<busy>,<pcm_mode>,...: the sixth field, 0 = 8 kHz per slot, 1 = 16 kHz (two samples) */
	mode = field_int(p, 5);
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

/*
 * The serving cell (AT+QENG="servingcell", the manual's shapes this port reads: NR5G-SA, EN-DC and LTE; GSM and WCDMA
 * set the mode word and nothing else). The answer is one to three lines:
 *   SA:  +QENG: "servingcell",<state>,"NR5G-SA",<duplex>,<MCC>,<MNC>,<cellID>,<PCID>,<TAC>,<ARFCN>,<band>,<DLbw>,<RSRP>,<RSRQ>,<SINR>,...
 *   LTE: +QENG: "servingcell",<state>,"LTE",<is_tdd>,<MCC>,<MNC>,<cellID>,<PCID>,<earfcn>,<band>,<ULbw>,<DLbw>,<TAC>,<RSRP>,<RSRQ>,<RSSI>,<SINR>,...
 *   NSA: +QENG: "servingcell",<state>   (the anchor line, no mode word of its own)
 *        +QENG: "LTE",...               (the anchor's row)
 *        +QENG: "NR5G-NSA",<MCC>,<MNC>,<PCID>,<RSRP>,<SINR>,<RSRQ>,<ARFCN>,<band>
 * A value the modem prints as "-" is a value it does not have: kept out rather than invented.
 */
int modemq_servingcell(const char *reply, struct modemq_cell *cell)
{
	char tok[18][14];
	const char *p = reply;
	unsigned int n;

	memset(cell, 0, sizeof(*cell));
	cell->rsrp = MQ_UNKNOWN;
	for (;;) {
		p = strstr(p, "+QENG:");
		if (!p)
			return cell->mode[0] ? 0 : 1;
		p += strlen("+QENG:");
		while (*p == ' ' || *p == '"')
			p++;
		memset(tok, 0, sizeof(tok));				/* a short line leaves the tokens behind it empty, not whatever was there before */
		for (n = 0; n < 18; n++) {
			size_t len;

			while (*p == ' ' || *p == '"')
				p++;				/* a field prints as plain text or in quotes: the quotes are its skin, not its content */
			len = strcspn(p, ",\r\n\"");			/* the token runs to the first comma, quote or line end, whichever is first */
			if (len >= sizeof(tok[n]))
				len = sizeof(tok[n]) - 1;
			memcpy(tok[n], p, len);
			tok[n][len] = 0;
			p += len;
			while (*p == '"' || *p == ' ')
				p++;				/* the closing quote goes with the token */
			if (*p != ',')
				break;				/* the line is over */
			p++;
			if (*p == '\r' || *p == '\n' || !*p)
				break;
		}
		if (!strcmp(tok[0], "servingcell")) {
			snprintf(cell->state, sizeof(cell->state), "%s", tok[1]);
			if (!strcmp(tok[2], "LTE")) {
				snprintf(cell->mode, sizeof(cell->mode), "LTE");
				snprintf(cell->mcc, sizeof(cell->mcc), "%s", tok[4]);
				snprintf(cell->mnc, sizeof(cell->mnc), "%s", tok[5]);
				snprintf(cell->band, sizeof(cell->band), "%s", tok[9]);
				cell->rsrp = field_int(tok[13], 0);
				return 0;
			}
			if (!strcmp(tok[2], "NR5G-SA")) {
				snprintf(cell->mode, sizeof(cell->mode), "NR5G-SA");
				snprintf(cell->mcc, sizeof(cell->mcc), "%s", tok[4]);
				snprintf(cell->mnc, sizeof(cell->mnc), "%s", tok[5]);
				snprintf(cell->band, sizeof(cell->band), "%s", tok[10]);
				cell->rsrp = field_int(tok[12], 0);
				return 0;
			}
			if (tok[2][0]) {				/* GSM or WCDMA: the domain is the fact, the numbers stay out */
				snprintf(cell->mode, sizeof(cell->mode), "%s", tok[2]);
				return 0;
			}
			/* the anchor line of an EN-DC answer: its mode word comes on the rows behind it */
		} else if (!strcmp(tok[0], "NR5G-NSA")) {
			snprintf(cell->mode, sizeof(cell->mode), "NR5G-NSA");
			snprintf(cell->mcc, sizeof(cell->mcc), "%s", tok[1]);
			snprintf(cell->mnc, sizeof(cell->mnc), "%s", tok[2]);
			snprintf(cell->band, sizeof(cell->band), "%s", tok[8]);
			cell->rsrp = field_int(tok[4], 0);
			return 0;
		}
		if (strchr(p, '\n'))
			p = strchr(p, '\n') + 1;
		else
			return cell->mode[0] ? 0 : 1;
	}
}

int modemq_spn(const char *reply, char *name, size_t cap)
{
	const char *p = after(reply, "+QSPN:"), *end;
	size_t n;

	if (cap)
		name[0] = 0;
	if (!p)
		return 0;
	while (*p == ' ')
		p++;
	if (*p != '"')
		return 0;
	p++;
	end = strchr(p, '"');
	if (!end)
		return 0;
	n = (size_t)(end - p);
	if (!n)
		return 0;
	if (n >= cap)
		n = cap - 1;
	memcpy(name, p, n);
	name[n] = 0;
	return 1;
}
