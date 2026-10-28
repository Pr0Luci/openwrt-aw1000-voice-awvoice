/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_API_H
#define AW_PHONED_API_H

#include "../core/phone.h"
#include "calllog.h"
#include "reply.h"

/*
 * The ubus object `awvoice` (libubus, blobmsg). The phone page's provider (`phone/live.js` of luci-app-awvoice) calls it
 * through rpcd; the app's ACL grants the methods. The reply shapes are the page's data contract v2
 * (packages/luci-app-awvoice/DESIGN.md §2) field for field, so the provider passes them on without reshaping.
 *
 * M4 methods:
 *   status     {}  -> Status:  { state, line, hook, machine, call: null | { id, direction, number, phase, elapsedSec },
 *                               voiceNetwork: { registered, ims }, signal: { rsrpDbm, network }, service: { running }, myNumber }
 *                     machine: "none" | "answering" | "answered" | null (the answering machine's part in the call)
 *                     state from enum phone_line: idle -> "idle"; dial_tone, dialling -> "offhook"; ringing -> "ring";
 *                     setup, call, release -> "call"; no_service -> "noservice" ("down" is the provider's, when ubus fails)
 *                     phase: incoming -> "ringing", dialing -> "dialing", alerting -> "alerting", active -> "active"
 *                     elapsedSec: (now - answered_ms) / 1000, 0 before the answer; null JSON for unknown tri-states
 *   health     {}  -> Health without log.tail: { atPort, line, modem (modem carries hdVoice 'off'|'set'), system, indicator { led, state, leds: [] }, counters,
 *                     machine { greetingLoaded, greetingMs, delaySec },
 *                     storage { recordings { path, ready, freeMb, error }, messages { ... } } }
 *                     modem also holds the caller-id restriction read back from the modem (M5): clir "network"|"hide"|"show"|null and
 *                     clirStatus "not_provisioned"|"provisioned"|"unknown"|"restricted"|"allowed"|null
 *                     machine.greetingLoaded is false when the greeting file was refused: the machine will not answer
 *                     storage: the folders' state as of the last check (start-up, a reload, a call's start); ready false with
 *                     error set to the system's reason (a missing mount, a read-only filesystem) means no file can be kept
 *   hangup     {}                  -> { ok, refused: null|"NO_CALL" }
 *   ring_test  { seconds: 1..10 }  -> { ok, refused: null|"ACTIVE_CALL"|"OFF_HOOK" }
 *   reload     {}                  -> { ok }                       the same as a UCI change (SIGHUP)
 * M5 methods (the ACL of the page: calls is read, calls_delete is write):
 *   line_test    {}                 -> { ok, refused, verdict, hook, batteryMv, expectedMv, loopUa, leakSuspect }   answered when the three samples are in (about 300 ms);
 *                                        refused: "ACTIVE_CALL" | "OFF_HOOK" | "SERVICE_DOWN"; ok is true for verdict "ok"
 *   forwarding_get {}                -> { network: "on"|"off"|"unknown", to, delaySec, applying, error }   what the network reports after the daemon set the
 *                                        forwarding (AT+CCFC, read back); the settings themselves are the UCI section `forwarding`
 *   calls        {}                 -> { calls: [ Call ] }          newest first, at most 200; the numbers in full;
 *                                        Call.message names the answering machine's message the call left ("" = none)
 *   calls_delete { type?: "IN"|"OUT"|"MISS" } -> { ok, removed }     all calls, or one type
 *   recordings / messages {}        -> { recordings: [ File ] } / { messages: [ File ] }   the finished files of the
 *                                        recordings directory / the machine's messages directory, as
 *                                        { name, bytes, rate, channels, seconds } (a message also ts: the epoch in its name)
 *   recording_get / message_get { name, offset }
 *                                   -> { name, total, offset, b64, more }   one chunk of the file, standard base64
 *   recording_delete / message_delete { name } -> { ok, removed }
 *                                        only <epoch>-<call id>.wav names are ever served; a path cannot get through
 * Events (ubus_send_event), never with a number:
 *   awvoice.line  { state, line }
 *   awvoice.call  { id, direction, phase }      phase as above plus "ended"
 *
 * Reserved, not registered yet: loopback_test (M5, the page's §6), and for the
 * add-on boundary (DESIGN.md §12) dial, answer, route, call_info. Service start / stop / restart are the LuCI app's rpcd script.
 */

struct ubus_context;

struct phonebook;
struct indicator;
struct machine;

struct api_ctx {
	struct ubus_context *ubus;	/* set by api_init (the connection the object lives on) */
	int connected;			/* the object is registered; events go out from then on (a dead connection drops them) */
	unsigned int ended_id;		/* the last call id whose "ended" event went out (the end shows twice in the core) */
	const char *my_number;		/* awvoice.main.my_number, for the status reply */
	unsigned int channel, at_interface;
	struct calllog *calls;		/* the call log (the daemon keeps it up to date from the core's call records) */
	const struct phonebook *book;	/* the phone book, for the names in the call log reply (may be NULL) */
	struct phone *phone;		/* NULL while the line is not in service (status then reports state "down") */
	const char *port;		/* the modem port in use, "" when none */
	const char *record_path;	/* awvoice.record_path, where the call recordings live ("" = none listed) */
	const char *message_path;	/* awvoice.machine_path, where the answering machine's messages live ("" = none listed) */
	struct reply_storage record_store;	/* the recordings folder's last check (the daemon's storage_measure) */
	struct reply_storage message_store;	/* the messages folder's last check */
	const struct indicator *ind;	/* the phone LED's state, for the health reply (may be NULL: none yet) */
	const struct machine *machine;	/* the answering machine, for the health reply (NULL: none) */
	unsigned int machine_delay_s;	/* the wait before the machine answers, for the health reply */
	int hd;				/* the HD voice state for the health reply: 0 off, 1 wanted, 2 on */
	char leds_buf[2048];		/* the /sys/class/leds names for the health reply, filled at the reply */
	const char *leds[32];
	int nleds;			/* the count in leds, or a negative errno from the scan */
	unsigned long long start_ms;	/* the daemon's start (monotonic) */
	int (*bridge_stats)(void *ctx, void *out);	/* fills struct awfxs_bridge_stats; 0 or -errno */
	int (*reload)(void *ctx);	/* the daemon's reload */
	void *ctx;
};

/* register the object on ctx; 0 or a UBUS_STATUS code. The connection comes from ubus_auto_connect, which registers the object again after a reconnect by itself. */
int api_init(struct ubus_context *ubus, struct api_ctx *a);

/* called after every step: sends the reply of a line test that has finished (the method answers late, when its samples are taken) */
void api_tick(struct api_ctx *a);

/* the phone_hooks.event target: turns a core event into a ubus event (never blocks; drops the event when ubus is down) */
void api_event(void *ctx, const struct phone_event *ev);

/* monotonic milliseconds of the core to epoch seconds (the call log's ts, M5): now_epoch - (now_mono - ms) / 1000 */
unsigned long long api_epoch_s(unsigned long long mono_ms, unsigned long long now_mono_ms);

#endif /* AW_PHONED_API_H */
