/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
/* The qmodem_voip object's logic and the qmodem_voip.call topic (call_arbiter.h). */
#include <errno.h>
#include <string.h>

#include "call_arbiter.h"

static const char *method_name(enum sip_method m)
{
	switch (m) {
	case SIP_M_STATUS:			return "status";
	case SIP_M_CAPABILITIES:		return "capabilities";
	case SIP_M_ORIGINATE:			return "originate";
	case SIP_M_ANSWER:			return "answer";
	case SIP_M_REJECT:			return "reject";
	case SIP_M_HANGUP:			return "hangup";
	case SIP_M_SEND_DTMF:			return "send_dtmf";
	case SIP_M_FLASH:			return "flash";
	case SIP_M_ISSUE_SOCKET_SESSION:	return "issue_socket_session";
	case SIP_M_NONE:			break;
	}
	return "";
}

enum sip_method sip_method_of(const char *name)
{
	int k;

	if (!name)
		return SIP_M_NONE;
	for (k = SIP_M_STATUS; k <= SIP_M_ISSUE_SOCKET_SESSION; k++)
		if (!strcmp(name, method_name((enum sip_method)k)))
			return (enum sip_method)k;
	return SIP_M_NONE;
}

/* the states a digit or an answer can attach to: the call exists and the line has not let it go */
static int live_state(enum fe_state s)
{
	switch (s) {
	case FE_STATE_OUTGOING_SETUP:
	case FE_STATE_INCOMING_RINGING:
	case FE_STATE_EARLY_MEDIA:
	case FE_STATE_ACTIVE:
	case FE_STATE_TERMINATING:
		return 1;
	case FE_STATE_DISABLED:
	case FE_STATE_IDLE:
	case FE_STATE_RECOVERING:
	case FE_STATE_FAULT:
		break;
	}
	return 0;
}

/* the fields status and every event carry; the caller writes its own first word before it */
static void fields(const struct call_arbiter *f, const struct reply_sink *o)
{
	const struct fe_call *v = fe_call_view(f->fe);
	const char *number = "";
	struct phone_status st;
	unsigned int k;
	int held = 0, waiting = 0;

	memset(&st, 0, sizeof(st));
	if (f->core && f->core->status)
		f->core->status(f->core->ctx, &st);
	for (k = 0; k < st.leg_count; k++) {
		if (st.legs[k].stat == LEG_HELD)
			held = 1;
		if (st.legs[k].stat == LEG_WAITING)
			waiting = 1;
	}
	if (v->id && !v->withheld)
		number = st.number;	/* a number reaches this reply and nowhere else */
	o->text(o->ctx, "state", fe_state_name(v->state));
	o->text(o->ctx, "origin", "cellular");
	/* the leg that carries the call: the page's own endpoint, the installed service's leg, or the cellular line alone */
	o->text(o->ctx, "endpoint", !v->id ? "cellular"
		: v->owner == FE_ANSWER_WEB ? "browser"
		: v->owner == FE_ANSWER_SIP ? "lan_sip" : "cellular");
	o->flag(o->ctx, "number_present", v->id && (v->withheld || st.number[0]));
	o->flag(o->ctx, "caller_id_withheld", v->withheld);
	o->text(o->ctx, "remote_number", number);
	o->number(o->ctx, "call_duration_seconds", (long long)v->duration_s);
	o->flag(o->ctx, "enabled", !f->off);
	o->flag(o->ctx, "held", held);
	o->flag(o->ctx, "waiting", waiting);
	o->number(o->ctx, "revision", (long long)v->revision);
	o->number(o->ctx, "restart_epoch", (long long)f->restart_epoch);
	o->number(o->ctx, "sequence", (long long)f->sequence);
	o->number(o->ctx, "drop_count", (long long)f->drop_count);
	o->flag(o->ctx, "reconcile_pending", 0);
	o->text(o->ctx, "answer_owner", fe_answer_owner_name(v->owner));
	o->text(o->ctx, "media_engine", f->media_revision == v->revision && f->capture_frames && f->playback_frames
		? "ready" : "idle");
	/* the counters of the call that is up: a frame still counted for the last revision is not this call's */
	o->number(o->ctx, "media_capture_frames", f->media_revision == v->revision ? f->capture_frames : 0);
	o->number(o->ctx, "media_playback_frames", f->media_revision == v->revision ? f->playback_frames : 0);
}

void arbiter_status(const struct call_arbiter *f, const struct reply_sink *out)
{
	out->text(out->ctx, "status", "ok");
	fields(f, out);
}

void arbiter_event(struct call_arbiter *f, const struct fe_event *ev, const struct reply_sink *out)
{
	const char *word;

	switch (ev->event) {
	case FE_EV_RING:
		word = "ring";
		break;
	case FE_EV_RELEASE:
		word = "release";
		break;
	case FE_EV_ACTIVE:
	case FE_EV_FAULT:
		word = "call_state";
		break;
	case FE_EV_DTMF:
		return;		/* a digit is not a state change: nothing is published on this topic */
	default:
		return;		/* an event this build does not name: nothing to publish */
	}
	out->text(out->ctx, "event", word);
	f->sequence++;
	fields(f, out);
}

