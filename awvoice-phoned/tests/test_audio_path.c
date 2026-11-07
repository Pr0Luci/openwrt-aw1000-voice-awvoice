/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
/*
 * The audio path's life (src/core/phone.h, phone_audio_apply): it is brought up while something exchanges samples
 * and let down once nothing has needed it for AUDIO_GRACE_MS. The bridge crosses the modem's samples and the line
 * chip's, so the work it does while the line is quiet is work for silence - and the thread that does it shows in the
 * router's load average as a task that is always busy.
 *
 * The rig records every start and every stop in order, so these tests say what the module was told, not what the
 * core believes it asked. The virtual clock is the fake chip's.
 *
 * make -f Makefile.host test
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "chip.h"
#include "fakes.h"
#include "phone.h"
#include "slic_map.h"
#include "stages.h"

#define SEQ_MAX		64
#define EVENTS_MAX	64

static const int32_t steady[] = { 59000, 59000, 59000, 59000, 59000 };
static const int32_t offhook[] = { 20000, 20000, 20000, 20000, 20000 };

struct rig {
	struct model m;
	struct chip c;
	struct board b;
	uint8_t file[4096];
	FILE *log;
	char *lbuf;
	size_t lsize;
	struct fake_modem fm;
	int listed;					/* what the modem's call list shows (enum call_state) */
	struct call_ops ops;
	struct phone_hooks hooks;
	struct phone_cfg cfg;
	struct phone *p;
	unsigned long long now;
	char seq[SEQ_MAX];				/* 'u' for every bridge start, 'd' for every stop, in order */
	unsigned int seq_count;
	int start_err;					/* what the next bridge start answers (0 = it took) */
	int stop_err;
	int refuse_atd;			/* the modem answers ERROR to a dial */					/* ... and what a stop answers */
	struct phone_event ev[EVENTS_MAX];
	unsigned int ev_count;
};

static struct rig *the_rig;

static int list_poll(void *user, int *st)
{
	(void)user;
	*st = the_rig->listed;
	return 0;
}

static int list_act(void *user, const char *what)
{
	(void)user;
	return the_rig->refuse_atd && !strncmp(what, "ATD", 3) ? -EIO : 0;
}

static int rig_start(void *ctx, unsigned int modem_rx, unsigned int modem_tx, unsigned int line_rx,
	unsigned int line_tx, unsigned int flags)
{
	struct rig *r = ctx;

	(void)modem_rx; (void)modem_tx; (void)line_rx; (void)line_tx; (void)flags;
	if (r->seq_count < SEQ_MAX)
		r->seq[r->seq_count] = 'u';
	r->seq_count++;
	return r->start_err;
}

static int rig_stop(void *ctx)
{
	struct rig *r = ctx;

	if (r->seq_count < SEQ_MAX)
		r->seq[r->seq_count] = 'd';
	r->seq_count++;
	return r->stop_err;
}

static void rig_event(void *ctx, const struct phone_event *ev)
{
	struct rig *r = ctx;

	if (r->ev_count < EVENTS_MAX)
		r->ev[r->ev_count] = *ev;
	r->ev_count++;
}

static void rig_init(struct rig *r, int with_bridge)
{
	size_t n;

	memset(r, 0, sizeof(*r));
	the_rig = r;
	r->c = (struct chip){ &model_ops, &r->m };
	n = make_linefeed_board(r->file, &(struct test_board){ .with_converter = 1, .with_imped = 1, .imped_word0 = 0x1000,
		.with_tone = 1, .tone_word0 = 0x1234000, .with_iir = 1, .with_ring = 1 });
	assert(n <= sizeof(r->file) && board_parse(r->file, n, &r->b) == 0);
	linefeed_model(&r->m, steady, 5, offhook, 5, NULL, 0);
	r->log = open_memstream(&r->lbuf, &r->lsize);
	r->listed = CALL_NONE;
	r->fm = (struct fake_modem){ .poll = list_poll, .act = list_act };
	r->ops = (struct call_ops){ .ctx = &r->fm, .begin = fm_begin, .pump = fm_pump, .setup = 1, .ring = 1, .service_log = 1 };
	r->hooks = (struct phone_hooks){ .ctx = r, .event = rig_event };
	if (with_bridge) {
		r->hooks.bridge_start = rig_start;
		r->hooks.bridge_stop = rig_stop;
	}
	r->cfg = (struct phone_cfg){ .channel = TEST_CH, .service_log = 1 };
}

