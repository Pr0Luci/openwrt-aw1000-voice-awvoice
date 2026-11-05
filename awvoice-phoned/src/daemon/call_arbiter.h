/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_CALL_ARBITER_H
#define AW_PHONED_CALL_ARBITER_H

#include <stdio.h>

#include "front_end.h"
#include "qmsk_server.h"
#include "reply.h"

/*
 * The qmodem_voip object and the qmodem_voip.call topic: the rules' call view as the replies and events the installed
 * SIP service reads, and a method call as a request on the stage machine. The same status is the page's own view (the
 * awvoice call_status method), so the page needs no object. The replies are written against struct reply_sink, so the
 * daemon gives them a blobmsg sink and the host test a JSON text sink. Nothing here registers an object or opens a
 * socket, and nothing here blocks.
 *
 * status answers the whole field set every time, so a consumer that waits for one state never has to ask twice:
 *   state                   fe_state_name() of the call view's state
 *   origin                  "cellular": the network is the only origin
 *   endpoint                the leg that answered: "browser" for this page, "lan_sip" for the installed service's
 *                           call, "cellular" for a call the handset or the machine took
 *   number_present          1 when the call has a number or the network withheld one
 *   caller_id_withheld      the call view's flag
 *   remote_number           the number in full, "" when withheld or no call; it appears in this reply only, never in
 *                           an event or a log line
 *   call_duration_seconds   the call view's duration
 *   enabled                 1 while the line's front end runs (the guards leave the line to it)
 *   held                    1 while one of the line's calls is held by the network (a flash put it there)
 *   waiting                 1 while the network lists another call waiting beside the active one
 *   revision                the call view's revision
 *   restart_epoch           the value the caller set at start: it changes when the daemon restarts
 *   sequence                the events published on the topic so far
 *   drop_count              media frames dropped for a front-end so far
 *   reconcile_pending       0: this build has no reconciler
 *   answer_owner            none, handset, machine, web or sip
 *   media_engine            "ready" after a capture frame and a playback frame in the current revision, else "idle"
 *   media_capture_frames    frames toward the front-end in the current revision (0 once the call moved on)
 *   media_playback_frames   frames from the front-end in the current revision
 *
 * capabilities answers status:"ok", media:"qmsk", sample_rate:8000, frame_ms:20, frame_samples:160, wideband:0,
 * calls:1 and endpoints ["lan_sip","cellular","browser"].
 *
 * An event's payload is `event` plus the same field set. The event word is ring, release or call_state (the active and
 * fault states are call_state); a digit is not published on this topic. The sequence climbs with every published event.
 *
 * A method call answers { ok, refused: null|"<word>" }; refused is null when it acted, else one of the refusal words
 * below. The call returns 0 when it acted and a negative errno for a refusal: the core's own errno when the core
 * refused, else -EPERM for refused-mode, -ENOENT for refused-nocall, -EBUSY for refused-busy, -ESTALE for
 * refused-revision and -EOPNOTSUPP for NOT_AVAILABLE.
 *
 * With the line's front end off every action is refused-mode and the stage machine is not touched: a side that asked
 * anyway does not move the handset's line. The object itself exists only while the SIP feature is in use and the
 * guards leave the line to the daemon (main.c's sip_apply), so a mode that serves no service publishes no object.
 *
 * Methods: status and capabilities take no arguments. originate takes endpoint ("lan_sip", "cellular" and "browser" are
 * served; any other word is refused-mode) and number. answer takes a ringing call, and takes over a call the answering
 * machine holds: the caller becomes this front end's, with nothing sent to the modem because the line is already in
 * the call. reject ends a ringing call and
 * remembers FE_REASON_REJECTED for the release that follows. hangup with nothing to end is accepted: the intent is
 * satisfied. send_dtmf takes one digit into a live call. issue_socket_session takes call_revision and answers session_id,
 * the id of a media binding minted for that revision at the caller's clock; eight live bindings are the most a mint
 * allows. flash is the page's own door alone (the object does not serve it): the hook flash a person at the handset
 * could make, hold, swap, take the waiting call or take the held call back; a digit within its window is a code.
 */