/* the reply of a method that acted */
static int accepted(const struct reply_sink *out)
{
	out->flag(out->ctx, "ok", 1);
	out->null(out->ctx, "refused");
	return 0;
}

/* one refusal line a second per word: the same word says the same thing, and the call id is the only fact that moves */
static void refusal_log(struct call_arbiter *f, enum sip_method m, const char *word, FILE *log)
{
	u64 now;

	if (!log || !f->core || !f->core->now_ms)
		return;
	now = f->core->now_ms(f->core->ctx);
	if (!strcmp(f->last_refusal, word) && now - f->last_refusal_ms < 1000)
		return;
	snprintf(f->last_refusal, sizeof(f->last_refusal), "%s", word);
	f->last_refusal_ms = now;
	fprintf(log, "%s: %s, call %u\n", method_name(m), word, fe_call_view(f->fe)->id);
}

/* the refusal reply; err is what the call returns to its caller */
static int refuse(struct call_arbiter *f, enum sip_method m, const char *word, int err, const struct reply_sink *out, FILE *log)
{
	out->flag(out->ctx, "ok", 0);
	out->text(out->ctx, "refused", word);
	refusal_log(f, m, word, log);
	return err;
}

/* a core errno as its refusal word: the modem gone serves no call, and anything else is a refusal of its own */
static const char *refusal_word(int err)
{
	switch (err) {
	case -ENOENT:
	case -ENODEV:
		return fe_refusal_name(FE_REFUSE_NOCALL);
	case -EBUSY:
		return fe_refusal_name(FE_REFUSE_BUSY);
	}
	return "NOT_AVAILABLE";
}

static int refused_errno(struct call_arbiter *f, enum sip_method m, int err, const struct reply_sink *out, FILE *log)
{
	return refuse(f, m, refusal_word(err), err, out, log);
}

static void capabilities(const struct reply_sink *out)
{
	out->text(out->ctx, "status", "ok");
	out->text(out->ctx, "media", "qmsk");
	out->number(out->ctx, "sample_rate", 8000);
	out->number(out->ctx, "frame_ms", 20);
	out->number(out->ctx, "frame_samples", 160);
	out->flag(out->ctx, "wideband", 0);
	out->number(out->ctx, "calls", 1);
	out->open(out->ctx, "endpoints", 1);
	out->text(out->ctx, NULL, "lan_sip");
	out->text(out->ctx, NULL, "cellular");
	out->text(out->ctx, NULL, "browser");
	out->close(out->ctx);
}

void arbiter_init(struct call_arbiter *f, struct fe *fe, struct qmsk_bindings *bindings, const struct ctl_ops *core)
{
	memset(f, 0, sizeof(*f));
	f->fe = fe;
	f->bindings = bindings;
	f->core = core;
}

void arbiter_set_off(struct call_arbiter *f, int off)
{
	f->off = off != 0;
}

