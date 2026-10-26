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
 * Standard commands (3GPP TS 27.007): +CPIN? (SIM), +CEREG? (EPS registration: stat 1 home, 5 roaming). The modem's own
 * commands, because this modem answers ERROR to +CESQ and +CIREG?): +QCSQ (signal), +QNWINFO (access technology and
 * band), +QCFG="ims" (IMS registration, the second field; TO-VERIFY in the unregistered state).
 */
#define MQ_UNKNOWN	(-1000)

#define MQ_NETWORK_MAX	24

extern const char *const modemq_command[];	/* the questions, in the order they are asked */
extern const unsigned int modemq_count;

/* 1 = ready, 0 = another state, MQ_UNKNOWN */
int modemq_sim_ready(const char *reply);
/* 1 = registered (home or roaming), 0 = not, MQ_UNKNOWN */
int modemq_registered(const char *reply);
/* the first field of the AT+QDAI answer, kept as the modem printed it (a digit or the literal "x"): copied into out; 0 when nothing usable */
int modemq_qdai_head(const char *reply, char *out, unsigned int cap);
/* the PCM mode: 1 = the modem is the slave of our clock, 0 = master, MQ_UNKNOWN (AT+QDAI?, the second field) */
int modemq_ims(const char *reply);
/* the PCM mode: 1 = the modem is the slave of our clock, 0 = master, MQ_UNKNOWN (AT+QDAI?, the second field) */
int modemq_audio_slave(const char *reply);
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
/* the access technology for display ("2G", "3G", "4G", "5G"; "" when unknown) and the band text of the reply ("LTE BAND 3"; "" when none) */
void modemq_network(const char *reply, char *network, size_t ncap, char *band, size_t bcap);

#endif /* AW_PHONE_CORE_MODEMQ_H */