static void rig_open(struct rig *r)
{
	the_rig = r;
	assert(phone_open(&r->p, &r->c, &r->b, &r->ops, &r->cfg, &r->hooks, r->log) == 0 && r->p);
	r->now = r->c.ops->now_ms(&r->m);
}

/* the memstream the phone logged into: closed and freed, or the gate's sanitizer calls it a leak */
static void rig_close(struct rig *r)
{
	fclose(r->log);
	free(r->lbuf);
}

static void step(struct rig *r)
{
	r->m.vtime_ms += HOOK_POLL_MS;
	r->now = r->c.ops->now_ms(&r->m);
	assert(phone_step(r->p, r->now) == 0);
}

static void run_ms(struct rig *r, unsigned int ms)
{
	unsigned int t;

	for (t = 0; t < ms; t += HOOK_POLL_MS)
		step(r);
}

static void handset(struct rig *r, int up)
{
	r->m.reg[TEST_CH][REG_LCRRTP] = up ? LCR_ON_HOOK : 0;
}

static unsigned int count_of(const struct rig *r, char what)
{
	unsigned int k, n = 0;

	for (k = 0; k < r->seq_count && k < SEQ_MAX; k++)
		n += r->seq[k] == what;
	return n;
}

static struct phone_audio_view view_of(struct rig *r)
{
	struct phone_audio_view v;

	phone_audio_view_of(r->p, &v);
	return v;
}

static const char *log_text(struct rig *r)
{
	fflush(r->log);
	return r->lbuf ? r->lbuf : "";
}

static unsigned int count_in(const char *text, const char *what)
{
	unsigned int n = 0;

	for (; (text = strstr(text, what)); text += strlen(what))
		n++;
	return n;
}

/* the line shown by the core (the call list is asked for once a second, so a test waits for the state rather than a
 * number of steps: the same reason the daemon's own loop never assumes the modem is instant) */
static enum phone_line line_of(struct rig *r)
{
	struct phone_status st;

	phone_status(r->p, &st);
	return st.line;
}

static void run_to_line(struct rig *r, enum phone_line want, unsigned int max_ms)
{
	unsigned int t;

	for (t = 0; t < max_ms && line_of(r) != want; t += HOOK_POLL_MS)
		step(r);
	assert(line_of(r) == want);
}

/* run until the core itself says nothing needs the samples crossing (the call's record closes when the network's
 * release has been seen, which is a step or two after the line reads idle) */
static void run_until_quiet(struct rig *r, unsigned int max_ms)
{
	unsigned int t;

	for (t = 0; t < max_ms && view_of(r).wanted; t += HOOK_POLL_MS)
		step(r);
	assert(!view_of(r).wanted);
}

/* an incoming call the modem lists, with the handset down: the line has a reason for the samples to cross */
static void ring_in(struct rig *r)
{
	r->listed = CALL_INCOMING;
	run_to_line(r, PHONE_LINE_RINGING, 3000);
}

/* the person at the handset answers: no front end and no machine, so the only reason the samples cross is the call
 * itself (an answer asked from outside leaves its own answering flag behind, and that is a different test) */
static void call_up(struct rig *r)
{
	handset(r, 1);
	r->listed = CALL_ACTIVE;
	run_to_line(r, PHONE_LINE_CALL, 3000);
	assert(line_of(r) == PHONE_LINE_CALL);
}

