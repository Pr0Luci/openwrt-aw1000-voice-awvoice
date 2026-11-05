/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
/* The front-end module against a sink of static counters: the call view and its revisions, the four events and the
 * digit, the cut at both rates and its tail, and the ring owners: make -f Makefile.host test */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "front_end.h"

static int ev_count;
static struct fe_event ev_last;
static int fr_count;
static struct fe_frame fr_last;

static void on_event(void *ctx, const struct fe_event *ev)
{
	assert(!ctx);
	ev_count++;
	ev_last = *ev;
}

static void on_capture(void *ctx, const struct fe_frame *fr)
{
	assert(!ctx);
	fr_count++;
	fr_last = *fr;
}

static void attach(struct fe *f)
{
	struct fe_sink s;

	memset(&s, 0, sizeof(s));
	s.event = on_event;
	s.capture = on_capture;
	fe_set_sink(f, &s);
}

static void reset_counters(void)
{
	ev_count = 0;
	fr_count = 0;
	memset(&ev_last, 0, sizeof(ev_last));
	memset(&fr_last, 0, sizeof(fr_last));
}

/* slot k carries k + 1 on the modem's sample and 2001 + k on its wideband second; the line chip's pair is never read */
static void fill_slots(struct fe_slot *slots, u32 n)
{
	u32 k;

	for (k = 0; k < n; k++) {
		slots[k].modem = (u16)(k + 1);
		slots[k].line = 0x7fff;
		slots[k].modem2 = (u16)(2001 + k);
		slots[k].line2 = 0x7fff;
	}
}

static void test_open_close(void)
{
	struct fe *f = NULL, *off = NULL;
	const struct fe_call *v;
	struct fe_owners *o;
	struct fe_sink zero;

	assert(fe_open(NULL, NULL, 0) == -EINVAL);
	assert(fe_open(&f, NULL, 0) == 0 && f);
	v = fe_call_view(f);
	assert(!v->id && v->state == FE_STATE_IDLE && v->dir == PHONE_DIR_NONE && v->owner == FE_ANSWER_NONE);
	assert(!v->withheld && !v->rate && !v->duration_s && v->revision == 1);
	o = fe_owners(f);
	assert(o->play == FE_PLAY_NONE && !o->sink && !strcmp(fe_ring_owner_name(o), "idle"));
	assert(!strcmp(fe_state_name(v->state), "idle"));
	/* sip.mode = off at the start: disabled from the first view, and the revision has not moved */
	assert(fe_open(&off, NULL, 1) == 0 && fe_call_view(off)->state == FE_STATE_DISABLED && fe_call_view(off)->revision == 1);
	/* a zeroed sink and a NULL one are both no sink */
	memset(&zero, 0, sizeof(zero));
	fe_set_sink(f, &zero);
	fe_set_sink(f, NULL);
	fe_close(off);
	fe_close(f);
	fe_close(NULL);
}

