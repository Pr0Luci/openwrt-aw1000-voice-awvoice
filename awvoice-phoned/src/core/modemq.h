/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONE_CORE_MODEMQ_H
#define AW_PHONE_CORE_MODEMQ_H

#include <stddef.h>

/*
 * The read-only questions the line asks the modem while it is idle, for the status page and for the log line that says
 * under which radio conditions a call was answered. Pure parsers of the reply text; the reply lines they are tested
 * with were captured on the router. Every parser answers MQ_UNKNOWN when the reply has another shape,
 * so a modem that words something differently shows "unknown" instead of a wrong value.
 *
 * Standard commands (3GPP TS 27.007): +CPIN? (SIM), +CEREG? (EPS registration: stat 1 home, 5 roaming), +COPS? (the
 * operator the line is registered on, read after the setup asked for its numeric form with AT+COPS=3,0). The modem's
 * own commands, because this modem answers ERROR to +CESQ and +CIREG?): +QCSQ (signal), +QNWINFO (access technology and
 * band), +QCFG="ims" (IMS registration, the second field; TO-VERIFY in the unregistered state).
 */
#define MQ_UNKNOWN	(-1000)

#define MQ_NETWORK_MAX	24
#define MQ_MCC_MAX	4	/* the three digits of a mobile country code and the closing NUL (ITU-T E.212) */

extern const char *const modemq_command[];	/* the questions, in the order they are asked */
extern const unsigned int modemq_count;

/* 1 = ready, 0 = another state, MQ_UNKNOWN */
int modemq_sim_ready(const char *reply);
/* 1 = registered (home or roaming), 0 = not, MQ_UNKNOWN */
int modemq_registered(const char *reply);
/* the first field of the AT+QDAI answer, kept as the modem printed it (a digit or the literal "x"): copied into out; 0 when nothing usable */
int modemq_qdai_head(const char *reply, char *out, unsigned int cap);
/* the IMS voice over session setting (AT+QCFG="ims", 3GPP TS 27.007 +QCFG): 1 enabled, 0 disabled, MQ_UNKNOWN */
int modemq_ims(const char *reply);
/* the PCM mode: 1 = the modem is the slave of the router's clock, 0 = master, MQ_UNKNOWN (AT+QDAI?, the second field) */
int modemq_audio_slave(const char *reply);
/*
 * The PCM sample rate the modem will put on the bus (AT+QDAI?, the sixth field, the <pcm_mode> value):
 * 0 = narrowband (8 kHz, one sample per frame), 1 = wideband (16 kHz, a second sample half a frame after each of
 * the four), MQ_UNKNOWN when the reply has another shape or holds a value this bridge cannot serve. This is the only
 * answer to whether the audio on the wire is really wideband: the setting and the chip's registers say what was
 * asked for, the modem's reply says what is.
 */
int modemq_audio_rate(const char *reply);
/* the range a setting accepts, from a test-command reply such as "+CLVL: (0-5)" (tag "+CLVL:"); 0 when the reply has no such range */
int modemq_range(const char *reply, const char *tag, int *low, int *high);
/*
 * The calling-line identification restriction (AT+CLIR?, 3GPP TS 27.007 +CLIR: <n>,<m>): n = the setting (0 as the network's subscription says, 1 hide, 2 show;
 * returned, MQ_UNKNOWN when the reply has another shape), m = the network's provisioning state (0 not provisioned, 1 provisioned, 2 unknown, 3 temporarily
 * restricted, 4 temporarily allowed; into *status, MQ_UNKNOWN when it is not there or out of range).
 */
int modemq_clir(const char *reply, int *status);
/* the network's call waiting for voice (AT+CCWA=1,2 answer "+CCWA: <status>,<class>", TS 27.007): 1 on, 0 off, MQ_UNKNOWN when there is no voice-class line */
int modemq_ccwa(const char *reply);
/* the reference signal power in dBm (the second value of the LTE and 5G forms), MQ_UNKNOWN when there is none */
int modemq_rsrp_dbm(const char *reply);

/*
 * The serving cell as AT+QENG="servingcell" answers it (the manual's three shapes this port cares about: NR5G-SA,
 * NR5G-NSA, LTE; GSM/WCDMA set the mode word and nothing else). `mode` is the network domain for display ("LTE",
 * "NR5G-SA", "NR5G-NSA"), `state` the UE state word ("SEARCH", "LIMSRV", "NOCONN", "CONNECT"), `band` the band as
 * the modem prints it, `mcc`/`mnc` the PLMN when the answer carries it, `rsrp` the serving cell's reference power in
 * dBm (MQ_UNKNOWN when the answer holds none). 0 when a row was read, 1 when the reply holds no row of this kind.
 */
struct modemq_cell {
	char state[10];
	char mode[12];
	char band[12];
	char mcc[4];
	char mnc[4];
	int rsrp;
};
int modemq_servingcell(const char *reply, struct modemq_cell *cell);

/* the service provider name (AT+QSPN, the first quoted string of the answer): copied without the quotes; 0 when the reply names nothing */
int modemq_spn(const char *reply, char *name, size_t cap);
/* the access technology for display ("2G", "3G", "4G", "5G"; "" when unknown) and the band text of the reply ("LTE BAND 3"; "" when none) */
void modemq_network(const char *reply, char *network, size_t ncap, char *band, size_t bcap);
/*
 * The network the modem is camped on, read out of the `at`-th comma field of a reply that carries the tag `tag`:
 * AT+COPS? (3GPP TS 27.007 +COPS: <mode>[,<format>,<oper>[,<AcT>]], the operator third, asked after the setup set the
 * numeric form with AT+COPS=3,0) or the modem's own +QNWINFO (access technology, PLMN, band, channel: the PLMN second).
 * `mcc` (at least MQ_MCC_MAX bytes) takes the three digits of the mobile country code (ITU-T E.212) -- the field is
 * < MCC (three), MNC (two or three) >, so "41301" and "413010" both answer "413"; `name` (may be cap 0) takes the
 * field as printed when that is not digits, so the caller can say what came back. 1 = digits read, 0 = something came
 * that is not a country code (a name in text, or digits of another length), -1 = nothing to read (no reply, no field,
 * or the reply names no operator).
 */
int modemq_operator(const char *reply, const char *tag, unsigned int at, char *mcc, size_t cap, char *name, size_t ncap);

#endif /* AW_PHONE_CORE_MODEMQ_H */