static void call_gone(struct rig *r)
{
	handset(r, 0);					/* the handset goes back down first: a release with the line still open shows the release tone */
	r->listed = CALL_GONE;
	run_ms(r, 400);
	r->listed = CALL_NONE;
	run_to_line(r, PHONE_LINE_IDLE, 3000);
}

/* the bring-up leaves the path up: a layout that cannot cross samples is still said at the same moment it always was */
static void test_bring_up_leaves_it_running(void)
{
	struct rig r;

	rig_init(&r, 1);
	rig_open(&r);
	assert(count_of(&r, 'u') == 1 && count_of(&r, 'd') == 0);
	assert(view_of(&r).up == 1 && view_of(&r).starts == 1);
	phone_close(r.p, 0);
	rig_close(&r);

}

/* ... and the watch lets it down once the line has been quiet for the grace, once only: the load the path costs while
 * nothing needs it was the whole complaint */
static void test_quiet_line_stands_it_by(void)
{
	struct rig r;

	rig_init(&r, 1);
	rig_open(&r);
	run_ms(&r, AUDIO_GRACE_MS - 500);
	assert(count_of(&r, 'd') == 0 && view_of(&r).up == 1);	/* the grace is not over: nothing has been said yet */
	run_ms(&r, 1000);
	assert(count_of(&r, 'd') == 1 && view_of(&r).up == 0);
	assert(count_in(log_text(&r), "the audio path stands by") == 1);
	run_ms(&r, 10000);
	assert(count_of(&r, 'd') == 1 && count_of(&r, 'u') == 1);	/* standing by is a state, not a loop */
	phone_close(r.p, 0);					/* nothing to stop: the close says no more about it */
	assert(count_of(&r, 'd') == 1);
	rig_close(&r);

}

/* a lifted handset with nothing behind it is dial tone and digits: the chip makes the first and the modem queue
 * carries the second, so the samples still have nothing to cross */
static void test_handset_alone_keeps_it_down(void)
{
	struct rig r;

	rig_init(&r, 1);
	rig_open(&r);
	run_ms(&r, AUDIO_GRACE_MS + 500);
	assert(view_of(&r).up == 0);
	handset(&r, 1);						/* the handset goes up: the dial tone starts */
	run_to_line(&r, PHONE_LINE_DIAL_TONE, 3000);
	run_ms(&r, AUDIO_GRACE_MS + 500);
	assert(count_of(&r, 'u') == 1 && view_of(&r).up == 0);
	assert(line_of(&r) == PHONE_LINE_DIAL_TONE);
	handset(&r, 0);
	phone_close(r.p, 0);
	rig_close(&r);

}

/* the moment the modem lists a call, the path is up again - and it is said which reason brought it up */
static void test_a_call_brings_it_up(void)
{
	struct rig r;

	rig_init(&r, 1);
	rig_open(&r);
	run_ms(&r, AUDIO_GRACE_MS + 500);
	assert(view_of(&r).up == 0);
	ring_in(&r);
	assert(count_of(&r, 'u') == 2 && view_of(&r).up == 1);
	assert(!strcmp(view_of(&r).reason, "call"));
	assert(count_in(log_text(&r), "the audio path is up for the call") == 1);
	call_up(&r);
	run_ms(&r, AUDIO_GRACE_MS + 2000);
	assert(view_of(&r).up == 1 && count_of(&r, 'd') == 1);	/* a live call is never a quiet line */
	call_gone(&r);
	/* the line shows idle before the call's own record has closed, so the wait for "nothing needs the samples" is
	 * the core's answer, not a number of steps: the grace is timed from the last moment something needed them */
	run_until_quiet(&r, 6000);
	assert(view_of(&r).up == 1);				/* the grace has not passed: a hook flash looks the same from here */
	run_ms(&r, AUDIO_GRACE_MS - 500);
	assert(view_of(&r).up == 1 && count_of(&r, 'd') == 1);
	run_ms(&r, 1000);
	assert(count_of(&r, 'd') == 2 && view_of(&r).up == 0);
	phone_close(r.p, 0);
	rig_close(&r);

}

