/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
/* The front-end module: the call view the stage machine publishes into, the events that change is, the cut from the
 * tap's slots into 20 ms frames, and the ring-owner record of the audio fan-out (front_end.h). */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "front_end.h"

struct fe {
	struct fe_call call;
	struct fe_owners owners;
	struct fe_sink sink;		/* copied at attach: the caller's struct may go at once */
	int attached;			/* a front-end is attached; its event or capture callback may still be NULL */
	int off;			/* the line's front end may not run: every line state reads as disabled */
	enum phone_line line;		/* the last line the stage machine observed */
	struct fe_capture cap;		/* rate 0 = the cut is not running */
};

/* NULL, or two NULL callbacks, is no front-end: something has to receive what the rules publish */
static int fe_sink_usable(const struct fe_sink *s)
{
	return s && (s->event || s->capture);
}

static void fe_detach(struct fe *f)
{
	memset(&f->sink, 0, sizeof(f->sink));
	f->attached = 0;
	memset(&f->cap, 0, sizeof(f->cap));	/* the part held is dropped, and the cut does not start again on its own */
}

int fe_open(struct fe **out, const struct fe_sink *sink, int off)
{
	struct fe *f;

	if (!out)
		return -EINVAL;
	*out = NULL;
	f = calloc(1, sizeof(*f));
	if (!f)
		return -ENOMEM;
	fe_call_init(&f->call);
	f->call.state = fe_state_of(off, PHONE_LINE_IDLE);
	f->off = off;
	f->line = PHONE_LINE_IDLE;
	if (fe_sink_usable(sink)) {
		f->sink = *sink;
		f->attached = 1;
	}
	*out = f;
	return 0;
}

void fe_close(struct fe *f)
{
	free(f);		/* free(NULL) is the no-op the close takes */
}

void fe_set_off(struct fe *f, int off)
{
	f->off = off;
	fe_call_state(&f->call, fe_state_of(off, f->line));	/* the revision moves, nothing is published */
}

void fe_set_sink(struct fe *f, const struct fe_sink *sink)
{
	if (!fe_sink_usable(sink)) {
		fe_detach(f);
		return;
	}
	f->sink = *sink;
	f->attached = 1;		/* attaching starts no cut: fe_capture_reset does */
}

const struct fe_call *fe_call_view(const struct fe *f)
{
	return &f->call;
}

struct fe_owners *fe_owners(struct fe *f)
{
	return &f->owners;
}

/* the states a call is in: a release out of one of them ends the call */
static int fe_state_is_call(enum fe_state s)
{
	return s == FE_STATE_OUTGOING_SETUP || s == FE_STATE_INCOMING_RINGING || s == FE_STATE_EARLY_MEDIA ||
		s == FE_STATE_ACTIVE || s == FE_STATE_TERMINATING;
}

int fe_observe(struct fe *f, enum phone_line line, enum call_state call, enum phone_dir dir, unsigned int call_id,
	enum fe_answer_owner owner, int withheld, unsigned int duration_s, enum fe_reason reason)
{
	enum fe_state prev = f->call.state, next;
	enum fe_event_kind kind;
	struct fe_event ev;
	unsigned int ended = f->call.id;

	f->line = line;
	f->call.id = call_id;
	f->call.dir = dir;
	/* the owner is the call's: with no call, nobody answered it (a status that keeps the last answerer is the core's
	 * own, and the view must not carry it into the next idle) */
	f->call.owner = call_id ? owner : FE_ANSWER_NONE;
	f->call.withheld = withheld;
	f->call.duration_s = duration_s;
	next = fe_state_of_call(f->off, call, line);
	fe_call_state(&f->call, next);
	if (next == FE_STATE_INCOMING_RINGING && prev != FE_STATE_INCOMING_RINGING)
		kind = FE_EV_RING;
	else if (next == FE_STATE_ACTIVE && prev != FE_STATE_ACTIVE)
		kind = FE_EV_ACTIVE;
	else if (fe_state_is_call(prev) && (next == FE_STATE_IDLE || next == FE_STATE_DISABLED))
		kind = FE_EV_RELEASE;
	else if (next == FE_STATE_FAULT && prev != FE_STATE_FAULT)
		kind = FE_EV_FAULT;
	else
		return 0;	/* the revision may have moved; there is nothing to publish */
	if (!f->attached || !f->sink.event)
		return 0;
	memset(&ev, 0, sizeof(ev));
	ev.event = kind;
	ev.id = kind == FE_EV_RELEASE ? ended : call_id;	/* the release names the call that ended, not the one this observation brought */
	ev.dir = dir;
	ev.phase = line;
	ev.owner = owner;
	ev.withheld = withheld;
	ev.revision = f->call.revision;
	ev.duration_s = duration_s;
	ev.reason = reason;
	f->sink.event(f->sink.ctx, &ev);
	return 1;
}

void fe_dtmf(struct fe *f, char digit)
{
	struct fe_event ev;

	if (!f->attached || !f->sink.event)
		return;
	memset(&ev, 0, sizeof(ev));
	ev.event = FE_EV_DTMF;
	ev.id = f->call.id;		/* 0 outside a call */
	ev.dir = f->call.dir;
	ev.phase = f->line;
	ev.owner = f->call.owner;
	ev.withheld = f->call.withheld;
	ev.revision = f->call.revision;
	ev.duration_s = f->call.duration_s;
	ev.digit = digit;
	f->sink.event(f->sink.ctx, &ev);
}

int fe_capture_reset(struct fe *f, u32 rate, int wideband)
{
	struct fe_capture probe;
	struct fe_frame tail;

	/* the arguments are judged on a copy: a refused generation leaves the cut exactly as it was */
	if (fe_capture_init(&probe, rate, wideband))
		return -EINVAL;
	if (f->attached && f->sink.capture && fe_capture_flush(&f->cap, &tail))
		f->sink.capture(f->sink.ctx, &tail);	/* the part held goes out padded, at the rate it was cut at */
	f->call.rate = rate;		/* the rate the tap runs at is the call's own */
	return fe_capture_init(&f->cap, rate, wideband);
}

void fe_tap(struct fe *f, const struct fe_slot *slots, u32 n)
{
	u32 k;

	if (!f->attached || !f->cap.rate)
		return;		/* no front-end, or no cut: the slots go whole */
	for (k = 0; k < n; k++)
		if (fe_capture_push(&f->cap, &slots[k]) && f->sink.capture)
			f->sink.capture(f->sink.ctx, &f->cap.frame);
}
