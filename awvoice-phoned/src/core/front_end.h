/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONE_CORE_FRONT_END_H
#define AW_PHONE_CORE_FRONT_END_H

#include <errno.h>
#include <stdint.h>

#include "phone.h"	/* enum phone_line, enum phone_dir, PHONE_NUMBER_MAX */

typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/*
 * The front-end contract: the 20 ms frame a telephony front-end speaks, the event set the stage machine publishes,
 * the state and failure vocabularies, the ring-owner rules of the daemon's audio fan-out, and the shape of an action
 * a front-end asks with. The rules are transport-neutral, so they live here and not in any one transport.
 *
 * The unit: the kernel bridge hands out one frame per 125 us slot at either rate, so 160 slots make one 20 ms frame
 * and a 16 kHz frame carries two time samples per slot. fe_capture and fe_play_expand are the only places that unit
 * is converted; nothing else may count slots.
 *
 * A number never goes into an event: `withheld` is a flag, and the digits answer call_info and the arbiter's own
 * status reply only.
 *
 * Host-tested (tests/test_front_end.c). A change to this file is its own commit, and every caller is rebuilt with it.
 */

#define FE_IF_VERSION		1u	/* the version of this rules set; a transport that cannot match it is refused, never guessed */
#define FE_FRAME_MS		20u
#define FE_SLOT_US		125u	/* one kernel bridge frame: the tap's and the play ring's unit, at either rate */
#define FE_SLOTS_PER_FRAME	160u	/* 160 x 125 us = the contract's 20 ms */
#define FE_SAMPLES_8K		160u
#define FE_SAMPLES_16K		320u
#define FE_FRAME_SAMPLES	FE_SAMPLES_16K	/* the largest frame; `count` says what this one holds */
#define FE_TAP_SLOTS		8192u	/* the capture ring's depth as the drain reads it (one second at 8 kHz) */
#define FE_PLAY_SLOTS		4096u	/* the playback ring's depth: 512 ms, 25 complete frames plus 96 slots, at either rate */

/* one 20 ms frame toward or from a front-end, at the call's native rate */
struct fe_frame {
	u32 sequence;		/* frames since the call began; the timestamp is sequence * FE_FRAME_MS */
	u32 rate;		/* 8000 or 16000 */
	u32 count;		/* samples in `sample`: FE_SAMPLES_8K or FE_SAMPLES_16K */
	int16_t sample[FE_FRAME_SAMPLES];
};

/* one 125 us slot, field for field the layout of the UAPI's struct awfxs_tap_frame / awfxs_play_frame: the modem's
 * sample, the line chip's, and their wideband seconds (0 on a narrowband call). The caller copies field by field;
 * the two structs are not one type and are never cast. */
struct fe_slot {
	u16 modem;
	u16 line;
	u16 modem2;
	u16 line2;
};

/* the cut from the tap's slots into 20 ms frames: 160 slots always, two samples per slot at 16 kHz */
struct fe_capture {
	u32 rate;
	int wideband;		/* the tap's flag in force for this generation: at 16 kHz it decides the second sample */
	u32 slot;		/* slots collected toward the next frame */
	u32 sequence;
	struct fe_frame frame;
};

/* start (or restart, after a generation bump) the cut: 0, -EINVAL when the rate is not 8000 or 16000, or when it is
 * 16000 and the tap reports narrowband (a bridge fault: the caller re-handshakes rather than cut short frames) */
static inline int fe_capture_init(struct fe_capture *c, u32 rate, int wideband)
{
	if (rate != 8000 && rate != 16000)
		return -EINVAL;
	if (rate == 16000 && !wideband)
		return -EINVAL;
	c->rate = rate;
	c->wideband = wideband;
	c->slot = 0;
	c->sequence = 0;
	c->frame.sequence = 0;
	c->frame.rate = rate;
	c->frame.count = rate == 16000 ? FE_SAMPLES_16K : FE_SAMPLES_8K;
	return 0;
}

/* one tap slot in; 1 when it completed a frame (it is in c->frame), 0 while it fills */
static inline int fe_capture_push(struct fe_capture *c, const struct fe_slot *s)
{
	u32 k = c->slot;

	if (c->rate == 16000) {
		c->frame.sample[2 * k] = (int16_t)s->modem;
		c->frame.sample[2 * k + 1] = (int16_t)s->modem2;
	} else {
		c->frame.sample[k] = (int16_t)s->modem;
	}
	if (++c->slot < FE_SLOTS_PER_FRAME)
		return 0;
	c->frame.sequence = c->sequence++;
	c->slot = 0;
	return 1;
}

