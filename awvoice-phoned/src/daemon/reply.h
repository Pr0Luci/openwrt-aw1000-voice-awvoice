/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_REPLY_H
#define AW_PHONED_REPLY_H

#include "calllog.h"
#include "indicator.h"
#include "phone.h"

/*
 * The shapes of the ubus replies and events (api.h), written against a tiny output interface so that they can be
 * tested on the host without libubox: api.c gives them a blobmsg sink, the host tests a JSON text sink. A table or
 * an array opened with open() is closed with close(); a member of an array has the name NULL.
 */
struct reply_sink {
	void *ctx;
	void (*open)(void *ctx, const char *name, int array);
	void (*close)(void *ctx);
	void (*text)(void *ctx, const char *name, const char *value);
	void (*number)(void *ctx, const char *name, long long value);
	void (*flag)(void *ctx, const char *name, int value);
	void (*null)(void *ctx, const char *name);
};

/* what the replies need besides the core's status: facts of the daemon itself */
struct reply_info {
	int line_in_service;			/* 0: the core is not running (status then says "down") */
	const char *my_number;
	unsigned int channel;
	unsigned int at_interface;
	const char *port;			/* the modem port in use, "" when none */
	int module_loaded;
	int bridge_ok;				/* the bridge counters below are valid */
	unsigned long long bridge_frames;
	unsigned int bridge_running, bridge_resyncs, bridge_tx_skipped, bridge_max_gap_us;
	const struct indicator *ind;		/* the phone LED's state (NULL: none yet) */
	int hd;					/* the HD voice state: 0 off, 1 wanted but the modem not switched yet, 2 on */
	const char *const *leds;		/* the router's LED names for the page's selector */
	int nleds;				/* the count, or a negative errno from the scan */
};

/* the status.state word of a line state: idle, offhook, ring, call, noservice (api.h) */
const char *reply_state_name(enum phone_line line);

/* the call phase word of the modem's view of a call: ringing, dialing, alerting, active ("ended" for the event only) */
const char *reply_phase_name(enum call_state call, enum phone_dir dir, int answered);

/* the status reply (DESIGN of the phone page, data contract v2); `now_ms` is the core's clock */
void reply_status(const struct reply_sink *out, const struct phone_status *st, const struct reply_info *info, unsigned long long now_ms);

/* the health reply without the log tail (the page's provider reads that itself) */
void reply_health(const struct reply_sink *out, const struct phone_status *st, const struct reply_info *info, unsigned long long now_ms);

/* the events: never a number, whatever the status holds */
void reply_event_line(const struct reply_sink *out, enum phone_line line);
void reply_event_call(const struct reply_sink *out, const struct phone_event *ev, int answered);

/* the calls reply: { calls: [ Call ] } newest first, the page's Call shape (numbers in full: the reply is for the access-controlled API only) */
struct phonebook;
/* the call log; with a phone book each record carries the contact's name ("" when none) */
void reply_calls(const struct reply_sink *out, const struct calllog *log, const struct phonebook *book);

/* the forwarding_get reply: { network: "on"|"off"|"unknown", to, delaySec, applying, error: string|null } -- what the network reports (the settings are UCI) */
void reply_forwarding(const struct reply_sink *out, const struct phone_forward_state *fwd);

/* the line test's reply: { ok, refused: null, verdict, hook, batteryMv, expectedMv, loopUa, leakSuspect } */
void reply_line_test(const struct reply_sink *out, const struct phone_line_test_result *res);

/* the refusal word of a method that could not act (NULL = it acted) */
const char *reply_refusal(int err, int off_hook);

#endif /* AW_PHONED_REPLY_H */