/* what a sink says it needs is a reason too: a recording keeps the path up with no call behind it, and the claim is
 * named in the reason the health reply shows */
static void test_a_claim_holds_it_up(void)
{
	struct rig r;

	rig_init(&r, 1);
	rig_open(&r);
	run_ms(&r, AUDIO_GRACE_MS + 500);
	assert(view_of(&r).up == 0);
	phone_audio_claim(r.p, PHONE_AUDIO_RECORDER, 1);
	assert(phone_audio_apply(r.p, r.now) == 0);
	assert(count_of(&r, 'u') == 2 && view_of(&r).up == 1);
	assert(!strcmp(view_of(&r).reason, "recording") && (view_of(&r).claims & PHONE_AUDIO_RECORDER));
	run_ms(&r, 3 * AUDIO_GRACE_MS);
	assert(view_of(&r).up == 1 && count_of(&r, 'd') == 1);	/* the claim renews the need every step */
	phone_audio_claim(r.p, PHONE_AUDIO_RECORDER, 0);
	assert(phone_audio_apply(r.p, r.now) == 0);		/* the grace has not passed: the arm is not refused */
	run_ms(&r, AUDIO_GRACE_MS + 500);
	assert(count_of(&r, 'd') == 2 && view_of(&r).up == 0);
	phone_close(r.p, 0);
	rig_close(&r);

}

/* a sink that arms itself against the bridge needs the path under it now, not at the next tick - and when the module
 * will not have it, the refusal must reach that caller instead of arriving as an errno nobody explains */
static void test_apply_answers_now(void)
{
	struct rig r;

	rig_init(&r, 1);
	rig_open(&r);
	run_ms(&r, AUDIO_GRACE_MS + 500);
	assert(view_of(&r).up == 0);
	r.start_err = -EIO;
	phone_audio_claim(r.p, PHONE_AUDIO_PAGE, 1);
	assert(phone_audio_apply(r.p, r.now) == -EIO);
	assert(view_of(&r).up == 0 && view_of(&r).last_error == -EIO);
	r.start_err = 0;
	r.now += AUDIO_RETRY_MS;				/* the backoff is the core's, so the next try is a later step's */
	assert(phone_audio_apply(r.p, r.now) == 0);
	assert(view_of(&r).up == 1 && view_of(&r).last_error == 0);
	phone_close(r.p, 0);
	rig_close(&r);

}

/* the try is cheap and the log line is not: a module that keeps refusing is said once, tried again on the backoff,
 * and the recovery is said when it comes */
static void test_failure_is_said_once_and_retried(void)
{
	struct rig r;
	unsigned int tries;

	rig_init(&r, 1);
	rig_open(&r);
	run_ms(&r, AUDIO_GRACE_MS + 500);
	r.start_err = -ENODEV;
	phone_audio_claim(r.p, PHONE_AUDIO_MESSAGE, 1);
	tries = count_of(&r, 'u');
	run_ms(&r, 2000);
	assert(count_of(&r, 'u') > tries);			/* it keeps trying: the path is not left down until somebody notices */
	assert(count_of(&r, 'u') - tries <= 2000 / AUDIO_RETRY_MS + 2);	/* ... on the backoff, not once a step: the try costs an ioctl */
	assert(count_in(log_text(&r), "the audio path did not come up") == 1);
	assert(count_in(log_text(&r), "the message") >= 1);
	r.start_err = 0;
	run_ms(&r, 500);
	assert(view_of(&r).up == 1);
	assert(count_in(log_text(&r), "came up for the message") == 1);
	phone_close(r.p, 0);
	rig_close(&r);

}

/* the line is brought up again for another layout: the path the earlier bring-up started does not outlive it, so the
 * new bring-up never finds one already crossing */