/* the call ended or the rate changed: the slots held become one last frame, padded with silence so a consumer never
 * sees a short frame. 1 and *out filled when any were held, 0 otherwise */
static inline int fe_capture_flush(struct fe_capture *c, struct fe_frame *out)
{
	u32 k;

	if (!c->slot)
		return 0;
	for (k = c->slot; k < FE_SLOTS_PER_FRAME; k++) {
		if (c->rate == 16000)
			c->frame.sample[2 * k] = c->frame.sample[2 * k + 1] = 0;
		else
			c->frame.sample[k] = 0;
	}
	c->frame.sequence = c->sequence++;
	c->slot = 0;
	*out = c->frame;
	return 1;
}

/* one 20 ms frame toward the network, expanded into the play ring's slots: `modem` is the network's sample, `line`
 * stays silent (the handset is not part of a bridged call), and at 16 kHz the second time sample rides in
 * modem2. Returns the slots written (160), or 0 when the frame is not one this unit writes (rate and count disagree). */
static inline u32 fe_play_expand(const struct fe_frame *f, struct fe_slot *out)
{
	u32 k, wide;

	if (f->rate != 8000 && f->rate != 16000)
		return 0;
	wide = f->rate == 16000;
	if (f->count != (wide ? FE_SAMPLES_16K : FE_SAMPLES_8K))
		return 0;
	for (k = 0; k < FE_SLOTS_PER_FRAME; k++) {
		out[k].modem = (u16)f->sample[wide ? 2 * k : k];
		out[k].line = 0;
		out[k].modem2 = wide ? (u16)f->sample[2 * k + 1] : 0;
		out[k].line2 = 0;
	}
	return FE_SLOTS_PER_FRAME;
}

/* The state the arbiter reports. Only six are ever published: the line has no early-media
 * stage, and recovering is a modem reconcile with no call; the names exist because the contract's vocabulary does. */
enum fe_state {
	FE_STATE_DISABLED,		/* the line's front end may not run */
	FE_STATE_IDLE,
	FE_STATE_OUTGOING_SETUP,
	FE_STATE_INCOMING_RINGING,
	FE_STATE_EARLY_MEDIA,
	FE_STATE_ACTIVE,
	FE_STATE_TERMINATING,
	FE_STATE_RECOVERING,
	FE_STATE_FAULT,
};

/* the line chip's state as the front-end knows it; dial tone and digit collection are before a call exists */
static inline enum fe_state fe_state_of(int off, enum phone_line line)
{
	if (off)
		return FE_STATE_DISABLED;
	switch (line) {
	case PHONE_LINE_RINGING:
		return FE_STATE_INCOMING_RINGING;
	case PHONE_LINE_SETUP:
		return FE_STATE_OUTGOING_SETUP;
	case PHONE_LINE_CALL:
		return FE_STATE_ACTIVE;
	case PHONE_LINE_RELEASE:
		return FE_STATE_TERMINATING;
	case PHONE_LINE_NO_SERVICE:
		return FE_STATE_FAULT;
	default:
		return FE_STATE_IDLE;	/* idle, dial tone, digit collection */
	}
}

/* the front end follows the call, not the line: AT+CLCC is a snapshot (3GPP TS 27.007 7.18) and a line that
 * reads idle between the answer and the call list is not a released call. The line is only the phase. */
static inline enum fe_state fe_state_of_call(int off, enum call_state call, enum phone_line line)
{
	if (off)
		return FE_STATE_DISABLED;
	if (line == PHONE_LINE_NO_SERVICE)
		return FE_STATE_FAULT;
	switch (call) {
	case CALL_INCOMING:
		return FE_STATE_INCOMING_RINGING;
	case CALL_ACTIVE:
	case CALL_HELD:
		return FE_STATE_ACTIVE;
	case CALL_DIALING:
	case CALL_ALERTING:
		return FE_STATE_OUTGOING_SETUP;
	case CALL_GONE:
	case CALL_NONE:
		if (line == PHONE_LINE_RINGING)
			return FE_STATE_INCOMING_RINGING;
		if (line == PHONE_LINE_RELEASE)
			return FE_STATE_TERMINATING;
		return FE_STATE_IDLE;
	}
	return FE_STATE_IDLE;
}

