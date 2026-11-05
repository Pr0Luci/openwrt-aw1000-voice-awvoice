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

/* one of the daemon's two file folders, as the health reply shows it (storage.h checks it; the reply only carries it) */
struct reply_storage {
	int ready;				/* the folder was proven: there, writable, room enough */
	int err;				/* why not, an errno value (0 when ready) */
	unsigned long long free_mb;		/* the room left on its filesystem (0 when not ready) */
};

struct dnd;		/* the quiet hours as the daemon holds them (dnd.h); the reply reads them, writes nothing */

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
	int layout;				/* the audio layout the line was brought up with: 0 = 8 kHz, 1 = 16 kHz */
	const char *region;			/* the numbering plan in force, as the number table names it (numbers.h); NULL = not chosen yet */
	const char *const *leds;		/* the router's LED names for the page's selector */
	int nleds;				/* the count, or a negative errno from the scan */
	int machine_greeting;			/* the daemon holds a usable greeting, whatever the settings ask (the machine answers when both hold) */
	unsigned int machine_greeting_ms;	/* its length, milliseconds (0 without a greeting) */
	const char *machine_greeting_err;	/* the one word for why there is none: reply_greeting_word(), "" when one is loaded */
	unsigned int machine_delay_s;		/* the wait before the machine answers */
	const char *record_path;		/* the recordings folder ("" when none is configured) */
	const char *message_path;		/* the answering machine's messages folder */
	struct reply_storage record_store;	/* the recordings folder's last check */
	struct reply_storage message_store;	/* the messages folder's last check */
	const char *sip_mode;			/* awvoice.sip.mode: web, interop or baresip */
	const char *sip_object_owner;		/* health sip.objectOwner: awvoice, voipd, none */
	const char *sip_port_owner;		/* health atPort.portOwner: free, busy */
	const char *sip_last_refusal;		/* the last refusal word the object logged ("" = none) */
	unsigned int sip_consumer_pid;		/* the front end that attached a media binding (0 = none) */
	const char *sip_web_audio;		/* health web.audio: off, waiting, listening, connected, or why not */
	unsigned int sip_web_port;		/* health web.port: the browser-audio listener's port */
	const char *sip_web_addr;		/* health web.address: the address it is bound to ("" = not listening) */
	const struct dnd *dnd;			/* the quiet hours the daemon read and the ask the line carries (NULL: none read at all) */
};

/* the status.state word of a line state: idle, offhook, ring, call, noservice (api.h) */
const char *reply_state_name(enum phone_line line);

/* the call phase word of the modem's view of a call: ringing, dialing, alerting, active ("ended" for the event only) */
const char *reply_phase_name(enum call_state call, enum phone_dir dir, int answered);

/* the status reply (the shapes api.h states); `now_ms` is the core's clock */
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

/* the one word a greeting's state goes by, on the health reply and on the upload methods' refusals: "" when there is
 * nothing to say about the file (reply_greeting_word maps the errnos machine.h hands back) */
const char *reply_greeting_word(int err);

#endif /* AW_PHONED_REPLY_H */
