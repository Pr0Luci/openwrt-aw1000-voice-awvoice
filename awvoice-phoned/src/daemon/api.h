/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_API_H
#define AW_PHONED_API_H

#include <libubox/uloop.h>
#include <libubus.h>

#include "../core/phone.h"
#include "calllog.h"
#include "reply.h"

/*
 * The ubus object `awvoice` (libubus, blobmsg). The phone page's provider (`phone/live.js` of luci-app-awvoice) calls it
 * through rpcd; the app's ACL grants the methods. The reply shapes below are the contract between this object and that
 * provider, field for field, so the provider passes them on without reshaping.
 *
 * The line's own methods:
 *   status     {}  -> Status:  { state, line, hook, machine, call: null | { id, direction, number, phase, elapsedSec },
 *                               voiceNetwork: { registered, ims }, signal: { rsrpDbm, network }, service: { running }, myNumber }
 *                     machine: "none" | "answering" | "answered" | null (the answering machine's part in the call)
 *                     state from enum phone_line: idle -> "idle"; dial_tone, dialling -> "offhook"; ringing -> "ring";
 *                     setup, call, release -> "call"; no_service -> "noservice" ("down" is the provider's, when ubus fails)
 *                     phase: incoming -> "ringing", dialing -> "dialing", alerting -> "alerting", active -> "active"
 *                     elapsedSec: (now - answered_ms) / 1000, 0 before the answer; null JSON for unknown tri-states
 *   health     {}  -> Health without log.tail: { atPort, line, modem (modem carries hdVoice 'off'|'set'|'on', audioRate, voiceLayout 8000|16000), system, indicator { led, state, leds: [] }, counters,
 *                     machine { greetingLoaded, greetingMs, greetingError, delaySec },
 *                     storage { recordings { path, ready, freeMb, error }, messages { ... } },
 *                     web { mode, audio, port, address }, sip { objectOwner, lastRefusal, consumerPid } }
 *                     web is the line's own side: the mode the settings ask for and the browser-audio listener's
 *                     state, its port and the address it is bound to (\"\" while not listening); sip is the installed
 *                     service's side: who holds the qmodem_voip name, the last refusal word and the attached consumer
 *                     modem also holds the caller-id restriction read back from the modem: clir "network"|"hide"|"show"|null and
 *                     clirStatus "not_provisioned"|"provisioned"|"unknown"|"restricted"|"allowed"|null
 *                     machine.greetingLoaded is what the daemon holds, whatever the switch says: the machine answers when it
 *                     is true and the settings ask for it. machine.greetingMs is its length; machine.greetingError is the one
 *                     word for why there is no greeting - "" when one is loaded, else MISSING | FORMAT | EMPTY | TRUNCATED |
 *                     TOO_BIG | MEMORY | UNREADABLE - the same words the upload methods refuse with
 *                     storage: the folders' state as of the last check (start-up, a reload, a call's start); ready false with
 *                     error set to the system's reason (a missing mount, a read-only filesystem) means no file can be kept
 *   call_status {} -> the call's own view, the same fields the qmodem_voip status carries: { state, origin, endpoint,
 *                     number_present, caller_id_withheld, remote_number, call_duration_seconds, enabled, revision,
 *                     restart_epoch, sequence, drop_count, reconcile_pending, answer_owner, media_engine,
 *                     media_capture_frames, media_playback_frames }
 *                     the page reads its call card here, so its own phone needs no SIP object and no mode; the number
 *                     appears in this reply only
 *   hangup     {}                  -> { ok, refused: null|"NO_CALL" }
 *   ring_test  { seconds: 1..10 }  -> { ok, refused: null|"ACTIVE_CALL"|"OFF_HOOK" }
 *   reload     {}                  -> { ok }                       the same as a UCI change (SIGHUP)
 * The methods the page's own phone and add-ons use (the ACL of the page: calls is read, calls_delete is write):
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
 *   greeting_get   { offset }          -> { total, offset, b64, more }  one chunk of the greeting's own file, the same
 *                                        24 KiB raw a recording's chunk carries. Refused: NOT_FOUND (no file there),
 *                                        TOO_BIG (beyond the cap). The managed path is one constant (machine.h), so no
 *                                        name travels: a page can only ever read the greeting the machine plays.
 *   greeting_begin {}                  -> { ok, refused }   open the staging of one upload; BUSY when a piece of another
 *                                        is still open, SYMLINK when the greeting's name is a link. Runs during a call:
 *                                        the pieces are not the greeting yet, the way a message is written mid-call.
 *   greeting_put  { offset, b64 }      -> { ok, size, refused }   the next piece at exactly the offset the staging
 *                                        reached; OFFSET for a gap or a repeat, TOO_BIG past the cap, IO for a failed
 *                                        write - each leaves no staging and no tmp. INVALID_ARGUMENT (a ubus error, not
 *                                        a word) when nothing is open: the calls arrived out of order.
 *   greeting_commit { polish? }        -> { ok, ms, refused }   prove the staged bytes as the machine would and put them
 *                                        in the greeting's place. `polish` is 1 for a fresh recording: the silence off
 *                                        both ends goes and the 1 kHz tone is added, which is what a hand-stopped
 *                                        recording still needs; a file someone uploaded keeps the audio its author made.
 *                                        Refused only while the line is not quiet or the greeting is in flight (BUSY),
 *                                        FORMAT | EMPTY | TRUNCATED | TOO_BIG | MEMORY | UNREADABLE when the bytes are
 *                                        not a greeting, SYMLINK, IO. A refusal leaves the old file and the old
 *                                        greeting loaded; on success the core is told, so the machine answers from the
 *                                        next call without a settings save first.
 *   greeting_abort {}                  -> { ok }             drop the staging and the tmp; success with nothing staged
 *   web_originate { number }   -> { ok, refused }   the page's own call control, through the line's call arbiter
 *   web_answer / web_reject / web_hangup {}            (endpoint browser): ok is true when the action was taken,
 *   web_dtmf { digit }                                 refused is the arbiter's word otherwise. Always a
 *   web_flash {}                                       successful call, so rpcd hands the body on. flash is the
 *                                                      hook flash: hold, swap, take the waiting call, take the held
 *                                                      call back; a digit within its window is a code
 *   web_audio_open { call_revision } -> { ok, refused, token, port, rate, frame_ms, expires_ms }
 *                                        the page's browser-audio token: single use, 30 s, bound to the call revision
 *
 * Events (ubus_send_event), never with a number:
 *   awvoice.line  { state, line }
 *   awvoice.call  { id, direction, phase }      phase as above plus "ended"
 *
 * Not registered yet: loopback_test, and the add-on boundary's dial, answer, route, call_info. Service start / stop /
 * restart are the LuCI app's rpcd script.
 */

