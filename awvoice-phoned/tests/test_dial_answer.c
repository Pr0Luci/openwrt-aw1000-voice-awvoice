/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
/*
 * The front end's requests on the core, with the handset down: phone_answer, phone_dial and phone_dtmf against the
 * fake chip and the fake modem, and the owner the call record and the status report.
 * make -f Makefile.host test
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "blocking.h"
#include "chip.h"
#include "fakes.h"
#include "phone.h"
#include "slic_map.h"
#include "stages.h"

#define ACTS_MAX	32
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
	int listed;				/* what the modem's call list shows (enum call_state) */
	char acts[ACTS_MAX][56];		/* the commands the modem was given, in order */
	unsigned int act_count;
	struct call_ops ops;
	struct phone_hooks hooks;
	struct phone_cfg cfg;
	struct phone *p;
	unsigned long long now;
	struct phone_event ev[EVENTS_MAX];
	unsigned int ev_count;
	struct phone_call_record rec[8];	/* the call log's records, in order */
	unsigned int rec_count;
};

/* the fake modem's callbacks reach the rig of the running test: one rig at a time */
static struct rig *the_rig;

static int list_poll(void *user, int *st)
{
	(void)user;
	*st = the_rig->listed;
	return 0;
}

static int list_act(void *user, const char *what)
{
	struct rig *r = the_rig;

	(void)user;
	if (r->act_count < ACTS_MAX)
		snprintf(r->acts[r->act_count], sizeof(r->acts[0]), "%s", what);
	r->act_count++;
	return 0;
}

static void rig_event(void *ctx, const struct phone_event *ev)
{
	struct rig *r = ctx;

	if (r->ev_count < EVENTS_MAX)
		r->ev[r->ev_count] = *ev;
	r->ev_count++;
}

static void rig_ended(void *ctx, const struct phone_call_record *rec)
{
	struct rig *r = ctx;

	if (r->rec_count < 8)
		r->rec[r->rec_count] = *rec;
	r->rec_count++;
}

static const char *the_caller = "5551";

static int caller_number(void *ctx, char *out, size_t cap)
{
	(void)ctx;
	snprintf(out, cap, "%s", the_caller);
	return 0;
}

static void rig_init(struct rig *r)
{
	size_t n;

	memset(r, 0, sizeof(*r));
	the_rig = r;
	r->c = (struct chip){ &model_ops, &r->m };
	n = make_linefeed_board(r->file, &(struct test_board){ .with_converter = 1, .with_imped = 1, .imped_word0 = 0x1000, .with_tone = 1,
		.tone_word0 = 0x1234000, .with_iir = 1, .with_ring = 1 });
	assert(n <= sizeof(r->file) && board_parse(r->file, n, &r->b) == 0);
	linefeed_model(&r->m, steady, 5, offhook, 5, NULL, 0);
	r->log = open_memstream(&r->lbuf, &r->lsize);
	r->listed = CALL_NONE;
	r->fm = (struct fake_modem){ .poll = list_poll, .act = list_act };
	r->ops = (struct call_ops){ .ctx = &r->fm, .begin = fm_begin, .pump = fm_pump, .setup = 1, .ring = 1, .service_log = 1 };
	r->hooks = (struct phone_hooks){ .ctx = r, .event = rig_event, .call_ended = rig_ended };
	r->ops.number = caller_number;
	r->cfg = (struct phone_cfg){ .channel = TEST_CH, .service_log = 1 };
}

static void rig_open(struct rig *r)
{
	the_rig = r;
	assert(phone_open(&r->p, &r->c, &r->b, &r->ops, &r->cfg, &r->hooks, r->log) == 0 && r->p);
	r->now = r->c.ops->now_ms(&r->m);
}

/* the handset: up means the loop is closed (the chip's register shows the on-hook bit for the caller being off-hook) */
static void handset(struct rig *r, int up)
{
	r->m.reg[TEST_CH][REG_LCRRTP] = up ? LCR_ON_HOOK : 0;
}

static void step(struct rig *r)
{
	r->m.vtime_ms += HOOK_POLL_MS;
	r->now = r->c.ops->now_ms(&r->m);
	assert(phone_step(r->p, r->now) == 0);
}