/* every line value through fe_observe, with and without a call, and the revision only where the state moves */
static void test_observe_states(void)
{
	struct fe *f = NULL;
	const struct fe_call *v;
	u64 rev;

	assert(fe_open(&f, NULL, 0) == 0);
	v = fe_call_view(f);
	assert(!fe_observe(f, PHONE_LINE_IDLE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE));
	assert(v->state == FE_STATE_IDLE && v->revision == 1);
	/* the dial tone and the digit collection are before a call: still idle, and nothing moves */
	rev = v->revision;
	assert(!fe_observe(f, PHONE_LINE_DIAL_TONE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE));
	assert(!fe_observe(f, PHONE_LINE_DIALLING, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE));
	assert(v->state == FE_STATE_IDLE && v->revision == rev);
	/* an outgoing call: the setup, then the line's call */
	assert(!fe_observe(f, PHONE_LINE_SETUP, CALL_DIALING, PHONE_DIR_OUT, 4, FE_ANSWER_WEB, 0, 2, FE_REASON_NONE));
	assert(v->state == FE_STATE_OUTGOING_SETUP && v->id == 4 && v->dir == PHONE_DIR_OUT);
	assert(v->owner == FE_ANSWER_WEB && v->duration_s == 2 && v->revision == rev + 1);
	/* the line reads idle for a moment while an answer is on its way: the call is still the outgoing setup, and
	 * an idle line publishes no release (the front end follows the call, not the line) */
	{
		enum fe_state was = v->state;
		assert(!fe_observe(f, PHONE_LINE_IDLE, CALL_DIALING, PHONE_DIR_OUT, 4, FE_ANSWER_WEB, 0, 2, FE_REASON_NONE));
		assert(v->state == was);
	}
	assert(!fe_observe(f, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_OUT, 4, FE_ANSWER_WEB, 0, 7, FE_REASON_NONE));
	assert(v->state == FE_STATE_ACTIVE && v->revision == rev + 2);
	/* the same observation again: no state change, no revision */
	assert(!fe_observe(f, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_OUT, 4, FE_ANSWER_WEB, 0, 8, FE_REASON_NONE));
	assert(v->revision == rev + 2 && v->duration_s == 8);
	/* the network releases it while the handset is up */
	assert(!fe_observe(f, PHONE_LINE_RELEASE, CALL_GONE, PHONE_DIR_OUT, 4, FE_ANSWER_WEB, 0, 9, FE_REASON_LOCAL));
	assert(v->state == FE_STATE_TERMINATING && v->revision == rev + 3 && v->duration_s == 9);
	/* the handset goes down again; the answerer of the call before it is not the idle line's */
	assert(!fe_observe(f, PHONE_LINE_IDLE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_WEB, 0, 0, FE_REASON_LOCAL));
	assert(v->state == FE_STATE_IDLE && v->revision == rev + 4 && v->owner == FE_ANSWER_NONE);
	/* no service: fault once, and no second change while it stays there */
	assert(!fe_observe(f, PHONE_LINE_NO_SERVICE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE));
	assert(v->state == FE_STATE_FAULT && v->revision == rev + 5);
	assert(!fe_observe(f, PHONE_LINE_NO_SERVICE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE));
	assert(v->revision == rev + 5);
	/* the line recovers */
	assert(!fe_observe(f, PHONE_LINE_IDLE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE));
	assert(v->state == FE_STATE_IDLE && v->revision == rev + 6);
	/* the line's front end off reads as disabled whatever the line says, and back to it */
	fe_set_off(f, 1);
	assert(v->state == FE_STATE_DISABLED && v->revision == rev + 7);
	assert(!fe_observe(f, PHONE_LINE_RINGING, CALL_INCOMING, PHONE_DIR_IN, 5, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE));
	assert(v->state == FE_STATE_DISABLED && v->revision == rev + 7 && v->id == 5);
	fe_set_off(f, 0);
	assert(v->state == FE_STATE_INCOMING_RINGING && v->revision == rev + 8);
	/* the call's own observation: the state stays, the id and the flag arrive */
	assert(!fe_observe(f, PHONE_LINE_RINGING, CALL_INCOMING, PHONE_DIR_IN, 6, FE_ANSWER_NONE, 1, 0, FE_REASON_NONE));
	assert(v->id == 6 && v->dir == PHONE_DIR_IN && v->withheld && v->revision == rev + 8);
	/* answered */
	assert(!fe_observe(f, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_IN, 6, FE_ANSWER_HANDSET, 0, 0, FE_REASON_NONE));
	assert(v->state == FE_STATE_ACTIVE && v->owner == FE_ANSWER_HANDSET && !v->withheld && v->revision == rev + 9);
	fe_close(f);
}