#define SIP_OBJECT		"qmodem_voip"
#define SIP_EVENT_TOPIC		"qmodem_voip.call"

enum sip_method {
	SIP_M_NONE,
	SIP_M_STATUS,
	SIP_M_CAPABILITIES,
	SIP_M_ORIGINATE,
	SIP_M_ANSWER,
	SIP_M_REJECT,
	SIP_M_HANGUP,
	SIP_M_SEND_DTMF,
	SIP_M_FLASH,
	SIP_M_ISSUE_SOCKET_SESSION,
};

/* the method a registered ubus object's name is, SIP_M_NONE for one this object does not serve */
enum sip_method sip_method_of(const char *name);

/* one method call's arguments; only the fields of the method you call carry anything */
struct sip_args {
	const char *endpoint;		/* originate */
	const char *number;		/* originate */
	const char *origin;		/* originate, not read in this build */
	const char *username;		/* originate, not read in this build */
	char digit;			/* send_dtmf */
	int via_sip;			/* answer: the installed service's door, not the page's own */
	u64 call_revision;		/* issue_socket_session */
};

/* what the arbiter asks of the stage machine: the daemon's own functions in the daemon, a fake in the host test */
struct ctl_ops {
	void *ctx;
	int (*answer)(void *ctx, int via_sip);		/* 0 or -errno */
	int (*takeover)(void *ctx, int via_sip);	/* the call the answering machine holds, for this front end: 0 or -errno */
	int (*dial)(void *ctx, const char *number, int via_sip);	/* 0 or -errno */
	int (*hangup)(void *ctx);			/* 0 or -errno */
	int (*dtmf)(void *ctx, char digit);		/* 0 or -errno */
	int (*flash)(void *ctx);			/* the hook flash: 0 or -errno */
	void (*status)(void *ctx, struct phone_status *out);
	int (*history)(void *ctx, const struct reply_sink *out);	/* the call history, or NULL */
	u64 (*now_ms)(void *ctx);			/* a monotonic clock in milliseconds */
};

struct call_arbiter {
	struct fe *fe;				/* the rules' view of the call */
	struct qmsk_bindings *bindings;		/* the media ids are minted here */
	const struct ctl_ops *core;
	int off;				/* the line's front end may not run (the guards refused the line) */
	unsigned int restart_epoch;		/* set at start: changes when the daemon restarts */
	unsigned int sequence;			/* events published */
	unsigned int drop_count;		/* media frames dropped for a front-end */
	u64 media_revision;			/* the revision the counters below belong to */
	unsigned int capture_frames, playback_frames;
	enum fe_reason pending_reason;		/* reject and hangup: why the call is ending */
	char last_refusal[24];			/* the last refusal word written to the log */
	u64 last_refusal_ms;			/* the clock of that line: the word is not repeated within a second */
};

void arbiter_init(struct call_arbiter *f, struct fe *fe, struct qmsk_bindings *bindings, const struct ctl_ops *core);
void arbiter_set_off(struct call_arbiter *f, int off);
void arbiter_status(const struct call_arbiter *f, const struct reply_sink *out);
void arbiter_event(struct call_arbiter *f, const struct fe_event *ev, const struct reply_sink *out);
int arbiter_request(struct call_arbiter *f, enum sip_method m, const struct sip_args *a, const struct reply_sink *out, FILE *log);
/* a media frame crossed: capture toward the front-end or playback from it */
void arbiter_media(struct call_arbiter *f, int capture, int playback);
void arbiter_dropped(struct call_arbiter *f);
enum fe_reason arbiter_take_reason(struct call_arbiter *f);
const struct fe_call *arbiter_view(const struct call_arbiter *f);

#endif /* AW_PHONED_CALL_ARBITER_H */