/* run the daemon's loop for ms milliseconds: one step per HOOK_POLL_MS */
static void run_ms(struct rig *r, unsigned int ms)
{
	unsigned int t;

	for (t = 0; t < ms; t += HOOK_POLL_MS)
		step(r);
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

static unsigned int acts_of(const struct rig *r, const char *cmd)
{
	unsigned int k, n = 0;

	for (k = 0; k < r->act_count && k < ACTS_MAX; k++)
		n += !strcmp(r->acts[k], cmd);
	return n;
}

static unsigned int writes_of(const struct rig *r, unsigned int from, unsigned int reg, unsigned int value)
{
	unsigned int k, n = 0;

	for (k = from; k < r->m.log_count; k++)
		n += r->m.log[k].kind == 'r' && r->m.log[k].addr == reg && r->m.log[k].value == value;
	return n;
}

static void rig_close(struct rig *r)
{
	assert(phone_close(r->p, 0) == 0);
	fclose(r->log);
	free(r->lbuf);
}

static struct phone_status status_of(struct rig *r)
{
	struct phone_status st;

	phone_status(r->p, &st);
	return st;
}

/* run until the line shows this state (the assertion says when it did not) */
static void run_to_line(struct rig *r, enum phone_line want, unsigned int max_ms)
{
	unsigned int t;

	for (t = 0; t < max_ms && status_of(r).line != want; t += HOOK_POLL_MS)
		step(r);
	assert(status_of(r).line == want);
}

/* run until the modem was given this command */
static void run_to_act(struct rig *r, const char *cmd, unsigned int max_ms)
{
	unsigned int t;

	for (t = 0; t < max_ms && acts_of(r, cmd) == 0; t += HOOK_POLL_MS)
		step(r);
	assert(acts_of(r, cmd) >= 1);
}

static struct blocking *rules(const char *in, const char *out, int anonymous)
{
	struct blocking *b = calloc(1, sizeof(*b));

	assert(b);
	blocking_clear(b, NULL);
	b->anonymous = anonymous;
	if (in)
		assert(blocking_add(b, BLOCK_IN, in, NULL) == 0);
	if (out)
		assert(blocking_add(b, BLOCK_OUT, out, NULL) == 0);
	return b;
}

/* a ringing call with the handset down, answered by the front end, and the answer reaching the call list */
static void fe_call_up(struct rig *r)
{
	rig_init(r);
	rig_open(r);
	r->listed = CALL_INCOMING;
	run_to_line(r, PHONE_LINE_RINGING, 3000);
	assert(phone_answer(r->p, PHONE_ANSWERED_WEB) == 0);
	run_to_act(r, "ATA", 3000);
	r->listed = CALL_ACTIVE;
	run_ms(r, 1500);
	assert(status_of(r).line == PHONE_LINE_CALL && status_of(r).answered_by == PHONE_ANSWERED_WEB);
}

/* the offered call is answered by the front end: the ring stops, ATA goes out, and the record keeps the owner */
static void test_answer_accepted(void)
{
	struct rig r;
	struct phone_status st;

	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 100);
	assert(phone_answer(r.p, PHONE_ANSWERED_WEB) == -ENOENT);		/* nothing is ringing to answer */

	r.listed = CALL_INCOMING;
	run_to_line(&r, PHONE_LINE_RINGING, 3000);
	st = status_of(&r);
	assert(st.dir == PHONE_DIR_IN && st.call_id && st.answered_by == PHONE_ANSWERED_NONE);

	assert(phone_answer(r.p, PHONE_ANSWERED_WEB) == 0);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_RINGING);		/* the ring is off, but the answer is still on its way: not idle */
	assert(st.answered_by == PHONE_ANSWERED_WEB);
	assert(count_in(log_text(&r), "ring stopped (the front end answers)") == 1);

	run_to_act(&r, "ATA", 3000);
	assert(acts_of(&r, "ATA") == 1);

	r.listed = CALL_ACTIVE;
	run_ms(&r, 1500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_CALL && st.answered_by == PHONE_ANSWERED_WEB);
	assert(st.calls_in == 1 && st.dir == PHONE_DIR_IN);
	assert(acts_of(&r, "AT+CHUP") == 0);		/* the connected call with the handset down was not hung up */
	assert(count_in(log_text(&r), "the front end has the call") == 1);

	r.listed = CALL_GONE;
	run_ms(&r, 1500);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(r.rec_count == 1);
	assert(r.rec[0].dir == PHONE_DIR_IN && r.rec[0].result == PHONE_RES_ANSWERED);
	assert(r.rec[0].answered_by == PHONE_ANSWERED_WEB && r.rec[0].answered_ms != 0);
	assert(count_in(log_text(&r), "the front end's call ended") == 1);
	rig_close(&r);
}