/* the four events and their payload, through a sink */
static void test_events(void)
{
	struct fe *f = NULL;
	const struct fe_call *v;

	assert(fe_open(&f, NULL, 0) == 0);
	attach(f);
	v = fe_call_view(f);
	/* an incoming call rings: the id, the direction and the withheld flag */
	assert(fe_observe(f, PHONE_LINE_RINGING, CALL_INCOMING, PHONE_DIR_IN, 9, FE_ANSWER_NONE, 1, 0, FE_REASON_NONE) == 1);
	assert(ev_count == 1 && ev_last.event == FE_EV_RING && ev_last.id == 9 && ev_last.dir == PHONE_DIR_IN);
	assert(ev_last.phase == PHONE_LINE_RINGING && ev_last.owner == FE_ANSWER_NONE && ev_last.withheld);
	assert(ev_last.revision == v->revision && !ev_last.duration_s && ev_last.reason == FE_REASON_NONE && !ev_last.digit);
	assert(!strcmp(fe_event_name(ev_last.event), "ring"));
	/* the same ring again: nothing went out, nothing moved */
	assert(!fe_observe(f, PHONE_LINE_RINGING, CALL_INCOMING, PHONE_DIR_IN, 9, FE_ANSWER_NONE, 1, 1, FE_REASON_NONE));
	assert(ev_count == 1 && v->revision == ev_last.revision);
	/* the machine answered: the active event, with the owner and the duration */
	assert(fe_observe(f, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_IN, 9, FE_ANSWER_MACHINE, 0, 4, FE_REASON_NONE) == 1);
	assert(ev_count == 2 && ev_last.event == FE_EV_ACTIVE && ev_last.id == 9 && ev_last.owner == FE_ANSWER_MACHINE);
	assert(ev_last.phase == PHONE_LINE_CALL && ev_last.duration_s == 4 && ev_last.revision == v->revision);
	assert(!strcmp(fe_answer_owner_name(ev_last.owner), "machine"));
	/* the call ends: the release names the id that ended even though the observation says 0 */
	assert(fe_observe(f, PHONE_LINE_IDLE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 11, FE_REASON_HANDSET) == 1);
	assert(ev_count == 3 && ev_last.event == FE_EV_RELEASE && ev_last.id == 9 && !v->id);
	assert(ev_last.phase == PHONE_LINE_IDLE && ev_last.duration_s == 11 && ev_last.reason == FE_REASON_HANDSET);
	assert(!strcmp(fe_reason_name(ev_last.reason), "handset") && ev_last.revision == v->revision);
	/* an outgoing call: the setup publishes nothing, the answer does, and the end after the release state is still a release */
	assert(!fe_observe(f, PHONE_LINE_SETUP, CALL_DIALING, PHONE_DIR_OUT, 3, FE_ANSWER_WEB, 0, 1, FE_REASON_NONE));
	assert(ev_count == 3);
	assert(fe_observe(f, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_OUT, 3, FE_ANSWER_WEB, 0, 2, FE_REASON_NONE) == 1);
	assert(ev_count == 4 && ev_last.event == FE_EV_ACTIVE && ev_last.dir == PHONE_DIR_OUT && ev_last.owner == FE_ANSWER_WEB);
	assert(!fe_observe(f, PHONE_LINE_RELEASE, CALL_GONE, PHONE_DIR_OUT, 3, FE_ANSWER_WEB, 0, 3, FE_REASON_NONE));
	assert(ev_count == 4);
	assert(fe_observe(f, PHONE_LINE_IDLE, CALL_NONE, PHONE_DIR_NONE, 3, FE_ANSWER_NONE, 0, 4, FE_REASON_LOCAL) == 1);
	assert(ev_count == 5 && ev_last.event == FE_EV_RELEASE && ev_last.id == 3 && ev_last.reason == FE_REASON_LOCAL);
	/* the line falls to no service: one fault event, and its payload */
	assert(fe_observe(f, PHONE_LINE_NO_SERVICE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE) == 1);
	assert(ev_count == 6 && ev_last.event == FE_EV_FAULT && ev_last.phase == PHONE_LINE_NO_SERVICE && !ev_last.id);
	assert(!strcmp(fe_event_name(ev_last.event), "fault") && !strcmp(fe_state_name(v->state), "fault"));
	assert(!fe_observe(f, PHONE_LINE_NO_SERVICE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE));
	assert(ev_count == 6);
	/* the line comes back: a state change, no event */
	assert(!fe_observe(f, PHONE_LINE_IDLE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE));
	assert(ev_count == 6 && v->state == FE_STATE_IDLE);
	/* a fault in the middle of a call is a fault, not a release */
	assert(fe_observe(f, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_OUT, 5, FE_ANSWER_HANDSET, 0, 1, FE_REASON_NONE) == 1);
	assert(fe_observe(f, PHONE_LINE_NO_SERVICE, CALL_NONE, PHONE_DIR_NONE, 5, FE_ANSWER_NONE, 0, 2, FE_REASON_NONE) == 1);
	assert(ev_count == 8 && ev_last.event == FE_EV_FAULT && ev_last.id == 5);
	/* the line's front end off ends the call without an event; the fault state is not a call either */
	fe_set_off(f, 1);
	assert(ev_count == 8 && v->state == FE_STATE_DISABLED);
	fe_close(f);
}