static inline const char *fe_state_name(enum fe_state s)
{
	switch (s) {
	case FE_STATE_DISABLED:
		return "disabled";
	case FE_STATE_OUTGOING_SETUP:
		return "outgoing_setup";
	case FE_STATE_INCOMING_RINGING:
		return "incoming_ringing";
	case FE_STATE_EARLY_MEDIA:
		return "early_media";
	case FE_STATE_ACTIVE:
		return "active";
	case FE_STATE_TERMINATING:
		return "terminating";
	case FE_STATE_RECOVERING:
		return "recovering";
	case FE_STATE_IDLE:
		return "idle";
	case FE_STATE_FAULT:
		break;
	}
	return "fault";		/* the contract maps an unknown state to fault, not to idle */
}

/* who answered the call: the handset, the answering machine, the page's own endpoint, or the installed service's leg */
enum fe_answer_owner { FE_ANSWER_NONE, FE_ANSWER_HANDSET, FE_ANSWER_MACHINE, FE_ANSWER_WEB, FE_ANSWER_SIP };

static inline const char *fe_answer_owner_name(enum fe_answer_owner o)
{
	switch (o) {
	case FE_ANSWER_HANDSET:
		return "handset";
	case FE_ANSWER_MACHINE:
		return "machine";
	case FE_ANSWER_WEB:
		return "web";
	case FE_ANSWER_SIP:
		return "sip";
	case FE_ANSWER_NONE:
		break;
	}
	return "none";
}

/* the awvoice.call topic's `event` field, and the arbiter's own event names */
enum fe_event_kind { FE_EV_RING, FE_EV_ACTIVE, FE_EV_RELEASE, FE_EV_FAULT, FE_EV_DTMF };

static inline const char *fe_event_name(enum fe_event_kind e)
{
	switch (e) {
	case FE_EV_RING:
		return "ring";
	case FE_EV_ACTIVE:
		return "active";
	case FE_EV_RELEASE:
		return "release";
	case FE_EV_FAULT:
		return "fault";
	case FE_EV_DTMF:
		break;
	}
	return "dtmf";
}

/* why a call ended: a refusal code never ends a call, and adapter-gone is only ever a reason, never a refusal */
enum fe_reason { FE_REASON_NONE, FE_REASON_LOCAL, FE_REASON_REJECTED, FE_REASON_HANDSET, FE_REASON_ADAPTER_GONE };

static inline const char *fe_reason_name(enum fe_reason r)
{
	switch (r) {
	case FE_REASON_LOCAL:
		return "local";
	case FE_REASON_REJECTED:
		return "rejected";
	case FE_REASON_HANDSET:
		return "handset";
	case FE_REASON_ADAPTER_GONE:
		return "adapter-gone";
	case FE_REASON_NONE:
		break;
	}
	return "";
}

/* the failure vocabulary, one fixed word each; the same word answers a ubus reply, an arbiter refusal, a
 * QMSK ERROR frame and sip.lastRefusal */
enum fe_refusal {
	FE_REFUSE_NONE = 0,
	FE_REFUSE_BUSY,		/* refused-busy: the ring already has another owner */
	FE_REFUSE_NOCALL,	/* refused-nocall: no call to attach to, or it ended since */
	FE_REFUSE_REVISION,	/* refused-revision: the action named a call that is not the current one */
	FE_REFUSE_MODE,		/* refused-mode: the line's front end may not run (the guards refused the line, nothing was checked yet, or the request named an endpoint this build does not serve) */
	FE_REFUSE_RATE,		/* refused-rate: not 8000/16000, or more than the call carries */
	FE_REFUSE_PORT,		/* refused-port: the voice AT port has another opener */
	FE_REFUSE_OBJECT,	/* refused-object: qmodem_voip is already registered by someone else */
	FE_DROP_SLOW,		/* dropped-slow: the consumer fell behind, or its playback ran dry */
	FE_CALL_ENDED,		/* call-ended: the call ended and the stream closed; nothing failed */
	FE_INTERNAL,		/* internal: unexpected */
};

static inline const char *fe_refusal_name(enum fe_refusal r)
{
	switch (r) {
	case FE_REFUSE_BUSY:
		return "refused-busy";
	case FE_REFUSE_NOCALL:
		return "refused-nocall";
	case FE_REFUSE_REVISION:
		return "refused-revision";
	case FE_REFUSE_MODE:
		return "refused-mode";
	case FE_REFUSE_RATE:
		return "refused-rate";
	case FE_REFUSE_PORT:
		return "refused-port";
	case FE_REFUSE_OBJECT:
		return "refused-object";
	case FE_DROP_SLOW:
		return "dropped-slow";
	case FE_CALL_ENDED:
		return "call-ended";
	case FE_INTERNAL:
		return "internal";
	case FE_REFUSE_NONE:
		break;
	}
	return "";
}