/* a refusal changes nothing: a running ring test keeps ringing, the machine's answer keeps the call */
static void test_answer_refused(void)
{
	struct rig r;
	struct phone_status st;

	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 100);
	assert(phone_answer(r.p, PHONE_ANSWERED_WEB) == -ENOENT);
	assert(phone_ring_test(r.p, 5) == 0);
	run_ms(&r, 300);
	assert(status_of(&r).line == PHONE_LINE_RINGING);
	assert(phone_answer(r.p, PHONE_ANSWERED_WEB) == -ENOENT);		/* a test ring is no call to answer */
	st = status_of(&r);
	assert(st.line == PHONE_LINE_RINGING && count_in(log_text(&r), "ring stopped") == 0);
	run_ms(&r, 6000);				/* the test ends by itself */
	assert(status_of(&r).line == PHONE_LINE_IDLE);
	rig_close(&r);

	/* the machine is answering: the call is already somebody's */
	rig_init(&r);
	r.cfg.machine = 1;
	rig_open(&r);
	r.listed = CALL_INCOMING;
	run_to_act(&r, "ATA", 3000);
	/* the answer is on its way: the ring is off, but the line is not idle (idle reads as a release to a front end) */
	assert(status_of(&r).machine == PHONE_MACHINE_ANSWERING && status_of(&r).line == PHONE_LINE_RINGING);
	assert(phone_answer(r.p, PHONE_ANSWERED_WEB) == -EBUSY);
	st = status_of(&r);
	assert(st.machine == PHONE_MACHINE_ANSWERING && st.line == PHONE_LINE_RINGING && st.answered_by == PHONE_ANSWERED_MACHINE);
	assert(acts_of(&r, "ATA") == 1);
	rig_close(&r);
}

/* the handset lifts while the answer is on its way: the handset wins and no second answer goes out */
static void test_answer_and_lift(void)
{
	struct rig r;
	struct phone_status st;

	rig_init(&r);
	rig_open(&r);
	r.listed = CALL_INCOMING;
	run_to_line(&r, PHONE_LINE_RINGING, 3000);
	assert(phone_answer(r.p, PHONE_ANSWERED_WEB) == 0);
	run_to_act(&r, "ATA", 3000);
	assert(acts_of(&r, "ATA") == 1);
	handset(&r, 1);					/* within the retry spacing, while the list has not shown the answer */
	run_ms(&r, 100);
	assert(status_of(&r).answered_by == PHONE_ANSWERED_HANDSET);
	assert(acts_of(&r, "ATA") == 1);
	r.listed = CALL_ACTIVE;
	run_ms(&r, 1500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_CALL && st.answered_by == PHONE_ANSWERED_HANDSET);
	assert(acts_of(&r, "ATA") == 1 && acts_of(&r, "AT+CHUP") == 0);
	assert(count_in(log_text(&r), "the handset came up while the front end was answering") == 1);
	rig_close(&r);
}