static void test_dtmf(void)
{
	struct fe *f = NULL;
	const struct fe_call *v;

	assert(fe_open(&f, NULL, 0) == 0);
	v = fe_call_view(f);
	/* no sink: the digit goes nowhere and nothing moves */
	reset_counters();
	fe_dtmf(f, '1');
	assert(!ev_count && v->revision == 1);
	attach(f);
	/* outside a call the id is 0 */
	fe_dtmf(f, '#');
	assert(ev_count == 1 && ev_last.event == FE_EV_DTMF && ev_last.digit == '#' && !ev_last.id);
	assert(ev_last.revision == v->revision && ev_last.phase == PHONE_LINE_IDLE);
	assert(!strcmp(fe_event_name(ev_last.event), "dtmf"));
	/* inside one it is the call's id, and the revision does not move */
	assert(fe_observe(f, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_OUT, 12, FE_ANSWER_HANDSET, 0, 6, FE_REASON_NONE) == 1);
	reset_counters();
	fe_dtmf(f, '*');
	assert(ev_count == 1 && ev_last.id == 12 && ev_last.digit == '*' && ev_last.revision == v->revision);
	assert(ev_last.dir == PHONE_DIR_OUT && ev_last.duration_s == 6 && ev_last.phase == PHONE_LINE_CALL);
	fe_close(f);
}

static void test_capture_8k(void)
{
	struct fe *f = NULL;
	struct fe_slot slots[FE_SLOTS_PER_FRAME];

	assert(fe_open(&f, NULL, 0) == 0);
	attach(f);
	reset_counters();
	fill_slots(slots, FE_SLOTS_PER_FRAME);
	/* no cut before a reset: the slots go whole */
	fe_tap(f, slots, FE_SLOTS_PER_FRAME);
	assert(!fr_count);
	assert(fe_capture_reset(f, 8000, 0) == 0 && fe_call_view(f)->rate == 8000);
	fe_tap(f, slots, FE_SLOTS_PER_FRAME - 1);
	assert(!fr_count);
	fe_tap(f, slots + FE_SLOTS_PER_FRAME - 1, 1);
	assert(fr_count == 1);
	assert(!fr_last.sequence && fr_last.rate == 8000 && fr_last.count == FE_SAMPLES_8K);
	assert(fr_last.sample[0] == 1 && fr_last.sample[159] == 160);
	assert(fr_last.sample[0] != 0x7fff);	/* the line chip's sample is not the far end's */
	/* the next frame counts on */
	fe_tap(f, slots, FE_SLOTS_PER_FRAME);
	assert(fr_count == 2 && fr_last.sequence == 1 && fr_last.sample[159] == 160);
	/* an empty read changes nothing */
	fe_tap(f, slots, 0);
	assert(fr_count == 2);
	fe_close(f);
}