int arbiter_request(struct call_arbiter *f, enum sip_method m, const struct sip_args *a, const struct reply_sink *out, FILE *log)
{
	const struct sip_args none = { 0 };
	const struct fe_call *v = fe_call_view(f->fe);
	char id[QMSK_ID_MAX + 1];
	int err;

	if (!a)
		a = &none;
	switch (m) {
	case SIP_M_STATUS:
		arbiter_status(f, out);
		return 0;
	case SIP_M_CAPABILITIES:
		capabilities(out);
		return 0;
	case SIP_M_ANSWER:
		if (f->off)
			return refuse(f, m, fe_refusal_name(FE_REFUSE_MODE), -EPERM, out, log);
		if (v->id && v->state == FE_STATE_ACTIVE && v->owner == FE_ANSWER_MACHINE) {
			/* the answering machine holds the caller: this same verb gives that call to the front end that
			 * asked, the way a lift at the handset gives it to the person at the phone */
			err = f->core->takeover(f->core->ctx, a && a->via_sip ? 1 : 0);
			return err ? refused_errno(f, m, err, out, log) : accepted(out);
		}
		if (!v->id || v->state != FE_STATE_INCOMING_RINGING)
			return refuse(f, m, fe_refusal_name(FE_REFUSE_NOCALL), -ENOENT, out, log);
		if (v->owner == FE_ANSWER_HANDSET || v->owner == FE_ANSWER_MACHINE)
			return refuse(f, m, fe_refusal_name(FE_REFUSE_BUSY), -EBUSY, out, log);
		err = f->core->answer(f->core->ctx, a && a->via_sip ? 1 : 0);
		return err ? refused_errno(f, m, err, out, log) : accepted(out);
	case SIP_M_ORIGINATE:
		if (f->off)
			return refuse(f, m, fe_refusal_name(FE_REFUSE_MODE), -EPERM, out, log);
		/* the three endpoints this build serves: the LAN consumer, the same page over the LAN, and the page's own
		 * control surface; an external SIP leg is not this build's */
		if (!a->endpoint || (strcmp(a->endpoint, "lan_sip") && strcmp(a->endpoint, "cellular")
		    && strcmp(a->endpoint, "browser")))
			return refuse(f, m, fe_refusal_name(FE_REFUSE_MODE), -EPERM, out, log);
		err = f->core->dial(f->core->ctx, a->number ? a->number : "", a && a->via_sip ? 1 : 0);
		return err ? refused_errno(f, m, err, out, log) : accepted(out);
	case SIP_M_REJECT:
		if (f->off)
			return refuse(f, m, fe_refusal_name(FE_REFUSE_MODE), -EPERM, out, log);
		if (!v->id || v->state != FE_STATE_INCOMING_RINGING)
			return refuse(f, m, fe_refusal_name(FE_REFUSE_NOCALL), -ENOENT, out, log);
		err = f->core->hangup(f->core->ctx);	/* the core's hangup path rejects an incoming call */
		if (err)
			return refused_errno(f, m, err, out, log);
		f->pending_reason = FE_REASON_REJECTED;
		return accepted(out);
	case SIP_M_HANGUP:
		/* a hangup with nothing to end is accepted: the intent is satisfied, and the core says -ENOENT for it */
		if (f->off)
			return refuse(f, m, fe_refusal_name(FE_REFUSE_MODE), -EPERM, out, log);
		err = f->core->hangup(f->core->ctx);
		if (err && err != -ENOENT)
			return refused_errno(f, m, err, out, log);
		if (!err && v->id)
			f->pending_reason = FE_REASON_LOCAL;
		return accepted(out);
	case SIP_M_SEND_DTMF:
		if (f->off)
			return refuse(f, m, fe_refusal_name(FE_REFUSE_MODE), -EPERM, out, log);
		if (!v->id || !live_state(v->state))
			return refuse(f, m, fe_refusal_name(FE_REFUSE_NOCALL), -ENOENT, out, log);
		err = f->core->dtmf(f->core->ctx, a->digit);
		return err ? refused_errno(f, m, err, out, log) : accepted(out);
	case SIP_M_FLASH:
		/* the page's own door: the flash a person at the handset could make. the call view is no gate here: a
		 * held call is not in it, and the core reads the legs to decide (hold, swap, take the waiting call, take
		 * the held call back) */
		if (f->off)
			return refuse(f, m, fe_refusal_name(FE_REFUSE_MODE), -EPERM, out, log);
		err = f->core->flash ? f->core->flash(f->core->ctx) : -ENOENT;
		return err ? refused_errno(f, m, err, out, log) : accepted(out);
	case SIP_M_ISSUE_SOCKET_SESSION:
		if (f->off)
			return refuse(f, m, fe_refusal_name(FE_REFUSE_MODE), -EPERM, out, log);
		if (a->call_revision != v->revision)
			return refuse(f, m, fe_refusal_name(FE_REFUSE_REVISION), -ESTALE, out, log);
		if (!v->id)
			return refuse(f, m, fe_refusal_name(FE_REFUSE_NOCALL), -ENOENT, out, log);
		/* the page owns this call: its microphone and speaker hold the audio, and only the page releases it */
		if (v->owner == FE_ANSWER_WEB)
			return refuse(f, m, fe_refusal_name(FE_REFUSE_BUSY), -EBUSY, out, log);
		err = qmsk_binding_mint(f->bindings, a->call_revision, f->core->now_ms(f->core->ctx), id);
		if (err)
			return refused_errno(f, m, err, out, log);
		out->flag(out->ctx, "ok", 1);
		out->null(out->ctx, "refused");
		out->text(out->ctx, "session_id", id);
		return 0;
	case SIP_M_NONE:
		break;
	}
	return refuse(f, m, "NOT_AVAILABLE", -EOPNOTSUPP, out, log);
}

void arbiter_media(struct call_arbiter *f, int capture, int playback)
{
	u64 rev = fe_call_view(f->fe)->revision;

	if (f->media_revision != rev) {
		f->media_revision = rev;
		f->capture_frames = 0;
		f->playback_frames = 0;
	}
	if (capture)
		f->capture_frames++;
	if (playback)
		f->playback_frames++;
}

void arbiter_dropped(struct call_arbiter *f)
{
	f->drop_count++;
}

enum fe_reason arbiter_take_reason(struct call_arbiter *f)
{
	enum fe_reason r = f->pending_reason;

	f->pending_reason = FE_REASON_NONE;
	return r;
}

const struct fe_call *arbiter_view(const struct call_arbiter *f)
{
	return fe_call_view(f->fe);
}