/* the front end dials: ATD goes out, the local progress tones stay silent, and the answer is the front end's */
static void test_dial_accepted(void)
{
	struct rig r;
	struct phone_status st;
	unsigned int mark;

	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 100);
	assert(phone_dial(r.p, "45", PHONE_ANSWERED_WEB) == 0);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_SETUP && st.dir == PHONE_DIR_OUT && st.call_id);
	run_to_act(&r, "ATD45;", 1000);
	assert(acts_of(&r, "ATD45;") == 1);

	mark = r.m.log_count;
	r.listed = CALL_ALERTING;
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_SETUP);
	assert(writes_of(&r, mark, REG_OCON, OCON_ENABLE_SET) == 0);	/* no local ringing tone: the peer hears the network's */

	r.listed = CALL_ACTIVE;
	run_ms(&r, 1500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_CALL && st.answered_by == PHONE_ANSWERED_WEB);
	assert(st.dir == PHONE_DIR_OUT && st.calls_out == 1 && st.calls_in == 0);
	assert(acts_of(&r, "AT+CHUP") == 0);
	assert(count_in(log_text(&r), "the front end has the call") == 1);

	r.listed = CALL_GONE;
	run_ms(&r, 1500);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(r.rec_count == 1 && r.rec[0].dir == PHONE_DIR_OUT && r.rec[0].result == PHONE_RES_ANSWERED);
	assert(r.rec[0].answered_by == PHONE_ANSWERED_WEB && !strcmp(r.rec[0].number, "45"));
	rig_close(&r);
}

/* the dial refusals, each changing nothing */
static void test_dial_refused(void)
{
	struct rig r;
	struct blocking *b;

	/* not a number this line dials */
	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 100);
	assert(phone_dial(r.p, NULL, PHONE_ANSWERED_WEB) == -EINVAL);
	assert(phone_dial(r.p, "", PHONE_ANSWERED_WEB) == -EINVAL);
	assert(phone_dial(r.p, "12a", PHONE_ANSWERED_WEB) == -EINVAL);
	assert(phone_dial(r.p, "123456789012345678901", PHONE_ANSWERED_WEB) == -EINVAL);	/* longer than DIAL_NUMBER_MAX */
	assert(phone_dial(r.p, "1+2", PHONE_ANSWERED_WEB) == -EINVAL);			/* + only leads */
	assert(phone_dial(r.p, "*", PHONE_ANSWERED_WEB) == -EINVAL);				/* a lone service character is no dial string */
	assert(status_of(&r).line == PHONE_LINE_IDLE && status_of(&r).call_id == 0 && r.act_count < ACTS_MAX);
	assert(acts_of(&r, "ATD12a;") == 0 && acts_of(&r, "ATD;") == 0);
	rig_close(&r);

	/* no modem */
	rig_init(&r);
	r.fm.lost = 1;
	rig_open(&r);
	run_ms(&r, 300);
	assert(!status_of(&r).modem_up);
	assert(phone_dial(r.p, "45", PHONE_ANSWERED_WEB) == -ENODEV);
	rig_close(&r);

	/* the handset is up: the line is not free */
	rig_init(&r);
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 300);
	assert(status_of(&r).line == PHONE_LINE_DIAL_TONE);
	assert(phone_dial(r.p, "45", PHONE_ANSWERED_WEB) == -EBUSY);
	assert(acts_of(&r, "ATD45;") == 0);
	handset(&r, 0);
	run_ms(&r, 600);
	assert(status_of(&r).line == PHONE_LINE_IDLE);

	/* a call is up */
	r.listed = CALL_ACTIVE;
	handset(&r, 1);
	run_ms(&r, 2500);
	assert(status_of(&r).line == PHONE_LINE_CALL);
	assert(phone_dial(r.p, "45", PHONE_ANSWERED_WEB) == -EBUSY);
	rig_close(&r);

	/* the blocking rules refuse the number: nothing is dialled, the log holds the record */
	b = rules(NULL, "45", 0);
	rig_init(&r);
	r.cfg.block = b;
	rig_open(&r);
	run_ms(&r, 100);
	assert(phone_dial(r.p, "45", PHONE_ANSWERED_WEB) == -EPERM);
	assert(acts_of(&r, "ATD45;") == 0);
	assert(status_of(&r).line == PHONE_LINE_IDLE && status_of(&r).call_id == 0);
	assert(r.rec_count == 1 && r.rec[0].dir == PHONE_DIR_OUT && r.rec[0].result == PHONE_RES_BLOCKED);
	assert(!strcmp(r.rec[0].number, "45") && !strstr(log_text(&r), "45"));
	assert(!strstr(log_text(&r), "the busy tone") && !strstr(log_text(&r), "the congestion tone"));
	rig_close(&r);
	free(b);
}