static void test_capture_16k(void)
{
	struct fe *f = NULL;
	struct fe_slot slots[FE_SLOTS_PER_FRAME];

	assert(fe_open(&f, NULL, 0) == 0);
	attach(f);
	reset_counters();
	assert(fe_capture_reset(f, 16000, 1) == 0 && fe_call_view(f)->rate == 16000);
	fill_slots(slots, FE_SLOTS_PER_FRAME);
	fe_tap(f, slots, FE_SLOTS_PER_FRAME);
	assert(fr_count == 1 && fr_last.rate == 16000 && fr_last.count == FE_SAMPLES_16K && !fr_last.sequence);
	/* two time samples a slot, the modem's own first */
	assert(fr_last.sample[0] == 1 && fr_last.sample[1] == 2001);
	assert(fr_last.sample[2] == 2 && fr_last.sample[3] == 2002);
	assert(fr_last.sample[318] == 160 && fr_last.sample[319] == 2160);
	fe_close(f);
}

static void test_flush(void)
{
	struct fe *f = NULL;
	struct fe_slot slots[FE_SLOTS_PER_FRAME];

	assert(fe_open(&f, NULL, 0) == 0);
	attach(f);
	reset_counters();
	fill_slots(slots, FE_SLOTS_PER_FRAME);
	/* four slots held, then the rate switch: they go out as one padded frame at the old rate */
	assert(fe_capture_reset(f, 8000, 0) == 0);
	fe_tap(f, slots, 4);
	assert(!fr_count);
	assert(fe_capture_reset(f, 16000, 1) == 0);
	assert(fr_count == 1 && fr_last.rate == 8000 && fr_last.count == FE_SAMPLES_8K && !fr_last.sequence);
	assert(fr_last.sample[0] == 1 && fr_last.sample[3] == 4 && !fr_last.sample[4] && !fr_last.sample[159]);
	/* the new generation starts at its own sequence */
	fe_tap(f, slots, FE_SLOTS_PER_FRAME);
	assert(fr_count == 2 && fr_last.rate == 16000 && !fr_last.sequence && fr_last.count == FE_SAMPLES_16K);
	/* wideband: the padded tail zeroes both time samples of every slot */
	assert(fe_capture_reset(f, 16000, 1) == 0);
	fe_tap(f, slots, 2);
	reset_counters();
	assert(fe_capture_reset(f, 16000, 1) == 0);
	assert(fr_count == 1 && fr_last.rate == 16000 && !fr_last.sequence && fr_last.count == FE_SAMPLES_16K);
	assert(fr_last.sample[0] == 1 && fr_last.sample[1] == 2001 && fr_last.sample[2] == 2 && fr_last.sample[3] == 2002);
	assert(!fr_last.sample[4] && !fr_last.sample[5] && !fr_last.sample[319]);
	/* nothing held: nothing goes out */
	reset_counters();
	assert(fe_capture_reset(f, 8000, 0) == 0 && !fr_count);
	fe_close(f);
}

static void test_reset_refusals(void)
{
	struct fe *f = NULL;
	struct fe_slot slots[FE_SLOTS_PER_FRAME];

	assert(fe_open(&f, NULL, 0) == 0);
	attach(f);
	assert(fe_capture_reset(f, 11025, 0) == -EINVAL);
	assert(fe_capture_reset(f, 16000, 0) == -EINVAL);	/* the tap says narrowband: a bridge fault, not a short frame */
	assert(fe_capture_reset(f, 0, 0) == -EINVAL);
	reset_counters();
	/* a running cut with three slots held: a refused reset leaves it exactly as it was */
	fill_slots(slots, FE_SLOTS_PER_FRAME);
	assert(fe_capture_reset(f, 8000, 0) == 0);
	fe_tap(f, slots, 3);
	assert(fe_capture_reset(f, 16000, 0) == -EINVAL);
	assert(fe_capture_reset(f, 11025, 1) == -EINVAL);
	assert(!fr_count);		/* nothing was flushed */
	fe_tap(f, slots + 3, FE_SLOTS_PER_FRAME - 3);
	assert(fr_count == 1 && fr_last.rate == 8000 && !fr_last.sequence && fr_last.count == FE_SAMPLES_8K);
	assert(fr_last.sample[0] == 1 && fr_last.sample[2] == 3 && fr_last.sample[159] == 160);
	fe_close(f);
}