/* a retry is triggered by the event that changes the condition, never by a timer */
static inline int fe_refusal_retry(enum fe_refusal r)
{
	return r == FE_DROP_SLOW || r == FE_CALL_ENDED;
}

/* one published change. It carries no number; `withheld` says a number exists but the network hid it. */
struct fe_event {
	enum fe_event_kind event;
	unsigned int id;		/* the call's id, the topic's `id` (0 for a dtmf outside a call) */
	enum phone_dir dir;		/* PHONE_DIR_IN / PHONE_DIR_OUT */
	enum phone_line phase;		/* the line's state after the change, the topic's `phase` today */
	enum fe_answer_owner owner;
	int withheld;
	u64 revision;
	unsigned int duration_s;
	enum fe_reason reason;		/* release only */
	char digit;			/* dtmf only */
};

/* the call's public view: what call_info answers and the arbiter's status publishes */
struct fe_call {
	unsigned int id;		/* 0 = no call */
	enum fe_state state;
	enum phone_dir dir;
	enum fe_answer_owner owner;
	int withheld;
	u32 rate;			/* the call's native rate, 0 before it is known */
	u64 revision;			/* increments on every state change */
	unsigned int duration_s;
};

static inline void fe_call_init(struct fe_call *c)
{
	c->id = 0;
	c->state = FE_STATE_IDLE;
	c->dir = PHONE_DIR_NONE;
	c->owner = FE_ANSWER_NONE;
	c->withheld = 0;
	c->rate = 0;
	c->revision = 1;
	c->duration_s = 0;
}

/* 1 when the state changed (the revision moves with it), 0 when it did not */
static inline int fe_call_state(struct fe_call *c, enum fe_state s)
{
	if (c->state == s)
		return 0;
	c->state = s;
	c->revision++;
	return 1;
}

/* the action a front-end asks with: the 5.1 operations carry the revision they saw */
enum fe_action_kind { FE_ACT_ANSWER, FE_ACT_DIAL, FE_ACT_HANGUP, FE_ACT_DTMF, FE_ACT_ROUTE, FE_ACT_INFO };

struct fe_action {
	enum fe_action_kind kind;
	u64 revision;				/* the call revision the caller saw */
	char number[PHONE_NUMBER_MAX + 1];	/* dial: the digits, the core's own rule (digits, *, #, a leading +) */
	char digit;				/* dtmf: one 0-9, *, #, A-D */
};

/* answer, hangup and dtmf need a call; dial and call_info do not */
static inline int fe_action_needs_call(enum fe_action_kind k)
{
	return k == FE_ACT_ANSWER || k == FE_ACT_HANGUP || k == FE_ACT_DTMF;
}

/* The gate every action passes before the stage machine sees it. A refusal never changes the call; a hangup for a
 * call that already ended is accepted, because the intent is satisfied and a human hanging up at the same moment is
 * not an error - so a hangup is never refused for having nothing to end. Answering a call that is not ringing is
 * refused-revision, never answered early. */
static inline enum fe_refusal fe_action_check(enum fe_action_kind kind, u64 asked, const struct fe_call *c)
{
	if (!c->id)
		return fe_action_needs_call(kind) && kind != FE_ACT_HANGUP ? FE_REFUSE_NOCALL : FE_REFUSE_NONE;
	if (kind == FE_ACT_ANSWER && c->state != FE_STATE_INCOMING_RINGING)
		return FE_REFUSE_REVISION;
	if (asked == c->revision)
		return FE_REFUSE_NONE;
	if (kind == FE_ACT_HANGUP && (c->state == FE_STATE_IDLE || c->state == FE_STATE_DISABLED ||
			c->state == FE_STATE_TERMINATING))
		return FE_REFUSE_NONE;
	return FE_REFUSE_REVISION;
}

/* A front-end may take less than the call carries (it decimates 16 kHz to 8 kHz), never more. */
static inline enum fe_refusal fe_rate_check(u32 wanted, const struct fe_call *c)
{
	if (wanted != 8000 && wanted != 16000)
		return FE_REFUSE_RATE;
	if (c->rate && wanted > c->rate)
		return FE_REFUSE_RATE;
	return FE_REFUSE_NONE;
}

/* The rings. The capture tap has one drain; the sinks below say which of them are armed on it. The
 * playback ring has exactly one owner: none, the machine's greeting, the front-end, or the page's own browser
 * audio (a front-end-class owner that the handset and the SIP consumer share the ladder with). */