/* a front-end dial released before the answer: the line is free at once and no tone plays; the cause was never asked */
static void test_dial_released(void)
{
	struct rig r;

	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 100);
	assert(phone_dial(r.p, "45", PHONE_ANSWERED_WEB) == 0);
	run_to_act(&r, "ATD45;", 1000);
	r.listed = CALL_ALERTING;
	run_ms(&r, 2000);
	assert(status_of(&r).line == PHONE_LINE_SETUP);
	r.listed = CALL_GONE;
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_IDLE);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_IDLE);
	/* the release tone and its cause query are the handset's; with nobody at the handset the cause stays unknown,
	 * so the record is failed rather than no_answer */
	assert(r.rec_count == 1 && r.rec[0].result == PHONE_RES_FAILED && r.rec[0].cause == 0);
	assert(r.rec[0].answered_ms == 0 && r.rec[0].answered_by == PHONE_ANSWERED_NONE);
	assert(!strstr(log_text(&r), "the busy tone") && !strstr(log_text(&r), "the congestion tone"));
	rig_close(&r);
}

/* one digit on the cellular leg of the front end's call */
static void test_dtmf(void)
{
	struct rig r;

	fe_call_up(&r);
	assert(phone_dtmf(r.p, '5') == 0);
	run_ms(&r, 100);
	assert(acts_of(&r, "AT+VTS=5") == 1);
	assert(phone_dtmf(r.p, 'x') == -EINVAL);
	assert(phone_dtmf(r.p, '\0') == -EINVAL);
	assert(acts_of(&r, "AT+VTS=x") == 0);

	r.listed = CALL_GONE;
	run_ms(&r, 1500);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_IDLE);
	assert(phone_dtmf(r.p, '5') == -ENOENT);		/* no call to carry it */
	rig_close(&r);
}

/* the handset lifts in the front end's call: the owner changes and the call goes on */
static void test_handset_takes_over(void)
{
	struct rig r;
	struct phone_status st;
	unsigned int id, k, n = 0;

	fe_call_up(&r);
	id = status_of(&r).call_id;
	handset(&r, 1);
	run_ms(&r, 300);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_CALL && st.call_id == id && st.answered_by == PHONE_ANSWERED_HANDSET);
	assert(count_in(log_text(&r), "the handset takes the front end's call") == 1);
	for (k = 0; k < r.ev_count && k < EVENTS_MAX; k++)
		n += r.ev[k].kind == PHONE_EV_CALL && r.ev[k].call_id == id && r.ev[k].answered_by == PHONE_ANSWERED_HANDSET;
	assert(n >= 1);

	/* the far end clears: the call is the handset's now, so it ends the way any call of the handset's ends,
	 * with the release tone into the handset and no quiet end of its own */
	r.listed = CALL_GONE;
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_RELEASE);
	assert(!strstr(log_text(&r), "the front end's call ended"));
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	handset(&r, 0);
	run_ms(&r, 800);
	assert(status_of(&r).line == PHONE_LINE_IDLE);
	assert(r.rec_count == 1 && r.rec[0].answered_by == PHONE_ANSWERED_HANDSET);
	rig_close(&r);
}

/* call waiting keeps working in the front end's call: the second leg is followed and nothing turns it off */
static void test_waiting_kept(void)
{
	struct rig r;
	struct phone_status st;

	rig_init(&r);
	r.ops.legs = fm_legs;
	r.cfg.call_waiting = 1;
	rig_open(&r);
	r.listed = CALL_INCOMING;
	r.fm.legs[0] = (struct call_leg){ .idx = 1, .dir = 1, .stat = LEG_INCOMING, .number = "5551" };
	r.fm.nlegs = 1;
	run_to_line(&r, PHONE_LINE_RINGING, 3000);
	assert(phone_answer(r.p, PHONE_ANSWERED_WEB) == 0);
	run_to_act(&r, "ATA", 3000);
	r.listed = CALL_ACTIVE;
	r.fm.legs[0].stat = LEG_ACTIVE;
	run_ms(&r, 1500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_CALL && st.answered_by == PHONE_ANSWERED_WEB && st.leg_count == 1);

	r.act_count = 0;
	r.fm.legs[1] = (struct call_leg){ .idx = 2, .dir = 1, .stat = LEG_WAITING, .number = "5552" };
	r.fm.nlegs = 2;
	run_ms(&r, 1500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_CALL && st.answered_by == PHONE_ANSWERED_WEB);
	assert(st.leg_count == 2 && st.legs[1].stat == LEG_WAITING && st.legs[1].id > st.legs[0].id);
	assert(count_in(log_text(&r), "a call is waiting") == 1);
	assert(count_in(log_text(&r), "the waiting tone sounds") == 0);	/* nobody at the handset hears it */
	assert(acts_of(&r, "AT+CHLD=0") == 0 && acts_of(&r, "AT+CCWA=0") == 0);
	assert(!strstr(log_text(&r), "5552"));
	rig_close(&r);
}