static void test_detach(void)
{
	struct fe *f = NULL;
	struct fe_slot slots[FE_SLOTS_PER_FRAME];
	struct fe_sink zero;

	assert(fe_open(&f, NULL, 0) == 0);
	attach(f);
	fill_slots(slots, FE_SLOTS_PER_FRAME);
	assert(fe_capture_reset(f, 8000, 0) == 0);
	fe_tap(f, slots, 100);
	reset_counters();
	/* detached: the held slots are dropped, the frames go, and nothing is published */
	fe_set_sink(f, NULL);
	assert(!fe_observe(f, PHONE_LINE_RINGING, CALL_INCOMING, PHONE_DIR_IN, 2, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE));
	fe_dtmf(f, '2');
	fe_tap(f, slots, FE_SLOTS_PER_FRAME);
	assert(!ev_count && !fr_count);
	/* attaching again starts no cut: the slots go whole until a reset */
	attach(f);
	fe_tap(f, slots, FE_SLOTS_PER_FRAME);
	assert(!fr_count);
	assert(fe_capture_reset(f, 8000, 0) == 0);
	fe_tap(f, slots, FE_SLOTS_PER_FRAME);
	assert(fr_count == 1 && !fr_last.sequence && fr_last.sample[0] == 1);
	/* a zeroed sink detaches like a NULL one */
	memset(&zero, 0, sizeof(zero));
	fe_set_sink(f, &zero);
	fe_tap(f, slots, FE_SLOTS_PER_FRAME);
	assert(fr_count == 1);
	fe_close(f);
}

static void test_play_expand(void)
{
	struct fe *f = NULL;
	struct fe_frame fr;
	struct fe_slot out[FE_SLOTS_PER_FRAME];
	u32 k;

	memset(&fr, 0, sizeof(fr));
	fr.rate = 8000;
	fr.count = FE_SAMPLES_8K;
	for (k = 0; k < FE_SAMPLES_8K; k++)
		fr.sample[k] = (int16_t)(k - 80);
	assert(fe_play_expand(&fr, out) == FE_SLOTS_PER_FRAME);
	for (k = 0; k < FE_SLOTS_PER_FRAME; k++) {
		assert(out[k].modem == (u16)fr.sample[k]);
		assert(!out[k].line && !out[k].modem2 && !out[k].line2);
	}
	/* 16 kHz: two time samples a slot, the second in modem2 */
	fr.rate = 16000;
	fr.count = FE_SAMPLES_16K;
	for (k = 0; k < FE_SAMPLES_16K; k++)
		fr.sample[k] = (int16_t)(k + 100);
	assert(fe_play_expand(&fr, out) == FE_SLOTS_PER_FRAME);
	assert(out[0].modem == 100 && out[0].modem2 == 101 && out[159].modem == 418 && out[159].modem2 == 419);
	/* round trip: a 16 kHz frame out to the slots, back through the cut, the samples read the same */
	assert(fe_open(&f, NULL, 0) == 0);
	attach(f);
	reset_counters();
	assert(fe_capture_reset(f, 16000, 1) == 0);
	fe_tap(f, out, FE_SLOTS_PER_FRAME);
	assert(fr_count == 1 && fr_last.rate == 16000 && fr_last.count == FE_SAMPLES_16K);
	assert(!memcmp(fr_last.sample, fr.sample, sizeof(fr.sample)));
	/* a frame that is not one this unit writes is refused, and not one slot is written */
	memset(out, 0xAA, sizeof(out));
	fr.rate = 11025;
	fr.count = FE_SAMPLES_8K;
	assert(!fe_play_expand(&fr, out) && out[0].modem == 0xAAAA);
	fr.rate = 8000;
	fr.count = FE_SAMPLES_16K;
	assert(!fe_play_expand(&fr, out) && out[0].modem == 0xAAAA);
	fr.rate = 16000;
	fr.count = FE_SAMPLES_8K;
	assert(!fe_play_expand(&fr, out) && out[0].modem == 0xAAAA);
	fr.rate = 0;
	fr.count = 0;
	assert(!fe_play_expand(&fr, out) && out[0].modem == 0xAAAA);
	fe_close(f);
}