struct ubus_context;

struct phonebook;
struct indicator;
struct machine;
struct machine_upload;
struct call_arbiter;
struct sip_guard_state;
struct dnd;
struct fe_event;
struct web_audio;

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
	const struct dnd *dnd;		/* the quiet hours the daemon holds, for the health reply (NULL: none read) */
	struct call_arbiter *arbiter;		/* the call's arbiter and published view (call_arbiter.h); NULL: not wired */
	const struct sip_guard_state *guard;	/* the line guards' last run, for the health reply */
	const char *sip_mode;		/* awvoice.sip.mode, as the health reply shows it */
	const char *sip_refusal;	/* the object's last refusal word ("" = none) */
	unsigned int sip_consumer_pid;	/* the front end that attached a media binding (0 = none) */
	struct web_audio *web;		/* the page's browser-audio session (web_audio.h); NULL: not listening */
	unsigned int web_port;		/* its port, for web_audio_open's reply */
	const char *web_addr;		/* its bound address, for health web.address ("" while not listening) */
	const char *sip_web_audio;	/* health web.audio: off, waiting, listening, connected, or why not */
	int sip_on;			/* the SIP feature is in use and the line is held here: qmodem_voip is registered on the connection */
	int sip_registered;		/* the object is registered on the current connection */
	struct ubus_request_data voip_answer_defer;	/* the installed service's answer, held until the call list shows it active */
	int voip_answer_held;		/* the held reply is still waiting (the installed service asked to answer) */
	struct uloop_timeout voip_answer_timer;	/* the held reply's deadline: no active call by then is a refusal */
	struct machine *machine;	/* the answering machine: the health reply reads it, the greeting methods stage into it (NULL: none) */
	struct machine_upload *greeting_up;	/* the one greeting upload the page may have open (machine.h); NULL: not wired */
	unsigned int machine_delay_s;	/* the wait before the machine answers, for the health reply */
	int hd;				/* the HD voice state for the health reply: 0 off, 1 wanted (the modem has not confirmed the rate, or the line is not brought up at it yet), 2 on */
	int layout;			/* the audio layout the line was brought up with: 0 = 8 kHz, 1 = 16 kHz (the health reply's voiceLayout) */
	const char *region;		/* the numbering plan in force, from the number table: the health reply's line.numbering */
	char leds_buf[2048];		/* the /sys/class/leds names for the health reply, filled at the reply */
	const char *leds[32];
	int nleds;			/* the count in leds, or a negative errno from the scan */
	unsigned long long start_ms;	/* the daemon's start (monotonic) */
	int (*bridge_stats)(void *ctx, void *out);	/* fills struct awfxs_bridge_stats; 0 or -errno */
	int (*reload)(void *ctx);	/* the daemon's reload */
	int (*greeting_may_swap)(void *ctx);	/* 1 when the greeting may be put in place now: the line quiet and nothing playing out of it */
	void (*greeting_applied)(void *ctx);	/* the core's settings pushed again, so the answer rule follows the new greeting */
	void *ctx;
};

/* register the object on ctx; 0 or a UBUS_STATUS code. The connection comes from ubus_auto_connect, which registers the object again after a reconnect by itself. */
int api_init(struct ubus_context *ubus, struct api_ctx *a);

/* called after every step: sends the reply of a line test that has finished (the method answers late, when its samples are taken) */
void api_tick(struct api_ctx *a);

/* the phone_hooks.event target: turns a core event into a ubus event (never blocks; drops the event when ubus is down) */
void api_event(void *ctx, const struct phone_event *ev);

/* the held answer's outcome: the call went active (1) or it ended or timed out (0) */
void api_voip_answer_settle(struct api_ctx *a, int ok);

/* the front end's event target (fe_sink.event): one qmodem_voip.call event, and nothing while the object is not there */
void api_sip_event(struct api_ctx *a, const struct fe_event *ev);

/* the SIP mode was switched: register or unregister qmodem_voip. Before the first connection the flag only stays for the
 * registration that follows it; 0, or -errno from ubus on a live connection. */
int api_sip_set(struct api_ctx *a, int on);

/* monotonic milliseconds of the core to epoch seconds (the call log's ts): now_epoch - (now_mono - ms) / 1000 */
unsigned long long api_epoch_s(unsigned long long mono_ms, unsigned long long now_mono_ms);

#endif /* AW_PHONED_API_H */