/* the call goes away while the front end's answer is still on its way: the line is free and quiet */
static void test_answer_call_gone(void)
{
	struct rig r;

	rig_init(&r);
	rig_open(&r);
	r.listed = CALL_INCOMING;
	run_to_line(&r, PHONE_LINE_RINGING, 3000);
	assert(phone_answer(r.p, PHONE_ANSWERED_WEB) == 0);
	run_to_act(&r, "ATA", 3000);
	assert(status_of(&r).line == PHONE_LINE_RINGING);	/* the answer is on its way: not idle */
	assert(status_of(&r).answered_by == PHONE_ANSWERED_WEB);

	r.listed = CALL_GONE;
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_IDLE);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_IDLE && status_of(&r).call_id == 0);
	assert(count_in(log_text(&r), "the front end's call ended") == 1);
	assert(!strstr(log_text(&r), "the busy tone") && !strstr(log_text(&r), "the congestion tone"));
	/* the answer went out but the call never became active: the record is missed, and still says who tried */
	assert(r.rec_count == 1 && r.rec[0].result == PHONE_RES_MISSED && r.rec[0].answered_ms == 0);
	assert(r.rec[0].answered_by == PHONE_ANSWERED_WEB);
	rig_close(&r);
}

/* the page's flash: the handset's flash key in a call, and the key after it a code (TS 22.030) */
static void test_front_end_flash(void)
{
	struct rig r;

	rig_init(&r);
	r.ops.legs = fm_legs;
	r.cfg.conference = 1;
	rig_open(&r);
	r.listed = CALL_INCOMING;
	r.fm.legs[0] = (struct call_leg){ .idx = 1, .dir = 1, .stat = LEG_INCOMING, .number = "5551" };
	r.fm.nlegs = 1;
	run_to_line(&r, PHONE_LINE_RINGING, 3000);
	assert(phone_answer(r.p, PHONE_ANSWERED_WEB) == 0);
	run_to_act(&r, "ATA", 3000);
	r.listed = CALL_ACTIVE;
	r.fm.legs[0].stat = LEG_ACTIVE;
	run_ms(&r, 1500);
	assert(status_of(&r).answered_by == PHONE_ANSWERED_WEB);

	/* a flash with no key after it: the call is held at the window's end (AT+CHLD=2) */
	r.act_count = 0;
	assert(phone_flash(r.p) == 0);
	run_ms(&r, 3200);			/* FLASH_CODE_MS is 3000 */
	assert(acts_of(&r, "AT+CHLD=2") == 1);
	assert(count_in(log_text(&r), "hook flash of 100 ms (the page)") == 1);
	assert(count_in(log_text(&r), "flash: the call is held") == 1);

	/* the network holds one leg and resumes the other beside it: the page's flash and the key 3 join them */
	r.fm.legs[0].stat = LEG_ACTIVE;
	r.fm.legs[1] = (struct call_leg){ .idx = 2, .dir = 1, .stat = LEG_HELD, .number = "5552" };
	r.fm.nlegs = 2;
	run_ms(&r, 1500);
	assert(status_of(&r).leg_count == 2);
	r.act_count = 0;
	assert(phone_flash(r.p) == 0);
	assert(phone_dtmf(r.p, '3') == 0);
	run_ms(&r, 1000);			/* the queued command goes out at a step */
	assert(acts_of(&r, "AT+CHLD=3") == 1);
	assert(count_in(log_text(&r), "flash code: the two calls are joined") == 1);

	/* a key that is no code after a flash goes to the network as a tone */
	r.act_count = 0;
	assert(phone_flash(r.p) == 0);
	assert(phone_dtmf(r.p, '#' ) == 0);
	run_ms(&r, 1000);
	assert(acts_of(&r, "AT+CHLD=3") == 0);
	rig_close(&r);

	/* nothing a flash can do on a free line */
	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 100);
	assert(phone_flash(r.p) == -ENOENT);
	rig_close(&r);
}