static void test_ring_owners(void)
{
	struct fe *f = NULL;
	struct fe_owners *o;

	assert(fe_open(&f, NULL, 0) == 0);
	o = fe_owners(f);
	assert(!o->sink && !fe_tap_needed(o));
	/* the recorder alone, then with the machine's message: the same word */
	fe_sink_set(o, FE_SINK_RECORDER, 1);
	assert(fe_tap_needed(o) && !strcmp(fe_ring_owner_name(o), "recorder"));
	fe_sink_set(o, FE_SINK_MACHINE, 1);
	assert(!strcmp(fe_ring_owner_name(o), "recorder"));
	/* SIP audio present: the front-end's word */
	fe_sink_set(o, FE_SINK_FRONT_END, 1);
	assert(!strcmp(fe_ring_owner_name(o), "front-end"));
	/* the greeting wins while it plays, and a second owner is refused with it */
	assert(fe_play_claim(o, FE_PLAY_FRONT_END) == 0 && !strcmp(fe_ring_owner_name(o), "front-end"));
	assert(fe_play_claim(o, FE_PLAY_GREETING) == 0 && !strcmp(fe_ring_owner_name(o), "greeting"));
	assert(fe_play_claim(o, FE_PLAY_GREETING) == 0);
	assert(fe_play_claim(o, FE_PLAY_FRONT_END) == -EBUSY && !strcmp(fe_ring_owner_name(o), "greeting"));
	/* released: the front-end takes it, and asking again is the same owner */
	fe_play_release(o);
	assert(fe_play_claim(o, FE_PLAY_FRONT_END) == 0 && o->play == FE_PLAY_FRONT_END);
	assert(fe_play_claim(o, FE_PLAY_FRONT_END) == 0);
	/* the page's browser audio is a front-end-class owner of its own: never beside the SIP front-end */
	assert(fe_play_claim(o, FE_PLAY_WEB) == -EBUSY && o->play == FE_PLAY_FRONT_END);
	fe_play_release(o);
	assert(fe_play_claim(o, FE_PLAY_WEB) == 0 && o->play == FE_PLAY_WEB);
	assert(fe_play_claim(o, FE_PLAY_FRONT_END) == -EBUSY && o->play == FE_PLAY_WEB);
	assert(fe_play_claim(o, FE_PLAY_GREETING) == 0 && o->play == FE_PLAY_GREETING);
	fe_play_release(o);
	/* its tap sink has its own word, below the SIP front-end's */
	fe_sink_set(o, FE_SINK_FRONT_END, 0);
	fe_sink_set(o, FE_SINK_WEB, 1);
	assert(!strcmp(fe_ring_owner_name(o), "web"));
	fe_sink_set(o, FE_SINK_WEB, 0);
	/* no sink at all: idle */
	fe_sink_set(o, FE_SINK_RECORDER | FE_SINK_MACHINE | FE_SINK_FRONT_END, 0);
	fe_play_release(o);
	assert(!fe_tap_needed(o) && !strcmp(fe_ring_owner_name(o), "idle"));
	/* the two failure ends read as different words */
	fe_sink_set(o, FE_SINK_RECORDER, 1);
	assert(!strcmp(fe_ring_owner_name(o), "recorder"));
	fe_sink_set(o, FE_SINK_RECORDER, 0);
	fe_sink_set(o, FE_SINK_FRONT_END, 1);
	assert(!strcmp(fe_ring_owner_name(o), "front-end"));
	fe_close(f);
}

int main(void)
{
	test_open_close();
	test_observe_states();
	test_events();
	test_dtmf();
	test_capture_8k();
	test_capture_16k();
	test_flush();
	test_reset_refusals();
	test_detach();
	test_play_expand();
	test_ring_owners();
	printf("front_end: all tests passed\n");
	return 0;
}