enum fe_play_owner { FE_PLAY_NONE, FE_PLAY_GREETING, FE_PLAY_FRONT_END, FE_PLAY_WEB };

#define FE_SINK_RECORDER	0x01u
#define FE_SINK_MACHINE		0x02u
#define FE_SINK_FRONT_END	0x04u
#define FE_SINK_WEB		0x08u

struct fe_owners {
	enum fe_play_owner play;
	unsigned int sink;		/* FE_SINK_* bits: who needs the tap's frames */
};

/* 0 granted, -EBUSY refused (the front-end is told refused-busy). The greeting always wins because it is mid-file:
 * claiming over a front-end owner replaces it, and the caller must then tell the front-end its owner changed. */
static inline int fe_play_claim(struct fe_owners *o, enum fe_play_owner want)
{
	if (want == FE_PLAY_NONE) {
		o->play = FE_PLAY_NONE;
		return 0;
	}
	if (o->play == want)
		return 0;		/* the same owner asking again */
	if (o->play != FE_PLAY_NONE && want != FE_PLAY_GREETING)
		return -EBUSY;
	o->play = want;
	return 0;
}

/* the handset took the call over, or the front-end's playback ran dry: the owner goes to none in the same step */
static inline void fe_play_release(struct fe_owners *o)
{
	o->play = FE_PLAY_NONE;
}

static inline void fe_sink_set(struct fe_owners *o, unsigned int bits, int on)
{
	if (on)
		o->sink |= bits;
	else
		o->sink &= ~bits;
}

/* the tap must be armed while any sink wants its frames */
static inline int fe_tap_needed(const struct fe_owners *o)
{
	return o->sink != 0;
}

/* The one word the health key sip.ringOwner publishes: the greeting while it plays, else the front-end
 * when SIP audio is present, else the page's browser audio, else the recorder. The failure ends - records but no
 * front-end audio, front-end audio but no recording - read as different words. */
static inline const char *fe_ring_owner_name(const struct fe_owners *o)
{
	if (o->play == FE_PLAY_GREETING)
		return "greeting";
	if ((o->sink & FE_SINK_FRONT_END) || o->play == FE_PLAY_FRONT_END)
		return "front-end";
	if ((o->sink & FE_SINK_WEB) || o->play == FE_PLAY_WEB)
		return "web";
	if (o->sink & (FE_SINK_RECORDER | FE_SINK_MACHINE))
		return "recorder";	/* the machine's message is a recording of the call in all but name */
	return "idle";
}

/*
 * The module, implemented once in front_end.c. The daemon owns one; the sink callbacks are how the transport
 * receives what the rules publish, and they must not block and must not call back in (they run inside the 5 ms step).
 */
struct fe;

struct fe_sink {
	void *ctx;
	void (*event)(void *ctx, const struct fe_event *ev);
	void (*capture)(void *ctx, const struct fe_frame *fr);	/* one 20 ms far-end frame toward the front-end */
};

int fe_open(struct fe **out, const struct fe_sink *sink, int off);	/* 0 or -errno */
void fe_close(struct fe *f);
void fe_set_off(struct fe *f, int off);
/* NULL or zeroed = no front-end attached: the cut stops and the frames are dropped */
void fe_set_sink(struct fe *f, const struct fe_sink *sink);
const struct fe_call *fe_call_view(const struct fe *f);
struct fe_owners *fe_owners(struct fe *f);
/* The stage machine moved: update the view and publish the one event the change is - ring when a call starts
 * ringing, active when it becomes the line's call, release when the call ends, fault when the line falls to no
 * service. A state change with no event still moves the revision. 1 when an event went out, 0 otherwise. */
int fe_observe(struct fe *f, enum phone_line line, enum call_state call, enum phone_dir dir, unsigned int call_id,
	enum fe_answer_owner owner, int withheld, unsigned int duration_s, enum fe_reason reason);
/* the digit went out on the cellular leg: publish it (the digit itself, never a number) */
void fe_dtmf(struct fe *f, char digit);
/* A bridge generation bump (a start, an HD switch, a restart): the part held is flushed to the sink as one padded
 * frame, then the cut restarts at this rate. 0, -EINVAL for a rate or wideband flag the bus cannot produce. */
int fe_capture_reset(struct fe *f, u32 rate, int wideband);
/* The fan-out's slots in, in the order the tap read them; completed frames go to the sink, the tail stays held. */
void fe_tap(struct fe *f, const struct fe_slot *slots, u32 n);

#endif /* AW_PHONE_CORE_FRONT_END_H */