static void test_close_stops_what_open_started(void)
{
	struct rig r;

	rig_init(&r, 1);
	rig_open(&r);
	assert(view_of(&r).up == 1);
	assert(phone_close(r.p, 0) == 0);
	assert(count_of(&r, 'd') == 1);				/* one start, one stop: the symmetry the leak was missing */
	rig_close(&r);

}

/* a stop that the module refuses is not swallowed: the path stays up, the try comes again, and the health reply
 * carries the reason it is still running */
static void test_a_refused_stop_is_tried_again(void)
{
	struct rig r;

	rig_init(&r, 1);
	rig_open(&r);
	r.stop_err = -EBUSY;
	run_ms(&r, AUDIO_GRACE_MS + 500);
	assert(count_of(&r, 'd') >= 1 && view_of(&r).up == 1);
	assert(view_of(&r).last_error == -EBUSY);
	assert(count_in(log_text(&r), "did not stand by") == 1);
	r.stop_err = 0;
	run_ms(&r, AUDIO_RETRY_MS + 500);
	assert(view_of(&r).up == 0);
	phone_close(r.p, 0);
	rig_close(&r);

}

/* a front end with no audio device at all - the stage tool, and the core's other test rigs - is served exactly as
 * before: nothing is asked of hooks it never installed, and the view still says what the line wanted */
/* a dial the modem refuses: the line holds no call afterwards, so the path stands by once the grace has passed. An
 * identity left behind keeps the path wanted for the life of the service - the bridge then runs for silence and the
 * router's load average follows it */
static void test_refused_dial_stands_by(void)
{
	struct rig r;

	rig_init(&r, 1);
	r.refuse_atd = 1;
	r.m.dtmf_events[0].at = 300; r.m.dtmf_events[0].code = 0x04;	/* '4' */
	r.m.dtmf_events[1].at = 375; r.m.dtmf_events[1].code = 0x05;	/* '5' */
	r.m.dtmf_events[2].at = 450; r.m.dtmf_events[2].code = 0x0c;	/* the send key */
	r.m.dtmf_event_count = 3;
	rig_open(&r);
	r.m.reg[TEST_CH][REG_LCRRTP] = LCR_ON_HOOK;			/* the handset goes up: the digits below are its own */
	run_ms(&r, 2000);
	run_ms(&r, AUDIO_GRACE_MS + 2000);				/* past the grace: the refusal must have let the path down */
	assert(!view_of(&r).up && !view_of(&r).wanted && !strcmp(view_of(&r).reason, "idle"));
	assert(view_of(&r).starts == 1 && view_of(&r).stops == 1);	/* it went up for the attempt and down after it */
	phone_close(r.p, 0);
	rig_close(&r);
}

static void test_without_bridge_hooks(void)
{
	struct rig r;

	rig_init(&r, 0);
	rig_open(&r);
	run_ms(&r, 2 * AUDIO_GRACE_MS);
	assert(r.seq_count == 0);
	ring_in(&r);
	call_up(&r);
	run_ms(&r, AUDIO_GRACE_MS + 500);
	assert(r.seq_count == 0 && view_of(&r).up == 0 && view_of(&r).wanted);	/* wanted, but there is no device to start */
	assert(phone_audio_apply(r.p, r.now) == 0);				/* this front end has no path to refuse about */
	call_gone(&r);
	run_until_quiet(&r, 6000);
	assert(r.seq_count == 0 && !view_of(&r).wanted);
	phone_close(r.p, 0);
	assert(r.seq_count == 0);
	rig_close(&r);

}

int main(void)
{
	test_bring_up_leaves_it_running();
	test_quiet_line_stands_it_by();
	test_handset_alone_keeps_it_down();
	test_a_call_brings_it_up();
	test_a_claim_holds_it_up();
	test_apply_answers_now();
	test_failure_is_said_once_and_retried();
	test_close_stops_what_open_started();
	test_a_refused_stop_is_tried_again();
	test_refused_dial_stands_by();
	test_without_bridge_hooks();
	printf("audio_path: all tests passed\n");
	return 0;
}