/* a waiting call taken by the page's flash: its record says which door took it */
static void test_waiting_taken_door(void)
{
	struct rig r;
	unsigned int k, found = 0;

	rig_init(&r);
	r.ops.legs = fm_legs;
	r.cfg.call_waiting = 1;
	rig_open(&r);
	r.listed = CALL_INCOMING;
	r.fm.legs[0] = (struct call_leg){ .idx = 1, .dir = 1, .stat = LEG_INCOMING, .number = "5551" };
	r.fm.nlegs = 1;
	run_to_line(&r, PHONE_LINE_RINGING, 3000);
	assert(phone_answer(r.p, PHONE_ANSWERED_WEB) == 0);
	run_to_act(&r, "ATA", 3000);
	r.listed = CALL_ACTIVE;
	r.fm.legs[0].stat = LEG_ACTIVE;
	run_ms(&r, 1500);

	r.fm.legs[1] = (struct call_leg){ .idx = 2, .dir = 1, .stat = LEG_WAITING, .number = "5552" };
	r.fm.nlegs = 2;
	run_ms(&r, 1500);
	assert(count_in(log_text(&r), "a call is waiting") == 1);

	/* the page flashes; the network takes the waiting call and holds the first */
	assert(phone_flash(r.p) == 0);
	run_ms(&r, 3200);
	assert(count_in(log_text(&r), "flash: the waiting call is taken") == 1);
	r.fm.legs[0].stat = LEG_HELD;
	r.fm.legs[1].stat = LEG_ACTIVE;
	run_ms(&r, 1500);
	assert(status_of(&r).legs[1].stat == LEG_ACTIVE);

	/* both end: the waiting call's record carries the page's door */
	r.listed = CALL_GONE;
	r.fm.nlegs = 0;
	run_ms(&r, 1500);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	for (k = 0; k < r.rec_count; k++)
		if (r.rec[k].id == 2) {
			found = 1;
			assert(r.rec[k].answered_by == PHONE_ANSWERED_WEB);
		}
	assert(found);
	rig_close(&r);
}

/* a held call comes back by a flash from the recall dial tone, or from an idle line with the handset up */
static void test_flash_resume_state(void)
{
	assert(phone_flash_resume_ok(PHONE_LINE_DIAL_TONE));
	assert(phone_flash_resume_ok(PHONE_LINE_IDLE));	/* the dial tone's start is still settling */
	assert(!phone_flash_resume_ok(PHONE_LINE_CALL));
	assert(!phone_flash_resume_ok(PHONE_LINE_RELEASE));
	assert(!phone_flash_resume_ok(PHONE_LINE_RINGING));
}

/* a call the installed service dials is the SIP door's: the view and the record say so */
static void test_dial_sip_door(void)
{
	struct rig r;
	struct phone_status st;

	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 100);
	assert(phone_dial(r.p, "46", PHONE_ANSWERED_SIP) == 0);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_SETUP && st.dir == PHONE_DIR_OUT && st.call_id);
	run_to_act(&r, "ATD46;", 1000);
	r.listed = CALL_ACTIVE;
	run_ms(&r, 1500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_CALL && st.answered_by == PHONE_ANSWERED_SIP);
	r.listed = CALL_GONE;
	run_ms(&r, 1500);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(r.rec_count == 1 && r.rec[0].answered_by == PHONE_ANSWERED_SIP && !strcmp(r.rec[0].number, "46"));
	rig_close(&r);
}

int main(void)
{
	test_answer_accepted();
	test_answer_refused();
	test_answer_and_lift();
	test_dial_accepted();
	test_dial_sip_door();
	test_dial_refused();
	test_dial_released();
	test_dtmf();
	test_handset_takes_over();
	test_waiting_kept();
	test_answer_call_gone();
	test_front_end_flash();
	test_flash_resume_state();
	test_waiting_taken_door();
	printf("dial_answer: all tests passed\n");
	return 0;
}
