// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * The daemon's use of the core: phone_open / phone_step / phone_close driven as the daemon's event loop drives them
 * (one step per HOOK_POLL_MS on the caller's clock), with the shared fake chip and fake modem.
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
#include "alarm.h"
#include "answer.h"
#include "cid.h"
#include "forward.h"
#include "blocking.h"
#include "phonebook.h"
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
	struct tm clock;			/* the wall clock the alarms see */
	int clock_ok;
	int refuse_atd;				/* the modem answers ERROR to a dial */
	int refuse_chld1;			/* ... to AT+CHLD=1x */
	int refuse_ccfc;			/* ... to a forwarding registration */
	int refuse_ata;				/* ... to an answer */
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
	if (r->refuse_chld1 && !strncmp(what, "AT+CHLD=1", 9))
		return -EIO;
	if (r->refuse_ccfc && !strncmp(what, "AT+CCFC=2,3", 11))
		return -EIO;
	if (r->refuse_ata && !strcmp(what, "ATA"))
		return -EIO;
	return r->refuse_atd && !strncmp(what, "ATD", 3) ? -EIO : 0;
}

static void rig_event(void *ctx, const struct phone_event *ev)
{
	struct rig *r = ctx;

	if (r->ev_count < EVENTS_MAX)
		r->ev[r->ev_count] = *ev;
	r->ev_count++;
}

static int rig_wall_clock(void *ctx, struct tm *out)
{
	struct rig *r = ctx;

	*out = r->clock;
	return r->clock_ok ? 0 : -EAGAIN;
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

static int cause_busy(void *user)
{
	(void)user;
	return 17;
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
	r->hooks = (struct phone_hooks){ .ctx = r, .event = rig_event, .call_ended = rig_ended, .wall_clock = rig_wall_clock };
	r->ops.number = caller_number;
	r->cfg = (struct phone_cfg){ .channel = TEST_CH, .service_log = 1 };
}

static void rig_open(struct rig *r)
{
	the_rig = r;
	assert(phone_open(&r->p, &r->c, &r->b, &r->ops, &r->cfg, &r->hooks, r->log) == 0 && r->p);
	r->now = r->c.ops->now_ms(&r->m);
}

/* the handset: up = the loop is closed (the chip's register shows the on-hook bit set for the caller being off-hook, as the other tests do) */
static void handset(struct rig *r, int up)
{
	r->m.reg[TEST_CH][REG_LCRRTP] = up ? LCR_ON_HOOK : 0;
}

/* run the daemon's loop for ms milliseconds: one step per HOOK_POLL_MS */
static void run_ms(struct rig *r, unsigned int ms)
{
	unsigned int t;

	for (t = 0; t < ms; t += HOOK_POLL_MS) {
		r->m.vtime_ms += HOOK_POLL_MS;
		r->now = r->c.ops->now_ms(&r->m);
		assert(phone_step(r->p, r->now) == 0);
	}
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

static unsigned int writes_of(const struct rig *r, unsigned int from, unsigned int reg, unsigned int value)
{
	unsigned int k, n = 0;

	for (k = from; k < r->m.log_count; k++)
		n += r->m.log[k].kind == 'r' && r->m.log[k].addr == reg && r->m.log[k].value == value;
	return n;
}

/* the line comes up with no modem and no hooks at all: the core does not need them */
static void test_open_close(void)
{
	struct rig r;
	struct phone *p = NULL;
	struct phone_status st;
	uint8_t bad[64];

	rig_init(&r);
	assert(phone_open(&p, &r.c, &r.b, NULL, &r.cfg, NULL, r.log) == 0 && p);
	phone_status(p, &st);
	assert(st.line == PHONE_LINE_IDLE && !st.modem_up && !st.off_hook && st.call_id == 0 && st.dir == PHONE_DIR_NONE);
	assert(st.sim_ready == PHONE_UNKNOWN && st.registered == PHONE_UNKNOWN && st.ims_registered == PHONE_UNKNOWN);
	assert(st.signal_dbm == PHONE_DBM_UNKNOWN && !st.number[0]);
	assert(phone_close(p, 0) == 0);
	assert(r.m.user[TEST_CH] == 0);			/* protected mode left, as the stage tool leaves it */
	fclose(r.log);
	free(r.lbuf);

	/* a board file without the sections the line needs: refused, nothing handed out, the chip untouched */
	rig_init(&r);
	memset(bad, 0, sizeof(bad));
	memcpy(bad, "AWBC", 4);
	*(uint32_t *)(bad + 4) = 1;
	*(uint32_t *)(bad + 8) = 0;
	memset(bad + 12, 0x5a, 32);
	{
		struct board empty;
		uint32_t crc = board_crc32(bad, 44);

		memcpy(bad + 44, &crc, 4);
		assert(board_parse(bad, 48, &empty) == 0);
		r.m.frames = 0;
		assert(phone_open(&p, &r.c, &empty, &r.ops, &r.cfg, &r.hooks, r.log) == -EINVAL && p == NULL && r.m.frames == 0);
	}
	fclose(r.log);
	free(r.lbuf);
}

/* the modem port is missing at the start and appears later: the line works meanwhile, the setup goes out when it is up */
static void test_modem_late(void)
{
	struct rig r;
	struct phone_status st;

	rig_init(&r);
	r.fm.lost = 1;
	rig_open(&r);
	run_ms(&r, 200);
	st = status_of(&r);
	assert(!st.modem_up && st.line == PHONE_LINE_IDLE && r.act_count == 0 && r.fm.begins == 0);
	assert(count_in(log_text(&r), "the modem port was lost") == 1);

	/* a handset lifted without a modem hears the congestion tone: there is no service to dial into (the line itself works) */
	handset(&r, 1);
	run_ms(&r, 100);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_NO_SERVICE && st.off_hook);
	handset(&r, 0);
	run_ms(&r, 400);
	assert(status_of(&r).line == PHONE_LINE_IDLE);

	/* the port appears: the daemon opens it and tells the core */
	r.fm.lost = 0;
	phone_modem_up(r.p);
	run_ms(&r, 600);
	st = status_of(&r);
	assert(st.modem_up);
	assert(r.act_count >= 4 && !strcmp(r.acts[0], "ATE0") && !strcmp(r.acts[1], "AT+CMEE=1") && !strcmp(r.acts[2], "AT+CLIP=1") && !strcmp(r.acts[3], "AT+CRC=1"));
	assert(r.fm.begins >= 5);			/* the four commands and the call list */
	assert(st.modem_reply_ms != 0);
	rig_close(&r);
}

/* the port is lost in a call: the caller hears the release tone, the status shows no modem, and the line recovers on reopen */
static void test_port_lost_in_call(void)
{
	struct rig r;
	struct phone_status st;
	unsigned int mark;

	rig_init(&r);
	rig_open(&r);
	r.listed = CALL_ACTIVE;
	handset(&r, 1);
	run_ms(&r, 2500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_CALL && st.modem_up && st.call == CALL_ACTIVE && st.answered_ms != 0);

	mark = r.m.log_count;
	r.fm.lost = 1;
	run_ms(&r, 1500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_RELEASE && !st.modem_up);
	assert(count_in(log_text(&r), "the modem port was lost") == 1);
	assert(writes_of(&r, mark, REG_OCON, OCON_ENABLE_SET) >= 1);	/* the release tone is playing */

	/* the handset goes down: the line is idle again with no modem to hang up */
	handset(&r, 0);
	run_ms(&r, 500);
	assert(status_of(&r).line == PHONE_LINE_IDLE);

	/* the port is back */
	r.listed = CALL_NONE;
	r.fm.lost = 0;
	r.act_count = 0;
	phone_modem_up(r.p);
	run_ms(&r, 600);
	st = status_of(&r);
	assert(st.modem_up && acts_of(&r, "AT+CLIP=1") == 1 && acts_of(&r, "AT+CRC=1") == 1);
	rig_close(&r);
}

/* new settings during a call wait for the idle moment with the handset down */
static void test_cfg_in_call(void)
{
	struct rig r;
	struct phone_cfg next;

	rig_init(&r);
	rig_open(&r);
	r.listed = CALL_ACTIVE;
	handset(&r, 1);
	run_ms(&r, 2500);
	assert(status_of(&r).line == PHONE_LINE_CALL);

	next = r.cfg;
	next.hook.flash_min_ms = 150;			/* a 100 ms break is a flash with the old limits (80..300), a bounce with these */
	next.hook.flash_max_ms = 400;
	assert(phone_set_cfg(r.p, &next) == 0);
	handset(&r, 0);
	run_ms(&r, 100);
	handset(&r, 1);
	run_ms(&r, 600);
	assert(count_in(log_text(&r), "hook flash of") == 1 && count_in(log_text(&r), "bounce") == 0);	/* the old limits judged it */
	assert(status_of(&r).line == PHONE_LINE_CALL);

	/* the caller hangs up: the call ends, the line is idle with the handset down, and only now the new limits take hold */
	handset(&r, 0);
	run_ms(&r, 350);
	assert(status_of(&r).line == PHONE_LINE_IDLE);
	assert(acts_of(&r, "AT+CHUP") >= 1);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	handset(&r, 1);					/* a new call: the dial tone */
	run_ms(&r, 300);
	assert(status_of(&r).line == PHONE_LINE_DIAL_TONE);
	handset(&r, 0);
	run_ms(&r, 100);
	handset(&r, 1);
	run_ms(&r, 600);
	assert(count_in(log_text(&r), "hook flash of") == 1);			/* still the one from the call */
	assert(count_in(log_text(&r), "hook bounce of 100 ms ignored") == 1);	/* the new limits judged this one */

	/* a different channel is refused and changes nothing */
	next.channel = 0;
	assert(phone_set_cfg(r.p, &next) == -EINVAL);
	rig_close(&r);
}

/* settings given while the handset is up on an idle line also wait */
static void test_cfg_offhook_idle(void)
{
	struct rig r;
	struct phone_cfg next;

	rig_init(&r);
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 300);
	assert(status_of(&r).line == PHONE_LINE_DIAL_TONE);
	next = r.cfg;
	next.hook.flash_min_ms = 150;
	next.hook.flash_max_ms = 400;
	assert(phone_set_cfg(r.p, &next) == 0);
	handset(&r, 0);
	run_ms(&r, 100);
	handset(&r, 1);
	run_ms(&r, 500);
	assert(count_in(log_text(&r), "hook flash of") == 1 && count_in(log_text(&r), "bounce") == 0);
	rig_close(&r);
}

static void test_ring_test(void)
{
	struct rig r;
	struct phone_status st;

	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 100);
	assert(phone_ring_test(r.p, 0) == -EINVAL);
	assert(phone_ring_test(r.p, RING_TEST_MAX_SECONDS + 1) == -EINVAL);

	/* refused while the handset is up (on the dial tone, in a call) */
	handset(&r, 1);
	run_ms(&r, 100);
	assert(status_of(&r).line == PHONE_LINE_DIAL_TONE);
	assert(phone_ring_test(r.p, 2) == -EBUSY);
	assert(status_of(&r).line == PHONE_LINE_DIAL_TONE);
	r.listed = CALL_ACTIVE;
	run_ms(&r, 2500);
	assert(status_of(&r).line == PHONE_LINE_CALL);
	assert(phone_ring_test(r.p, 2) == -EBUSY);
	handset(&r, 0);
	r.listed = CALL_NONE;
	run_ms(&r, 2000);
	assert(status_of(&r).line == PHONE_LINE_IDLE);

	/* accepted on an idle line with the handset down; a second one while it rings is refused; it ends by itself */
	assert(phone_ring_test(r.p, 1) == 0);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_RINGING && st.call_id == 0);
	assert(phone_ring_test(r.p, 1) == -EBUSY);
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_IDLE);

	/* lifting the handset during a test ring stops it */
	assert(phone_ring_test(r.p, 5) == 0);
	run_ms(&r, 300);
	assert(status_of(&r).line == PHONE_LINE_RINGING);
	handset(&r, 1);
	run_ms(&r, 100);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_DIAL_TONE);
	rig_close(&r);
}

static void test_hangup(void)
{
	struct rig r;

	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 300);
	assert(phone_hangup(r.p) == -ENOENT);		/* no call, no attempt */
	assert(acts_of(&r, "AT+CHUP") == 0);

	r.listed = CALL_ACTIVE;
	handset(&r, 1);
	run_ms(&r, 2500);
	assert(status_of(&r).line == PHONE_LINE_CALL);
	assert(phone_hangup(r.p) == 0);
	assert(phone_hangup(r.p) == 0);			/* a second request does not queue a second command */
	run_ms(&r, 300);
	assert(acts_of(&r, "AT+CHUP") == 1);

	/* an incoming call that rings is ended too */
	r.listed = CALL_NONE;
	handset(&r, 0);
	run_ms(&r, 2000);
	r.act_count = 0;
	r.listed = CALL_INCOMING;
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_RINGING && status_of(&r).dir == PHONE_DIR_IN);
	assert(phone_hangup(r.p) == 0);
	run_ms(&r, 300);
	assert(acts_of(&r, "AT+CHUP") == 1);
	rig_close(&r);
}

/* a call's course reaches the front end as events, and an event has no place for a number */
static void test_events(void)
{
	struct rig r;
	unsigned int k, lines = 0, calls = 0;
	int saw_ring = 0, saw_call = 0;

	rig_init(&r);
	rig_open(&r);
	r.listed = CALL_INCOMING;
	run_ms(&r, 1500);
	handset(&r, 1);
	r.listed = CALL_ACTIVE;
	run_ms(&r, 3000);
	handset(&r, 0);
	r.listed = CALL_NONE;
	run_ms(&r, 2000);
	assert(r.ev_count > 0 && r.ev_count <= EVENTS_MAX);
	for (k = 0; k < r.ev_count; k++) {
		const struct phone_event *e = &r.ev[k];

		if (e->kind == PHONE_EV_LINE)
			lines++;
		else
			calls++;
		saw_ring |= e->kind == PHONE_EV_LINE && e->line == PHONE_LINE_RINGING;
		saw_call |= e->kind == PHONE_EV_LINE && e->line == PHONE_LINE_CALL && e->dir == PHONE_DIR_IN && e->call_id == 1;
		assert(k == 0 || e->at_ms >= r.ev[k - 1].at_ms);
	}
	assert(lines >= 4 && calls >= 2 && saw_ring && saw_call);
	assert(r.ev[r.ev_count - 1].line == PHONE_LINE_IDLE);
	assert(status_of(&r).calls_in == 1);
	rig_close(&r);
}

/* the number is in the status of a call and nowhere in the log */
static void test_no_number_in_log(void)
{
	struct rig r;

	rig_init(&r);
	r.m.dtmf_events[0].at = 300; r.m.dtmf_events[0].code = 0x04;		/* '4' */
	r.m.dtmf_events[1].at = 375; r.m.dtmf_events[1].code = 0x0c;		/* '#' */
	r.m.dtmf_event_count = 2;
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 800);
	assert(acts_of(&r, "ATD4;") == 1);
	assert(!strcmp(status_of(&r).number, "4") && status_of(&r).dir == PHONE_DIR_OUT && status_of(&r).line == PHONE_LINE_SETUP);
	assert(strstr(log_text(&r), "dialing (1 digits)") && !strstr(log_text(&r), "ATD4") && !strstr(log_text(&r), "dtmf 4"));
	rig_close(&r);
}

/* a stall: a step that comes late is a gap in the record and nothing else */
static void test_late_step(void)
{
	struct rig r;
	struct phone_status st;

	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 200);
	r.m.vtime_ms += 60;				/* the loop was away for 60 ms */
	r.now = r.c.ops->now_ms(&r.m);
	assert(phone_step(r.p, r.now) == 0);
	run_ms(&r, 100);
	st = status_of(&r);
	assert(st.hook_gaps >= 1 && st.hook_gap_max_ms >= 60 && st.faults == 0);
	rig_close(&r);
}


/*
 * An incoming call while the handset is up finds the line in use. A telephone line that is off-hook is busy: the caller
 * is told so (user determined user busy, 3GPP TS 22.030 / TS 24.008 cause 17) and the phone does not ring; nothing
 * answers the call by itself. Only a handset lifted AFTER the call was offered (the phone rang, or would have) answers it
 * (the ring trip, ITU-T Q.31 / ETSI EN 300 001 clause on the called party's answer). Reported on the line 2026-10-23
 * a call that arrived at a handset on the dial tone was answered at once, and a call that arrived during the busy tone
 * rang the caller for nine seconds and was answered by the next hook toggle.
 */
static void test_busy_line_refuses(void)
{
	struct rig r;
	struct phone_status st;

	/* on the dial tone */
	rig_init(&r);
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 300);
	assert(status_of(&r).line == PHONE_LINE_DIAL_TONE);
	r.listed = CALL_INCOMING;
	run_ms(&r, 2500);
	assert(acts_of(&r, "ATA") == 0);			/* never answered by itself */
	assert(acts_of(&r, "AT+CHUP") >= 1);			/* the caller is released as busy */
	st = status_of(&r);
	assert(st.line == PHONE_LINE_DIAL_TONE && st.off_hook);	/* the caller at the handset keeps the line */
	assert(count_in(log_text(&r), "ring started") == 0);
	assert(count_in(log_text(&r), "line is in use") == 1);	/* said once, not at every poll */
	r.listed = CALL_NONE;					/* the network has released it */
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_DIAL_TONE);
	rig_close(&r);

	/* during the busy tone after the other side ended a call, the new caller is refused too and nothing rings later */
	rig_init(&r);
	rig_open(&r);
	r.listed = CALL_ACTIVE;
	handset(&r, 1);
	run_ms(&r, 2500);
	assert(status_of(&r).line == PHONE_LINE_CALL);
	r.listed = CALL_GONE;					/* the remote ended it (the AT client reports a listed call that vanished as gone) */
	run_ms(&r, 1200);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_RELEASE);
	r.act_count = 0;
	r.listed = CALL_INCOMING;
	run_ms(&r, 2500);
	assert(acts_of(&r, "ATA") == 0 && acts_of(&r, "AT+CHUP") >= 1);
	assert(status_of(&r).line == PHONE_LINE_RELEASE);	/* the busy tone goes on for the caller at the handset */
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	handset(&r, 0);					/* hang up: the refused call is gone, the phone stays quiet */
	run_ms(&r, 2500);
	assert(status_of(&r).line == PHONE_LINE_IDLE && count_in(log_text(&r), "ring started") == 0);
	rig_close(&r);

	/* a refused call the network keeps alive is offered when the handset goes down (a waiting call that was not taken) */
	rig_init(&r);
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 300);
	r.listed = CALL_INCOMING;			/* the modem does not drop it */
	run_ms(&r, 2500);
	assert(count_in(log_text(&r), "ring started") == 0);
	handset(&r, 0);
	run_ms(&r, 2500);
	assert(count_in(log_text(&r), "ring started") == 1 && status_of(&r).line == PHONE_LINE_RINGING);
	rig_close(&r);
}

/* the ring trip: the lift of a handset that was offered the call answers at once, not at the next call list */
static void test_ring_trip_answers_at_once(void)
{
	struct rig r;

	rig_init(&r);
	rig_open(&r);
	r.listed = CALL_INCOMING;
	run_ms(&r, 2500);
	assert(status_of(&r).line == PHONE_LINE_RINGING && acts_of(&r, "ATA") == 0);
	handset(&r, 1);
	run_ms(&r, 30);					/* within a few steps of the lift, long before the next call list is due */
	assert(acts_of(&r, "ATA") == 1);
	assert(count_in(log_text(&r), "ring stopped (the handset was lifted)") == 1);
	r.listed = CALL_ACTIVE;
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_CALL && status_of(&r).dir == PHONE_DIR_IN);
	rig_close(&r);
}

/*
 * The answering machine: an offered call left alone for the delay is answered by the line itself, with the handset down.
 * The call is nobody's at the handset (it never rings again, a lift takes it over, no tone follows the caller's hang-up),
 * and a blocked caller, a busy line or a ring test is never answered by it. The core runs only the state machine and the
 * events; the greeting and the message are the front end's (the ANSWERING / ANSWERED / TAKEN / ENDED events).
 */
static void machine_rig(struct rig *r, unsigned int delay_ms)
{
	rig_init(r);
	r->cfg.machine = 1;
	r->cfg.machine_delay_ms = delay_ms;
	rig_open(r);
}

static struct blocking *rules(const char *in, const char *out, int anonymous);

static unsigned int machine_events(const struct rig *r, int why)
{
	unsigned int k, n = 0;

	for (k = 0; k < r->ev_count && k < EVENTS_MAX; k++)
		n += r->ev[k].kind == PHONE_EV_MACHINE && r->ev[k].machine == why;
	return n;
}

static void test_machine_answer(void)
{
	struct rig r;
	struct phone_status st;
	struct blocking *b;

	/* off (the default): the phone rings and nothing answers by itself */
	rig_init(&r);
	rig_open(&r);
	r.listed = CALL_INCOMING;
	run_ms(&r, 6000);
	assert(acts_of(&r, "ATA") == 0 && status_of(&r).machine == PHONE_MACHINE_NONE && status_of(&r).line == PHONE_LINE_RINGING);
	assert(count_in(log_text(&r), "the answering machine") == 0);
	rig_close(&r);

	/* delay 0: answered at once, no ring; the answer reaches the call; no hang-up follows; the caller's end is silent */
	machine_rig(&r, 0);
	r.listed = CALL_INCOMING;
	run_ms(&r, 1200);
	assert(acts_of(&r, "ATA") == 1 && count_in(log_text(&r), "the answering machine answers (") == 1);
	assert(status_of(&r).machine == PHONE_MACHINE_ANSWERING && status_of(&r).line == PHONE_LINE_IDLE);
	assert(count_in(log_text(&r), "the phone rings for the incoming call") == 0 && machine_events(&r, PHONE_MACHINE_ANSWERING) == 1);
	r.listed = CALL_ACTIVE;
	run_ms(&r, 1500);			/* the call list catches up */
	st = status_of(&r);
	assert(st.machine == PHONE_MACHINE_ANSWERED && st.line == PHONE_LINE_CALL && st.calls_in == 1 && st.dir == PHONE_DIR_IN);
	assert(acts_of(&r, "AT+CHUP") == 0 && acts_of(&r, "ATA") == 1);	/* the answer is not a hang-up, and is never repeated */
	assert(count_in(log_text(&r), "the answering machine has the call") == 1);
	assert(count_in(log_text(&r), "the phone rings for the incoming call") == 0);
	assert(machine_events(&r, PHONE_MACHINE_ANSWERED) == 1 && !strstr(log_text(&r), "5551"));
	r.listed = CALL_GONE;			/* the caller ended it */
	run_ms(&r, 1200);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_IDLE && st.machine == PHONE_MACHINE_NONE && !st.off_hook);
	assert(machine_events(&r, PHONE_MACHINE_ENDED) == 1 && count_in(log_text(&r), "the answering machine's call ended") == 1);
	assert(!strstr(log_text(&r), "the busy tone") && !strstr(log_text(&r), "the congestion tone"));	/* nobody at the handset waits for a tone */
	r.listed = CALL_NONE;
	run_ms(&r, 1200);
	assert(r.rec_count == 1 && r.rec[0].dir == PHONE_DIR_IN && r.rec[0].answered_ms && r.rec[0].result == PHONE_RES_ANSWERED);
	/* the machine can take the next call too, with a new id */
	r.listed = CALL_INCOMING;
	run_ms(&r, 1500);
	assert(acts_of(&r, "ATA") == 2 && status_of(&r).call_id == 2 && status_of(&r).machine == PHONE_MACHINE_ANSWERING);
	r.listed = CALL_NONE;			/* the caller gave up before the answer was listed: nothing is recorded twice */
	run_ms(&r, 1200);
	assert(status_of(&r).line == PHONE_LINE_IDLE && status_of(&r).machine == PHONE_MACHINE_NONE);
	assert(count_in(log_text(&r), "the answering machine's answer did not reach the call") == 1);
	assert(machine_events(&r, PHONE_MACHINE_ENDED) == 1);	/* no message was begun: no end for it */
	assert(r.rec_count == 2 && r.rec[1].result == PHONE_RES_MISSED);
	rig_close(&r);

	/* the delay is counted from the call's first listing: the phone rings first, the machine answers later */
	machine_rig(&r, 4000);
	r.listed = CALL_INCOMING;
	run_ms(&r, 2500);
	assert(acts_of(&r, "ATA") == 0 && status_of(&r).machine == PHONE_MACHINE_NONE && status_of(&r).line == PHONE_LINE_RINGING);
	assert(count_in(log_text(&r), "the phone rings for the incoming call") == 1);
	run_ms(&r, 3000);
	assert(acts_of(&r, "ATA") == 1 && status_of(&r).machine == PHONE_MACHINE_ANSWERING);
	assert(count_in(log_text(&r), "the answering machine answers (") == 1);
	assert(count_in(log_text(&r), "ring stopped (the answering machine answers)") == 1);
	r.listed = CALL_ACTIVE;
	run_ms(&r, 1500);
	assert(status_of(&r).machine == PHONE_MACHINE_ANSWERED && status_of(&r).line == PHONE_LINE_CALL);
	rig_close(&r);

	/* the handset takes the call over: the machine gives way, the call carries on, and no second answer goes out */
	machine_rig(&r, 0);
	r.listed = CALL_INCOMING;
	run_ms(&r, 1200);
	r.listed = CALL_ACTIVE;
	run_ms(&r, 1500);
	assert(status_of(&r).machine == PHONE_MACHINE_ANSWERED);
	handset(&r, 1);
	run_ms(&r, 300);
	st = status_of(&r);
	assert(st.machine == PHONE_MACHINE_NONE && st.line == PHONE_LINE_CALL && acts_of(&r, "ATA") == 1);
	assert(count_in(log_text(&r), "the handset takes the answering machine's call") == 1 && machine_events(&r, PHONE_MACHINE_TAKEN) == 1);
	run_ms(&r, 2000);			/* the person's call: no hang-up, no tone */
	assert(acts_of(&r, "AT+CHUP") == 0 && status_of(&r).line == PHONE_LINE_CALL);
	r.listed = CALL_GONE;			/* the caller ended it: now the person at the handset gets the release tone */
	run_ms(&r, 1200);
	assert(status_of(&r).line == PHONE_LINE_RELEASE && strstr(log_text(&r), "the remote ended the call"));
	handset(&r, 0);
	run_ms(&r, 1200);
	assert(status_of(&r).line == PHONE_LINE_IDLE);
	rig_close(&r);

	/* a handset up when the call comes: the line is in use, the caller is released, the machine is not involved */
	machine_rig(&r, 0);
	handset(&r, 1);
	run_ms(&r, 300);
	r.listed = CALL_INCOMING;
	run_ms(&r, 2500);
	assert(acts_of(&r, "ATA") == 0 && acts_of(&r, "AT+CHUP") >= 1 && status_of(&r).machine == PHONE_MACHINE_NONE);
	assert(count_in(log_text(&r), "incoming call refused: the line is in use") == 1 && count_in(log_text(&r), "the answering machine") == 0);
	rig_close(&r);

	/* a blocked caller is released before any answer, the machine included */
	b = rules("5551", NULL, 0);
	rig_init(&r);
	r.cfg.machine = 1;
	r.cfg.block = b;
	rig_open(&r);
	r.listed = CALL_INCOMING;
	run_ms(&r, 2500);
	assert(acts_of(&r, "ATA") == 0 && acts_of(&r, "AT+CHUP") >= 1 && status_of(&r).machine == PHONE_MACHINE_NONE);
	assert(count_in(log_text(&r), "an incoming call was blocked") == 1 && count_in(log_text(&r), "the answering machine") == 0);
	r.listed = CALL_NONE;
	run_ms(&r, 1200);
	assert(r.rec_count == 1 && r.rec[0].result == PHONE_RES_BLOCKED && !strstr(log_text(&r), "5551"));
	rig_close(&r);
	free(b);

	/* every answer refused: the machine gives up once, the phone rings on, and the lift still answers */
	machine_rig(&r, 0);
	r.refuse_ata = 1;
	r.listed = CALL_INCOMING;
	run_ms(&r, 6000);
	assert(acts_of(&r, "ATA") == 3 && count_in(log_text(&r), "the answering machine could not answer: the phone rings on") == 1);
	assert(status_of(&r).machine == PHONE_MACHINE_NONE && status_of(&r).line == PHONE_LINE_RINGING);
	assert(count_in(log_text(&r), "the answering machine answers (") == 1);
	run_ms(&r, 4000);			/* it does not stop the ring again for a call it cannot answer */
	assert(acts_of(&r, "ATA") == 3 && count_in(log_text(&r), "the answering machine answers (") == 1);
	handset(&r, 1);
	run_ms(&r, 300);
	assert(acts_of(&r, "ATA") == 4);	/* the machine's failed tries are not the person's */
	rig_close(&r);
}

static void test_machine_waiting(void)
{
	struct rig r;

	/* a second call while the machine has the line: refused (the line is in use), no waiting tone into the message */
	rig_init(&r);
	r.ops.legs = fm_legs;
	r.cfg.machine = 1;
	rig_open(&r);
	r.listed = CALL_INCOMING;
	r.fm.legs[0] = (struct call_leg){ .idx = 1, .dir = 1, .stat = LEG_INCOMING, .number = "5551" };
	r.fm.nlegs = 1;
	run_ms(&r, 1500);
	r.listed = CALL_ACTIVE;
	r.fm.legs[0].stat = LEG_ACTIVE;
	run_ms(&r, 1500);
	assert(status_of(&r).machine == PHONE_MACHINE_ANSWERED && status_of(&r).line == PHONE_LINE_CALL);
	r.act_count = 0;
	r.fm.legs[1] = (struct call_leg){ .idx = 2, .dir = 1, .stat = LEG_WAITING, .number = "5552" };
	r.fm.nlegs = 2;
	run_ms(&r, 1500);
	assert(acts_of(&r, "AT+CHLD=0") >= 1 && count_in(log_text(&r), "a waiting call was refused: the answering machine has the line") == 1);
	assert(count_in(log_text(&r), "the waiting tone sounds") == 0 && count_in(log_text(&r), "a call is waiting") == 0);
	assert(status_of(&r).machine == PHONE_MACHINE_ANSWERED && status_of(&r).line == PHONE_LINE_CALL && status_of(&r).leg_count == 2);
	assert(!strstr(log_text(&r), "5552"));
	rig_close(&r);
}

/* one or two call lists without an answer are a busy modem, not a lost port; the third in a row is */
static void test_modem_timeouts(void)
{
	struct rig r;
	unsigned int k;

	rig_init(&r);
	rig_open(&r);
	r.listed = CALL_ACTIVE;
	handset(&r, 1);
	run_ms(&r, 2500);
	assert(status_of(&r).line == PHONE_LINE_CALL && status_of(&r).modem_up);
	r.fm.state_timeout = 1;
	for (k = 0; k < 2; k++) {
		unsigned int before = r.fm.begins;

		while (r.fm.begins == before)
			run_ms(&r, HOOK_POLL_MS);
		run_ms(&r, 100);
	}
	assert(status_of(&r).modem_up && status_of(&r).line == PHONE_LINE_CALL);	/* the call goes on */
	r.fm.state_timeout = 0;
	run_ms(&r, 1500);				/* an answer ends the count */
	assert(status_of(&r).modem_up);
	r.fm.state_timeout = 1;
	run_ms(&r, 8000);
	assert(!status_of(&r).modem_up);		/* three in a row: the daemon closes the port and looks again */
	rig_close(&r);
}

/* the idle questions to the modem: the answers reach the status, nothing is asked in a call, and an answered call says under which radio conditions */
static unsigned int asked[8];
static const char *cereg_reply = "+CEREG: 0,1\r\n\r\nOK\r\n";
static const char *qdai_reply = "+QDAI: x,1,0,4,0,0,1,1\r\n\r\nOK\r\n";	/* as captured on the router: the second field 1 = slave */
static const char *clvl_range_reply = "+CLVL: (0-5)\r\n\r\nOK\r\n";

static const char *modem_text(void *user, const char *cmd)
{
	(void)user;
	if (!strcmp(cmd, "AT+QDAI?")) return qdai_reply;
	if (!strcmp(cmd, "AT+CLVL=?")) return clvl_range_reply;
	if (!strcmp(cmd, "AT+CPIN?")) { asked[0]++; return "AT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n"; }
	if (!strcmp(cmd, "AT+CEREG?")) { asked[1]++; return cereg_reply; }
	if (!strncmp(cmd, "AT+QCFG", 7)) { asked[2]++; return "+QCFG: \"ims\",0,1\r\n\r\nOK\r\n"; }
	if (!strcmp(cmd, "AT+QCSQ")) { asked[3]++; return "+QCSQ: \"LTE\",-62,-93,8,-11\r\n\r\nOK\r\n"; }
	if (!strcmp(cmd, "AT+CLIR?")) return "AT+CLIR?\r\r\n+CLIR: 1,4\r\n\r\nOK\r\n";
	if (!strcmp(cmd, "AT+QNWINFO")) { asked[4]++; return "+QNWINFO: \"FDD LTE\",\"41302\",\"LTE BAND 3\",1725\r\n\r\nOK\r\n"; }
	return "";
}

static void test_idle_questions(void)
{
	struct rig r;
	struct phone_status st;
	unsigned int before, k;

	memset(asked, 0, sizeof(asked));
	rig_init(&r);
	r.ops.body = fm_body;
	r.fm.text = modem_text;
	rig_open(&r);
	st = status_of(&r);
	assert(st.sim_ready == PHONE_UNKNOWN && st.registered == PHONE_UNKNOWN && st.ims_registered == PHONE_UNKNOWN &&
	       st.signal_dbm == PHONE_DBM_UNKNOWN && !st.network[0]);		/* nothing is known before the first answers */
	run_ms(&r, 12000);							/* five questions, one every 2 s */
	st = status_of(&r);
	assert(st.sim_ready == 1 && st.registered == 1 && st.ims_registered == 1 && st.signal_dbm == -93 && !strcmp(st.network, "4G"));
	for (k = 0; k < 5; k++)
		assert(asked[k] >= 1);

	/* none while the handset is up or a call is going on */
	for (before = 0, k = 0; k < 5; k++)
		before += asked[k];
	r.listed = CALL_ACTIVE;
	handset(&r, 1);
	run_ms(&r, 10000);
	for (k = 0; k < 5; k++)
		before -= asked[k];
	assert(before == 0);
	assert(count_in(log_text(&r), "call answered on 4G LTE BAND 3, signal rsrp -93 dBm, ims registered") == 1);
	rig_close(&r);

	/* a modem that answers ERROR to a question keeps the old value and the others go on */
	rig_init(&r);
	r.ops.body = fm_body;
	r.fm.text = modem_text;
	rig_open(&r);
	run_ms(&r, 12000);
	r.fm.text = NULL;							/* empty replies: no verdict, the old values stay */
	run_ms(&r, 12000);
	assert(status_of(&r).signal_dbm == -93 && status_of(&r).registered == 1);
	rig_close(&r);

	/* without body() the line asks nothing */
	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 6000);
	assert(status_of(&r).signal_dbm == PHONE_DBM_UNKNOWN && r.fm.begins <= 12);	/* the four setup commands and the call list, no questions */
	rig_close(&r);
}

/* ---- step 4 ---- */

static void with_questions(struct rig *r)
{
	r->ops.body = fm_body;
	r->fm.text = modem_text;
}

/* 1. a key pressed in a call goes to the network as AT+VTS (3GPP TS 27.007), in order, and a key that finds the queue full is dropped with one line and no digit */
static void test_dtmf_in_call(void)
{
	struct rig r;
	unsigned int k;

	rig_init(&r);
	r.m.dtmf_events[0].at = 3000; r.m.dtmf_events[0].code = 0x05;	/* '5' */
	r.m.dtmf_events[1].at = 3300; r.m.dtmf_events[1].code = 0x0c;	/* '#' */
	r.m.dtmf_events[2].at = 3600; r.m.dtmf_events[2].code = 0x0b;	/* '*' */
	r.m.dtmf_event_count = 3;
	rig_open(&r);
	r.listed = CALL_ACTIVE;
	handset(&r, 1);
	run_ms(&r, 2500);
	assert(status_of(&r).line == PHONE_LINE_CALL);
	run_ms(&r, 2500);
	assert(acts_of(&r, "AT+VTS=5") == 1 && acts_of(&r, "AT+VTS=#") == 1 && acts_of(&r, "AT+VTS=*") == 1);
	for (k = 0; k < r.act_count && k < ACTS_MAX; k++)		/* in the order pressed */
		if (!strcmp(r.acts[k], "AT+VTS=#"))
			break;
	assert(k > 0 && !strcmp(r.acts[k - 1], "AT+VTS=5"));
	assert(!strstr(log_text(&r), "dtmf 5") && !strstr(log_text(&r), "VTS=5"));	/* never a digit in the log */
	assert(status_of(&r).line == PHONE_LINE_CALL);
	rig_close(&r);

	/* a modem too slow for the keys: the queue holds 8, the rest are dropped, said once */
	rig_init(&r);
	for (k = 0; k < 14; k++) {
		r.m.dtmf_events[k].at = 3000 + 40 * k;
		r.m.dtmf_events[k].code = 0x01 + (k % 9);
	}
	r.m.dtmf_event_count = 14;
	rig_open(&r);
	r.listed = CALL_ACTIVE;
	handset(&r, 1);
	run_ms(&r, 2500);
	assert(status_of(&r).line == PHONE_LINE_CALL);
	r.fm.latency = 400;						/* then a modem that takes two seconds over a command */
	run_ms(&r, 12000);
	assert(acts_of(&r, "AT+VTS=1") >= 1);
	assert(count_in(log_text(&r), "a key was dropped") == 1);
	assert(!strstr(log_text(&r), "VTS"));				/* the line says a key was lost, never which */
	rig_close(&r);

	/* keys before the call (dialling) are digits, not VTS */
	rig_init(&r);
	r.m.dtmf_events[0].at = 300; r.m.dtmf_events[0].code = 0x04;
	r.m.dtmf_event_count = 1;
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 800);
	assert(acts_of(&r, "AT+VTS=4") == 0);
	rig_close(&r);
}

/* the call log's records: one per call, with the number, the times and the result (the number is for the log only, not for the text log or an event) */
static void dial_45(struct rig *r)
{
	r->m.dtmf_events[0].at = 600; r->m.dtmf_events[0].code = 0x04;
	r->m.dtmf_events[1].at = 900; r->m.dtmf_events[1].code = 0x05;
	r->m.dtmf_events[2].at = 1200; r->m.dtmf_events[2].code = 0x0c;
	r->m.dtmf_event_count = 3;
}

static void test_call_records(void)
{
	struct rig r;

	/* an incoming call, answered at the lift, ended by the remote */
	rig_init(&r);
	rig_open(&r);
	r.listed = CALL_INCOMING;
	run_ms(&r, 2500);
	assert(r.rec_count == 0);
	handset(&r, 1);
	r.listed = CALL_ACTIVE;
	run_ms(&r, 3000);
	r.listed = CALL_GONE;
	run_ms(&r, 1200);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(r.rec_count == 1);
	assert(r.rec[0].dir == PHONE_DIR_IN && !strcmp(r.rec[0].number, "5551") && r.rec[0].result == PHONE_RES_ANSWERED);
	assert(r.rec[0].answered_ms > r.rec[0].start_ms && r.rec[0].end_ms > r.rec[0].answered_ms && r.rec[0].id == 1);
	assert(!strstr(log_text(&r), "5551"));
	rig_close(&r);

	/* an incoming call nobody answers: missed */
	rig_init(&r);
	rig_open(&r);
	r.listed = CALL_INCOMING;
	run_ms(&r, 3000);
	r.listed = CALL_GONE;
	run_ms(&r, 1200);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(r.rec_count == 1 && r.rec[0].dir == PHONE_DIR_IN && r.rec[0].result == PHONE_RES_MISSED && r.rec[0].answered_ms == 0);
	rig_close(&r);

	/* an incoming call while the handset is up: rejected */
	rig_init(&r);
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 300);
	r.listed = CALL_INCOMING;
	run_ms(&r, 2500);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(r.rec_count == 1 && r.rec[0].result == PHONE_RES_REJECTED);
	rig_close(&r);

	/* an outgoing call the remote refuses with user busy (cause 17) */
	rig_init(&r);
	dial_45(&r);
	r.fm.cause = cause_busy;
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 1500);
	r.listed = CALL_ALERTING;
	run_ms(&r, 2500);
	r.listed = CALL_GONE;
	run_ms(&r, 1500);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(r.rec_count == 1 && r.rec[0].dir == PHONE_DIR_OUT && !strcmp(r.rec[0].number, "45"));
	assert(r.rec[0].result == PHONE_RES_BUSY && r.rec[0].cause == 17 && r.rec[0].answered_ms == 0);
	assert(!strstr(log_text(&r), "45"));
	rig_close(&r);

	/* an outgoing call that is answered */
	rig_init(&r);
	dial_45(&r);
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 1500);
	r.listed = CALL_ACTIVE;
	run_ms(&r, 3000);
	r.listed = CALL_GONE;
	run_ms(&r, 1200);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(r.rec_count == 1 && r.rec[0].result == PHONE_RES_ANSWERED && r.rec[0].answered_ms && r.rec[0].end_ms - r.rec[0].answered_ms >= 2500);
	rig_close(&r);

	/* a dial the modem refuses never reaches the call list: its record is made at the refusal, once */
	rig_init(&r);
	dial_45(&r);
	r.refuse_atd = 1;
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 3000);
	assert(r.rec_count == 1 && r.rec[0].dir == PHONE_DIR_OUT && r.rec[0].result == PHONE_RES_FAILED);
	handset(&r, 0);
	run_ms(&r, 1500);
	assert(r.rec_count == 1);
	rig_close(&r);

	/* an attempt that never showed in the list is closed when the next call begins; the ids rise */
	rig_init(&r);
	dial_45(&r);
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 1500);
	handset(&r, 0);
	run_ms(&r, 1000);
	r.listed = CALL_INCOMING;
	run_ms(&r, 2500);
	assert(r.rec_count == 1 && r.rec[0].dir == PHONE_DIR_OUT && r.rec[0].result == PHONE_RES_FAILED);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(r.rec_count == 2 && r.rec[1].dir == PHONE_DIR_IN && r.rec[1].id == r.rec[0].id + 1);
	rig_close(&r);
}

static void test_result_mapping(void)
{
	assert(phone_result_of(PHONE_DIR_IN, 1, 0, 0, 0) == PHONE_RES_ANSWERED);
	assert(phone_result_of(PHONE_DIR_IN, 0, 0, 0, 0) == PHONE_RES_MISSED);
	assert(phone_result_of(PHONE_DIR_IN, 0, 1, 0, 0) == PHONE_RES_REJECTED);
	assert(phone_result_of(PHONE_DIR_IN, 0, 1, 1, 0) == PHONE_RES_BLOCKED);
	assert(phone_result_of(PHONE_DIR_IN, 0, 0, 1, 0) == PHONE_RES_BLOCKED);
	assert(phone_result_of(PHONE_DIR_OUT, 0, 0, 1, 0) == PHONE_RES_BLOCKED);
	assert(phone_result_of(PHONE_DIR_OUT, 1, 0, 0, 17) == PHONE_RES_ANSWERED);
	assert(phone_result_of(PHONE_DIR_OUT, 0, 0, 0, 17) == PHONE_RES_BUSY);
	assert(phone_result_of(PHONE_DIR_OUT, 0, 0, 0, 18) == PHONE_RES_NO_ANSWER);
	assert(phone_result_of(PHONE_DIR_OUT, 0, 0, 0, 19) == PHONE_RES_NO_ANSWER);
	assert(phone_result_of(PHONE_DIR_OUT, 0, 0, 0, 0) == PHONE_RES_FAILED);
	assert(phone_result_of(PHONE_DIR_OUT, 0, 0, 0, 34) == PHONE_RES_FAILED);
	assert(!strcmp(phone_result_name(PHONE_RES_NO_ANSWER), "no_answer") && !strcmp(phone_result_name(PHONE_RES_ANSWERED), "answered"));
	assert(!strcmp(phone_result_name((enum phone_result)99), "failed"));
}

/* speed dial: two digits and the send key dial the contact's first number; two digits alone are a number */
static void press(struct rig *r, const char *keys)
{
	unsigned int k;

	for (k = 0; keys[k]; k++) {
		r->m.dtmf_events[k].at = 600 + 300 * k;
		r->m.dtmf_events[k].code = keys[k] == '#' ? 0x0c : keys[k] == '*' ? 0x0b : keys[k] == '0' ? 0x0a : (uint8_t)(keys[k] - '0');
	}
	r->m.dtmf_event_count = k;
}

static const char *dialled(struct rig *r, const char *keys, const struct phonebook *book, const struct number_region *region)
{
	static char got[24];
	unsigned int k;

	rig_init(r);
	press(r, keys);
	r->cfg.book = book;
	r->cfg.region = region;
	rig_open(r);
	handset(r, 1);
	run_ms(r, 9000);
	got[0] = 0;
	for (k = 0; k < r->act_count && k < ACTS_MAX; k++)
		if (!strncmp(r->acts[k], "ATD", 3))
			snprintf(got, sizeof(got), "%s", r->acts[k]);
	assert(!strstr(log_text(r), "0777"));		/* the stored number never reaches the log */
	rig_close(r);
	return got;
}

static void test_speed_dial(void)
{
	struct rig r;
	struct phonebook *pb = calloc(1, sizeof(*pb));
	const char *ann[] = { "0777:mobile", "0999:home" };
	const char *toolong[] = { "123456789012345678901234567890" };
	static const char *const own[] = { "05", NULL };
	const struct number_region special = { "XX", "1", "1", "011", own };

	assert(pb);
	phonebook_clear(pb, NULL);
	assert(phonebook_add(pb, "Ann", ann, 2, "05", NULL) == 0);
	assert(phonebook_add(pb, "Long", toolong, 1, "09", NULL) == 0);
	assert(!strcmp(dialled(&r, "05#", pb, NULL), "ATD0777;"));		/* the first number of the contact */
	assert(!strcmp(dialled(&r, "05", pb, NULL), "ATD05;"));			/* without the send key: an ordinary number, after the pause */
	assert(!strcmp(dialled(&r, "06#", pb, NULL), "ATD06;"));		/* no such code: the digits as pressed */
	assert(!strcmp(dialled(&r, "05#", NULL, NULL), "ATD05;"));		/* no phone book */
	assert(!strcmp(dialled(&r, "505#", pb, NULL), "ATD505;"));		/* three digits are not a code */
	assert(!strcmp(dialled(&r, "09#", pb, NULL), "ATD09;"));		/* a stored number too long to dial is not used */
	assert(!strcmp(dialled(&r, "05#", pb, &special), "ATD05;"));		/* an emergency number as dialled is never replaced */
	assert(!strcmp(dialled(&r, "112#", pb, NULL), "ATD112;"));
	free(pb);
}

/* call blocking: a blocked caller never rings, a blocked number is not dialled, emergency numbers pass */
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

static void test_blocking_calls(void)
{
	struct rig r;
	struct blocking *b = rules("5551", "45", 0);

	/* a listed caller: released at the first list that shows the call, no ring, no answer, a record that says blocked */
	rig_init(&r);
	r.cfg.block = b;
	rig_open(&r);
	r.listed = CALL_INCOMING;
	run_ms(&r, 3000);
	assert(count_in(log_text(&r), "ring started") == 0 && status_of(&r).line == PHONE_LINE_IDLE);
	assert(acts_of(&r, "AT+CHUP") >= 1 && acts_of(&r, "ATA") == 0);
	assert(count_in(log_text(&r), "an incoming call was blocked") == 1 && !strstr(log_text(&r), "5551"));
	handset(&r, 1);					/* lifting during the blocked call does not answer it */
	run_ms(&r, 1000);
	assert(acts_of(&r, "ATA") == 0);
	handset(&r, 0);					/* ... and putting it down does not offer it: the network keeps it alive */
	run_ms(&r, 3000);
	assert(count_in(log_text(&r), "ring started") == 0);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(r.rec_count == 1 && r.rec[0].dir == PHONE_DIR_IN && r.rec[0].result == PHONE_RES_BLOCKED && !strcmp(r.rec[0].number, "5551"));
	rig_close(&r);

	/* another caller rings as before */
	the_caller = "5552";
	rig_init(&r);
	r.cfg.block = b;
	rig_open(&r);
	r.listed = CALL_INCOMING;
	run_ms(&r, 2500);
	assert(count_in(log_text(&r), "ring started") == 1 && acts_of(&r, "AT+CHUP") == 0);
	rig_close(&r);

	/* a withheld number: refused with the switch on, rings with it off */
	the_caller = "";
	rig_init(&r);
	r.cfg.block = b;
	rig_open(&r);
	r.listed = CALL_INCOMING;
	run_ms(&r, 2500);
	assert(count_in(log_text(&r), "ring started") == 1);
	rig_close(&r);
	b->anonymous = 1;
	rig_init(&r);
	r.cfg.block = b;
	rig_open(&r);
	r.listed = CALL_INCOMING;
	run_ms(&r, 2500);
	assert(count_in(log_text(&r), "ring started") == 0 && acts_of(&r, "AT+CHUP") >= 1);
	rig_close(&r);

	/* an emergency number calling in is never blocked */
	the_caller = "112";
	assert(blocking_add(b, BLOCK_IN, "112", NULL) == 0);
	rig_init(&r);
	r.cfg.block = b;
	rig_open(&r);
	r.listed = CALL_INCOMING;
	run_ms(&r, 2500);
	assert(count_in(log_text(&r), "ring started") == 1);
	rig_close(&r);
	the_caller = "5551";
	free(b);

	/* a blocked number is not dialled: the congestion tone, a record, nothing for the modem */
	b = rules(NULL, "45", 0);
	rig_init(&r);
	press(&r, "45#");
	r.cfg.block = b;
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 3000);
	assert(acts_of(&r, "ATD45;") == 0 && status_of(&r).line == PHONE_LINE_RELEASE && status_of(&r).call_id == 0);
	assert(r.rec_count == 1 && r.rec[0].dir == PHONE_DIR_OUT && r.rec[0].result == PHONE_RES_BLOCKED && !strcmp(r.rec[0].number, "45"));
	assert(count_in(log_text(&r), "a blocked number was dialled") == 1 && !strstr(log_text(&r), "45"));
	handset(&r, 0);
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_IDLE && r.rec_count == 1);
	rig_close(&r);
	free(b);

	/* an emergency number is dialled even when a rule would block it */
	b = rules(NULL, "112", 0);
	assert(blocking_add(b, BLOCK_OUT_PREFIX, "1", NULL) == 0);
	rig_init(&r);
	press(&r, "112#");
	r.cfg.block = b;
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 3000);
	assert(acts_of(&r, "ATD112;") == 1 && r.rec_count == 0);
	rig_close(&r);
	free(b);
}

/* CLIR: written at every modem setup and at a change, read back by the idle questions */
static void test_clir(void)
{
	struct rig r;
	struct phone_cfg c2;
	struct phone_status st;

	rig_init(&r);
	r.ops.body = fm_body;
	r.fm.text = modem_text;
	r.cfg.clir_set = 1;
	r.cfg.clir = 1;
	rig_open(&r);
	run_ms(&r, 3000);
	assert(acts_of(&r, "AT+CLIR=1") == 1);
	st = status_of(&r);
	assert(st.clir == PHONE_UNKNOWN && st.clir_status == PHONE_UNKNOWN);	/* nothing read back yet */
	run_ms(&r, 20000);
	st = status_of(&r);
	assert(st.clir == 1 && st.clir_status == 4);				/* what the modem answered, not what we wrote */
	/* a changed setting is written at once when the line is idle; the same setting is not written again */
	c2 = r.cfg;
	assert(phone_set_cfg(r.p, &c2) == 0);
	run_ms(&r, 500);
	assert(acts_of(&r, "AT+CLIR=1") == 1);
	c2.clir = 2;
	assert(phone_set_cfg(r.p, &c2) == 0);
	run_ms(&r, 500);
	assert(acts_of(&r, "AT+CLIR=2") == 1);
	phone_modem_up(r.p);							/* a modem that came back gets the setting again */
	run_ms(&r, 500);
	assert(acts_of(&r, "AT+CLIR=2") == 2);
	rig_close(&r);

	/* off: nothing is written */
	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 3000);
	assert(acts_of(&r, "AT+CLIR=0") == 0 && acts_of(&r, "AT+CLIR=1") == 0);
	rig_close(&r);
}

/* the per-call caller-id codes are typed as a service code and reach the modem unchanged; the number behind them is what blocking sees */
static void test_service_codes(void)
{
	struct rig r;
	struct blocking *b = rules(NULL, "45", 0);

	assert(!strcmp(dialled(&r, "#3145", NULL, NULL), "ATD#3145;"));		/* everything after the first # or * belongs to the code */
	assert(!strcmp(dialled(&r, "#31#45", NULL, NULL), "ATD#31#45;"));
	assert(!strcmp(dialled(&r, "*31#45", NULL, NULL), "ATD*31#45;"));
	assert(!strcmp(dialled(&r, "*#21#", NULL, NULL), "ATD*#21#;"));		/* a service code alone */
	assert(!strcmp(dialled(&r, "45#", NULL, NULL), "ATD45;"));		/* the send key still works for a number */
	assert(!strcmp(dialled(&r, "4*5#", NULL, NULL), "ATD45;"));		/* a star inside a number is dropped as before */
	assert(!strcmp(dialled(&r, "#", NULL, NULL), ""));			/* a lone # dials nothing: it starts a service code */
	/* blocking looks behind the code */
	rig_init(&r);
	press(&r, "#31#45");
	r.cfg.block = b;
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 9000);
	assert(acts_of(&r, "ATD#31#45;") == 0 && r.rec_count == 1 && r.rec[0].result == PHONE_RES_BLOCKED);
	rig_close(&r);
	free(b);
}

/* the hotline: nothing dialled for the delay dials the stored number; any key ends the wait; no hotline, no dial */
static unsigned int hotline_dials(const char *keys, unsigned int delay_ms, const char *number, unsigned int wait_ms)
{
	struct rig r;
	unsigned int n;

	rig_init(&r);
	if (keys)
		press(&r, keys);
	snprintf(r.cfg.hotline, sizeof(r.cfg.hotline), "%s", number);
	r.cfg.hotline_delay_ms = delay_ms;
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, wait_ms);
	n = acts_of(&r, "ATD777;");
	assert(!strstr(log_text(&r), "777"));
	rig_close(&r);
	return n;
}

static void test_hotline(void)
{
	assert(hotline_dials(NULL, 3000, "777", 2500) == 0);		/* the delay has not passed */
	assert(hotline_dials(NULL, 3000, "777", 4500) == 1);		/* it has: the stored number goes out, once */
	assert(hotline_dials(NULL, 3000, "", 9000) == 0);		/* no hotline set */
	assert(hotline_dials("4", 3000, "777", 9000) == 0);		/* a key before the delay ends the wait: the caller's own digits go out */
	assert(hotline_dials("4", 3000, "777", 9000) == 0);
	/* the caller's own number goes out, not the hotline */
	{
		struct rig r;

		rig_init(&r);
		press(&r, "45#");
		snprintf(r.cfg.hotline, sizeof(r.cfg.hotline), "777");
		r.cfg.hotline_delay_ms = 3000;
		rig_open(&r);
		handset(&r, 1);
		run_ms(&r, 9000);
		assert(acts_of(&r, "ATD45;") == 1 && acts_of(&r, "ATD777;") == 0);
		rig_close(&r);
	}
	/* an emergency number typed within the delay is dialled as typed */
	{
		struct rig r;

		rig_init(&r);
		press(&r, "112#");
		snprintf(r.cfg.hotline, sizeof(r.cfg.hotline), "777");
		r.cfg.hotline_delay_ms = 3000;
		rig_open(&r);
		handset(&r, 1);
		run_ms(&r, 9000);
		assert(acts_of(&r, "ATD112;") == 1 && acts_of(&r, "ATD777;") == 0);
		rig_close(&r);
	}
	/* a hang-up and a new lift start the wait again */
	{
		struct rig r;

		rig_init(&r);
		snprintf(r.cfg.hotline, sizeof(r.cfg.hotline), "777");
		r.cfg.hotline_delay_ms = 3000;
		rig_open(&r);
		handset(&r, 1);
		run_ms(&r, 2000);
		handset(&r, 0);
		run_ms(&r, 1500);
		handset(&r, 1);
		run_ms(&r, 2000);
		assert(acts_of(&r, "ATD777;") == 0);
		run_ms(&r, 2500);
		assert(acts_of(&r, "ATD777;") == 1);
		rig_close(&r);
	}
}

/* the line test: three samples, a verdict; only on an idle line with the handset down */
static void test_line_test(void)
{
	struct rig r;
	struct phone_line_test_result res;
	unsigned int k;

	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 300);
	assert(phone_line_test_poll(r.p, &res) == 0);			/* none started */
	assert(phone_line_test(r.p) == 0 && phone_line_test(r.p) == -EBUSY);
	assert(phone_line_test_poll(r.p, &res) == 0);			/* running */
	run_ms(&r, 500);
	assert(phone_line_test_poll(r.p, &res) == 1);
	assert(res.verdict == PHONE_LT_OK && !res.hook_off && res.battery_mv == 59000 && res.expected_mv == TEST_HIGH_MV);
	assert(res.loop_ua == 20000 && res.leak_suspect);		/* the fake line's loop current is high: reported, the verdict is not leak (threshold TO-VERIFY) */
	assert(phone_line_test_poll(r.p, &res) == 0);			/* handed over once */
	assert(count_in(log_text(&r), "line test: ok") == 1);
	assert(phone_line_test(r.p) == 0);				/* a new test can follow */
	run_ms(&r, 500);
	assert(phone_line_test_poll(r.p, &res) == 1);
	rig_close(&r);

	/* refused with the handset up, while ringing, and in a call */
	rig_init(&r);
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 300);
	assert(phone_line_test(r.p) == -EBUSY);
	handset(&r, 0);
	run_ms(&r, 1500);
	r.listed = CALL_INCOMING;
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_RINGING && phone_line_test(r.p) == -EBUSY);
	rig_close(&r);

	/* the handset is lifted during the test */
	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 300);
	assert(phone_line_test(r.p) == 0);
	run_ms(&r, 100);
	handset(&r, 1);
	run_ms(&r, 500);
	assert(phone_line_test_poll(r.p, &res) == 1 && res.verdict == PHONE_LT_OFF_HOOK && res.hook_off);
	rig_close(&r);

	/* the phone rings during the test (a ring test stands for a call here: the call list is polled once a second) */
	rig_init(&r);
	rig_open(&r);
	run_ms(&r, 300);
	assert(phone_line_test(r.p) == 0);
	run_ms(&r, 5);
	assert(phone_ring_test(r.p, 1) == 0);
	run_ms(&r, 1200);
	assert(phone_line_test_poll(r.p, &res) == 1 && res.verdict == PHONE_LT_INTERRUPTED);
	rig_close(&r);

	/* no battery */
	rig_init(&r);
	for (k = 0; k < 5; k++)
		r.m.vbat_mv[k] = 8000;
	rig_open(&r);
	run_ms(&r, 300);
	assert(phone_line_test(r.p) == 0);
	run_ms(&r, 500);
	assert(phone_line_test_poll(r.p, &res) == 1 && res.verdict == PHONE_LT_NO_BATTERY && res.battery_mv == 8000);
	rig_close(&r);

	assert(!strcmp(phone_line_verdict_name(PHONE_LT_LEAK), "leak") && !strcmp(phone_line_verdict_name(PHONE_LT_NO_BATTERY), "no_battery"));
	assert(!strcmp(phone_line_verdict_name((enum phone_line_verdict)42), "error"));
}

/* alarm calls: the phone rings at the set minute and weekday, at most a minute, without caller id; a busy line or an untrusted clock skips it */
static void set_clock(struct rig *r, int wday, int hour, int min)
{
	memset(&r->clock, 0, sizeof(r->clock));
	r->clock.tm_year = 126;
	r->clock.tm_yday = 40;
	r->clock.tm_wday = wday;
	r->clock.tm_hour = hour;
	r->clock.tm_min = min;
	r->clock_ok = 1;
}

static struct alarms *alarm_set(const char *time, const char *day, const char *enabled)
{
	struct alarms *a = calloc(1, sizeof(*a));
	const char *days[] = { day };

	assert(a);
	assert(alarms_add(a, "Wake", time, days, 1, enabled, NULL) == 0);
	return a;
}

static void test_alarms(void)
{
	struct rig r;
	struct alarms *a = alarm_set("06:30", "1", "1");

	/* before the minute: quiet; in the minute: rings, with no caller id, once */
	rig_init(&r);
	r.ops.cid = CID_DTMF;
	r.cfg.alarms = a;
	set_clock(&r, 1, 6, 29);
	rig_open(&r);
	run_ms(&r, 35000);
	assert(count_in(log_text(&r), "ring started") == 0 && status_of(&r).line == PHONE_LINE_IDLE);
	set_clock(&r, 1, 6, 30);
	run_ms(&r, 31000);
	assert(count_in(log_text(&r), "alarm: the phone rings") == 1 && status_of(&r).line == PHONE_LINE_RINGING && status_of(&r).call_id == 0);
	run_ms(&r, 20000);
	assert(count_in(log_text(&r), "caller id") == 0);			/* an alarm has no caller */
	assert(!strstr(log_text(&r), "Wake"));
	run_ms(&r, 40000);						/* more than a minute after the start */
	assert(status_of(&r).line == PHONE_LINE_IDLE && count_in(log_text(&r), "ring stopped (the test time is over)") == 1);
	run_ms(&r, 60000);						/* the clock still shows that minute: no second ring */
	assert(count_in(log_text(&r), "alarm: the phone rings") == 1);
	set_clock(&r, 1, 6, 31);
	run_ms(&r, 35000);
	set_clock(&r, 2, 6, 30);					/* Tuesday: not a chosen day */
	run_ms(&r, 35000);
	assert(count_in(log_text(&r), "alarm: the phone rings") == 1);
	rig_close(&r);

	/* a lift stops the ring */
	rig_init(&r);
	r.cfg.alarms = a;
	set_clock(&r, 1, 6, 30);
	rig_open(&r);
	run_ms(&r, 3000);
	assert(status_of(&r).line == PHONE_LINE_RINGING);
	handset(&r, 1);
	run_ms(&r, 500);
	assert(status_of(&r).line != PHONE_LINE_RINGING && count_in(log_text(&r), "ring stopped (the handset was lifted)") == 1);
	rig_close(&r);

	/* a busy line: skipped, said once, not rung when the handset goes down in the same minute */
	rig_init(&r);
	r.cfg.alarms = a;
	set_clock(&r, 1, 6, 30);
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 3000);
	assert(count_in(log_text(&r), "alarm missed: line busy") == 1);
	handset(&r, 0);
	run_ms(&r, 40000);
	assert(count_in(log_text(&r), "alarm: the phone rings") == 0 && count_in(log_text(&r), "alarm missed") == 1);
	rig_close(&r);

	/* a clock that is not trusted: no ring, one line per stretch; it rings once the clock is good */
	rig_init(&r);
	r.cfg.alarms = a;
	set_clock(&r, 1, 6, 30);
	r.clock_ok = 0;
	rig_open(&r);
	run_ms(&r, 100000);
	assert(count_in(log_text(&r), "the clock is not set") == 1 && count_in(log_text(&r), "ring started") == 0);
	r.clock_ok = 1;
	run_ms(&r, 31000);
	assert(count_in(log_text(&r), "alarm: the phone rings") == 1);
	rig_close(&r);

	/* a disabled alarm never rings; without alarms or a clock hook nothing is asked */
	free(a);
	a = alarm_set("06:30", "1", "0");
	rig_init(&r);
	r.cfg.alarms = a;
	set_clock(&r, 1, 6, 30);
	rig_open(&r);
	run_ms(&r, 70000);
	assert(count_in(log_text(&r), "ring started") == 0);
	rig_close(&r);
	rig_init(&r);
	set_clock(&r, 1, 6, 30);
	rig_open(&r);
	run_ms(&r, 70000);
	assert(count_in(log_text(&r), "ring started") == 0);
	rig_close(&r);
	free(a);
}

/* call forwarding: erase, register, read back; only with an idle line; a refusal is kept as the reason */
static const char *fwd_reply = "AT+CCFC=2,2\r\r\n+CCFC: 1,1,\"+9412345\",145,,,20\r\n\r\nOK\r\n";
static char fwd_asked[24];

static const char *fwd_text(void *user, const char *cmd)
{
	(void)user;
	if (strncmp(cmd, "AT+CCFC=", 8))
		return "";
	snprintf(fwd_asked, sizeof(fwd_asked), "%s", cmd);
	return fwd_reply;
}

static struct phone_forward fwd_cfg(int enabled, const char *to, enum fwd_when when, unsigned int delay)
{
	struct phone_forward f;

	memset(&f, 0, sizeof(f));
	f.configured = 1;
	f.enabled = enabled;
	snprintf(f.to, sizeof(f.to), "%s", to);
	f.when = when;
	f.delay_s = delay;
	return f;
}

static unsigned int ccfc_index(const struct rig *r, const char *cmd)
{
	unsigned int k;

	for (k = 0; k < r->act_count && k < ACTS_MAX; k++)
		if (!strcmp(r->acts[k], cmd))
			return k + 1;
	return 0;
}

static void test_forwarding(void)
{
	struct rig r;
	struct phone_status st;
	struct phone_cfg c2;

	/* settings present at the start: erase everything, register, query the reason; the answer is what the page shows */
	fwd_asked[0] = 0;
	fwd_reply = "AT+CCFC=2,2\r\r\n+CCFC: 1,1,\"+9412345\",145,,,20\r\n\r\nOK\r\n";
	rig_init(&r);
	r.ops.body = fm_body;
	r.fm.text = fwd_text;
	r.cfg.forward = fwd_cfg(1, "012345", FWD_NOANSWER, 25);
	rig_open(&r);
	st = status_of(&r);
	assert(st.fwd.network == PHONE_UNKNOWN && !st.fwd.applying && !st.fwd.done);	/* nothing asked yet */
	run_ms(&r, 3000);
	assert(ccfc_index(&r, "AT+CCFC=4,4") && ccfc_index(&r, "AT+CCFC=2,3,\"012345\",129,,,25") > ccfc_index(&r, "AT+CCFC=4,4"));
	assert(!strcmp(fwd_asked, "AT+CCFC=2,2"));
	st = status_of(&r);
	assert(st.fwd.done && !st.fwd.applying && st.fwd.network == 1 && !strcmp(st.fwd.number, "+9412345") && st.fwd.delay_s == 20 && !st.fwd.error[0]);
	assert(!strstr(log_text(&r), "012345") && !strstr(log_text(&r), "9412345"));	/* a number never reaches the log */
	/* the same settings again: nothing is sent; changed settings: a new exchange */
	c2 = r.cfg;
	r.act_count = 0;
	assert(phone_set_cfg(r.p, &c2) == 0);
	run_ms(&r, 2000);
	assert(acts_of(&r, "AT+CCFC=4,4") == 0);
	c2.forward.when = FWD_BUSY;
	assert(phone_set_cfg(r.p, &c2) == 0);
	run_ms(&r, 3000);
	assert(acts_of(&r, "AT+CCFC=4,4") == 1 && ccfc_index(&r, "AT+CCFC=1,3,\"012345\",129") && !strcmp(fwd_asked, "AT+CCFC=1,2"));
	rig_close(&r);

	/* disabled: erased, and the read-back says off */
	fwd_reply = "+CCFC: 0,1\r\n\r\nOK\r\n";
	rig_init(&r);
	r.ops.body = fm_body;
	r.fm.text = fwd_text;
	r.cfg.forward = fwd_cfg(0, "", FWD_ALWAYS, 20);
	rig_open(&r);
	run_ms(&r, 3000);
	st = status_of(&r);
	assert(ccfc_index(&r, "AT+CCFC=4,4") && st.fwd.done && st.fwd.network == 0 && !st.fwd.number[0] && !st.fwd.error[0]);
	assert(!ccfc_index(&r, "AT+CCFC=0,3,\"\",129"));
	rig_close(&r);

	/* no settings: the network is never touched */
	rig_init(&r);
	r.ops.body = fm_body;
	r.fm.text = fwd_text;
	rig_open(&r);
	run_ms(&r, 5000);
	assert(!ccfc_index(&r, "AT+CCFC=4,4") && !status_of(&r).fwd.done);
	rig_close(&r);

	/* the network refuses the registration: the reason is kept, nothing is retried, the read-back still comes */
	fwd_reply = "+CCFC: 0,1\r\n\r\nOK\r\n";
	rig_init(&r);
	r.ops.body = fm_body;
	r.fm.text = fwd_text;
	r.refuse_ccfc = 1;
	r.cfg.forward = fwd_cfg(1, "012345", FWD_NOANSWER, 20);
	rig_open(&r);
	run_ms(&r, 20000);
	st = status_of(&r);
	assert(acts_of(&r, "AT+CCFC=2,3,\"012345\",129,,,20") == 1 && st.fwd.done && st.fwd.error[0] && st.fwd.network == 0);
	assert(!strstr(log_text(&r), "012345"));
	rig_close(&r);

	/* a reply that says nothing usable: unknown */
	fwd_reply = "OK\r\n";
	rig_init(&r);
	r.ops.body = fm_body;
	r.fm.text = fwd_text;
	r.cfg.forward = fwd_cfg(1, "+9412345", FWD_ALWAYS, 20);
	rig_open(&r);
	run_ms(&r, 3000);
	assert(status_of(&r).fwd.done && status_of(&r).fwd.network == PHONE_UNKNOWN);
	rig_close(&r);

	/* a busy line waits: the exchange starts when the handset is down */
	fwd_reply = "+CCFC: 1,1,\"+9412345\",145\r\n";
	rig_init(&r);
	r.ops.body = fm_body;
	r.fm.text = fwd_text;
	r.cfg.forward = fwd_cfg(1, "+9412345", FWD_ALWAYS, 20);
	rig_open(&r);
	handset(&r, 1);
	run_ms(&r, 4000);
	assert(!ccfc_index(&r, "AT+CCFC=4,4"));
	handset(&r, 0);
	run_ms(&r, 4000);
	assert(ccfc_index(&r, "AT+CCFC=4,4") && status_of(&r).fwd.network == 1);
	rig_close(&r);

	/* the modem is lost in the middle: the reason is kept, one more try when it is back */
	rig_init(&r);
	r.ops.body = fm_body;
	r.fm.text = fwd_text;
	r.fm.latency = 40;						/* slow enough to be caught open */
	r.cfg.forward = fwd_cfg(1, "+9412345", FWD_ALWAYS, 20);
	rig_open(&r);
	run_ms(&r, 900);						/* the four setup commands, then the erase is on its way */
	assert(status_of(&r).fwd.applying);
	r.fm.lost = 1;
	run_ms(&r, 1500);
	st = status_of(&r);
	assert(!st.fwd.applying && !strcmp(st.fwd.error, "the modem was lost") && !st.modem_up);
	r.fm.lost = 0;
	r.fm.open = 0;
	r.fm.latency = 0;
	r.act_count = 0;
	phone_modem_up(r.p);
	run_ms(&r, 4000);
	st = status_of(&r);
	assert(acts_of(&r, "AT+CCFC=4,4") == 1 && st.fwd.done && st.fwd.network == 1 && !st.fwd.error[0]);
	rig_close(&r);
}

/* the legs of the call list: every voice call line is read, each leg keeps its own id for as long as it is listed */
static void test_legs(void)
{
	struct call_leg legs[CALL_LEGS_MAX + 1];
	struct rig r;
	struct phone_status st;
	unsigned int first;

	/* the parser: every voice line, the stat values, the number's digits, other modes skipped, a cut line skipped */
	assert(answer_legs("+CLCC: 1,1,0,0,0,\"5551\",129\r\n+CLCC: 2,1,5,0,0,\"5552\",129\r\nOK", legs, CALL_LEGS_MAX) == 2);
	assert(legs[0].idx == 1 && legs[0].stat == LEG_ACTIVE && legs[0].dir == 1 && !strcmp(legs[0].number, "5551") && !legs[0].mpty);
	assert(legs[1].idx == 2 && legs[1].stat == LEG_WAITING && !strcmp(legs[1].number, "5552"));
	assert(answer_legs("+CLCC: 1,0,1,0,0,\"+94 77\",145\r\n+CLCC: 2,0,0,0,1,\"\",128\r\n+CLCC: 3,0,0,0,1,\"7\",128\r\n", legs, CALL_LEGS_MAX) == 3);
	assert(legs[0].stat == LEG_HELD && !strcmp(legs[0].number, "+9477") && legs[1].mpty == 1 && !legs[1].number[0]);
	/* the international type without the plus gets one; the national type keeps its digits */
	assert(answer_legs("+CLCC: 1,1,0,0,0,\"9477123\",145\r\n+CLCC: 2,1,5,0,0,\"771234567\",129\r\nOK", legs, CALL_LEGS_MAX) == 2);
	assert(!strcmp(legs[0].number, "+9477123") && !strcmp(legs[1].number, "771234567"));
	assert(answer_legs("+CLCC: 1,0,0,1,0,\"1\",128\r\nOK", legs, CALL_LEGS_MAX) == 0);		/* a data call */
	assert(answer_legs("+CLCC: 1,0,0\r\n+CLCC: 2,0,9,0,0,\"1\",128\r\n", legs, CALL_LEGS_MAX) == 0);	/* cut short, or a stat that does not exist */
	assert(answer_legs("OK\r\n", legs, CALL_LEGS_MAX) == 0 && answer_legs("", legs, CALL_LEGS_MAX) == 0);
	assert(answer_legs("+CLCC: 1,0,0,0,0,\"1\",128\r\n+CLCC: 2,0,1,0,0,\"2\",128\r\n+CLCC: 3,0,1,0,0,\"3\",128\r\n+CLCC: 4,0,1,0,0,\"4\",128\r\n", legs, CALL_LEGS_MAX) == CALL_LEGS_MAX);

	/* the core: a leg gets an id when it is first listed and keeps it; the first leg has the line's call id; a gone leg's id is not reused */
	rig_init(&r);
	r.ops.legs = fm_legs;
	rig_open(&r);
	r.listed = CALL_INCOMING;
	r.fm.legs[0] = (struct call_leg){ .idx = 1, .dir = 1, .stat = LEG_INCOMING, .number = "5551" };
	r.fm.nlegs = 1;
	run_ms(&r, 2500);
	handset(&r, 1);
	r.listed = CALL_ACTIVE;
	r.fm.legs[0].stat = LEG_ACTIVE;
	run_ms(&r, 2500);
	st = status_of(&r);
	assert(st.leg_count == 1 && st.legs[0].stat == LEG_ACTIVE && !strcmp(st.legs[0].number, "5551") && st.legs[0].id && st.legs[0].id == st.call_id);
	first = st.legs[0].id;
	r.fm.legs[1] = (struct call_leg){ .idx = 2, .dir = 1, .stat = LEG_WAITING, .number = "5552" };
	r.fm.nlegs = 2;
	run_ms(&r, 2500);
	st = status_of(&r);
	assert(st.leg_count == 2 && st.legs[0].id == first && st.legs[1].id > first && st.legs[1].stat == LEG_WAITING);
	{
		unsigned int second = st.legs[1].id;

		r.fm.legs[0].stat = LEG_HELD;			/* the legs swap places in the list: the ids follow the modem's index */
		r.fm.legs[1].stat = LEG_ACTIVE;
		run_ms(&r, 2500);
		st = status_of(&r);
		assert(st.legs[0].id == first && st.legs[1].id == second && st.legs[0].stat == LEG_HELD);
		r.fm.nlegs = 1;				/* the second leg ends */
		r.fm.legs[0] = r.fm.legs[1];
		r.fm.legs[0].idx = 2;
		run_ms(&r, 2500);
		st = status_of(&r);
		assert(st.leg_count == 1 && st.legs[0].id == second);
		r.fm.legs[0].idx = 3;			/* a new index is a new call, with a new id */
		run_ms(&r, 2500);
		assert(status_of(&r).legs[0].id > second);
	}
	r.fm.nlegs = 0;
	r.listed = CALL_NONE;
	run_ms(&r, 2500);
	assert(status_of(&r).leg_count == 0);
	/* the numbers are in the status reply only: never in the log */
	assert(!strstr(log_text(&r), "5551") && !strstr(log_text(&r), "5552"));
	rig_close(&r);

	/* without the legs hook the line sees no legs and works as before */
	rig_init(&r);
	rig_open(&r);
	r.listed = CALL_ACTIVE;
	handset(&r, 1);
	run_ms(&r, 2500);
	assert(status_of(&r).leg_count == 0 && status_of(&r).line == PHONE_LINE_CALL);
	rig_close(&r);
}

/* call waiting: a second incoming call during a call (stat 5), the tone, the flash keys, the refusals */
static void key_after(struct rig *r, char key, unsigned int ms)
{
	struct { unsigned int at; uint8_t code; } *e = (void *)r->m.dtmf_events;

	e[r->m.dtmf_event_count].at = (unsigned int)(r->m.vtime_ms - r->m.lcr_origin) + ms;
	e[r->m.dtmf_event_count].code = key == '#' ? 0x0c : key == '*' ? 0x0b : key == '0' ? 0x0a : (uint8_t)(key - '0');
	r->m.dtmf_event_count++;
}

static void flash(struct rig *r)
{
	handset(r, 0);
	run_ms(r, 150);			/* between the flash minimum and the hang-up window */
	handset(r, 1);
}

/* an answered call with the legs hook, the handset up */
static int want_conference;

static void waiting_rig(struct rig *r, int call_waiting, const struct blocking *block)
{
	rig_init(r);
	r->ops.legs = fm_legs;
	r->cfg.call_waiting = call_waiting;
	r->cfg.conference = want_conference;
	r->cfg.block = block;
	rig_open(r);
	r->listed = CALL_INCOMING;
	r->fm.legs[0] = (struct call_leg){ .idx = 1, .dir = 1, .stat = LEG_INCOMING, .number = "5551" };
	r->fm.nlegs = 1;
	run_ms(r, 2500);
	handset(r, 1);
	r->listed = CALL_ACTIVE;
	r->fm.legs[0].stat = LEG_ACTIVE;
	run_ms(r, 2500);
	assert(status_of(r).line == PHONE_LINE_CALL);
	r->act_count = 0;
}

static void a_call_waits(struct rig *r)
{
	r->fm.legs[1] = (struct call_leg){ .idx = 2, .dir = 1, .stat = LEG_WAITING, .number = "5552" };
	r->fm.nlegs = 2;
	run_ms(r, 1500);
}

static void test_call_waiting(void)
{
	struct rig r;
	struct blocking *b = rules("5552", NULL, 0);
	unsigned int k;

	/* the state of the line's call from the listed calls */
	{
		struct call_leg l[3] = { { .stat = LEG_HELD }, { .stat = LEG_ACTIVE }, { .stat = LEG_WAITING } };
		unsigned int w = 9;

		assert(answer_state_of_legs(l, 3, &w) == CALL_ACTIVE && w == 1);
		assert(answer_state_of_legs(l, 1, &w) == CALL_HELD && w == 0);
		l[1].stat = LEG_DIALING;
		assert(answer_state_of_legs(l, 3, &w) == CALL_DIALING && w == 1);
		l[1].stat = LEG_ALERTING;
		assert(answer_state_of_legs(l, 3, &w) == CALL_ALERTING);
		l[1].stat = LEG_INCOMING;
		assert(answer_state_of_legs(l, 3, &w) == CALL_INCOMING);
		assert(answer_state_of_legs(&l[2], 1, &w) == CALL_INCOMING && w == 0);	/* a waiting call alone is presented as incoming */
		assert(answer_state_of_legs(l, 0, &w) == CALL_NONE);
	}

	/* the waiting call is offered: the tone sounds, nothing is sent to the modem, the line stays in the call */
	waiting_rig(&r, 1, NULL);
	a_call_waits(&r);
	assert(count_in(log_text(&r), "a call is waiting") == 1 && count_in(log_text(&r), "the waiting tone sounds") == 1);
	assert(acts_of(&r, "AT+CHLD=0") == 0 && acts_of(&r, "AT+CHUP") == 0 && status_of(&r).line == PHONE_LINE_CALL);
	assert(status_of(&r).leg_count == 2 && status_of(&r).legs[1].stat == LEG_WAITING && status_of(&r).legs[1].id > status_of(&r).legs[0].id);
	assert(!strstr(log_text(&r), "5552"));
	/* a key with no flash before it is an ordinary key */
	key_after(&r, '5', 100);
	run_ms(&r, 500);
	assert(acts_of(&r, "AT+VTS=5") == 1);
	/* a flash and nothing after it: the waiting call is taken and the active call held, once the window is over */
	flash(&r);
	run_ms(&r, 2000);
	assert(acts_of(&r, "AT+CHLD=2") == 0);
	run_ms(&r, 1500);
	assert(acts_of(&r, "AT+CHLD=2") == 1 && acts_of(&r, "AT+CHLD=1") == 0);
	rig_close(&r);

	/* flash and 2: the same at once; flash and 1: the active call ends and the waiting one is taken; flash and 0: the waiting call is refused */
	for (k = 0; k < 3; k++) {
		static const char key[3] = { '2', '1', '0' };
		static const char *const cmd[3] = { "AT+CHLD=2", "AT+CHLD=1", "AT+CHLD=0" };

		waiting_rig(&r, 1, NULL);
		a_call_waits(&r);
		flash(&r);
		key_after(&r, key[k], 200);
		run_ms(&r, 1000);
		run_ms(&r, 1000);
		assert(acts_of(&r, cmd[k]) >= 1 && (k == 2 || acts_of(&r, cmd[k]) == 1) && acts_of(&r, "AT+VTS=2") + acts_of(&r, "AT+VTS=1") + acts_of(&r, "AT+VTS=0") == 0);
		run_ms(&r, 4000);					/* the window is over: no second action */
		assert(acts_of(&r, "AT+CHLD=2") == (k == 0));
		rig_close(&r);
	}

	/* flash and a key that is no code: the key goes to the network, the flash does nothing */
	waiting_rig(&r, 1, NULL);
	a_call_waits(&r);
	flash(&r);
	key_after(&r, '7', 200);
	run_ms(&r, 4500);
	assert(acts_of(&r, "AT+VTS=7") == 1 && acts_of(&r, "AT+CHLD=2") == 0);
	rig_close(&r);

	/* call waiting off: the waiting call is refused with CHLD=0, never with CHUP, and the active call goes on; the record says rejected */
	waiting_rig(&r, 0, NULL);
	a_call_waits(&r);
	assert(acts_of(&r, "AT+CHLD=0") >= 1 && acts_of(&r, "AT+CHUP") == 0 && count_in(log_text(&r), "the waiting tone sounds") == 0);
	r.fm.nlegs = 1;
	run_ms(&r, 1500);
	assert(r.rec_count == 1 && r.rec[0].result == PHONE_RES_REJECTED && !strcmp(r.rec[0].number, "5552") && status_of(&r).line == PHONE_LINE_CALL);
	rig_close(&r);

	/* a blocked number waiting: refused, recorded blocked */
	waiting_rig(&r, 1, b);
	a_call_waits(&r);
	assert(acts_of(&r, "AT+CHLD=0") >= 1 && count_in(log_text(&r), "a waiting call was blocked") == 1 && count_in(log_text(&r), "the waiting tone sounds") == 0);
	r.fm.nlegs = 1;
	run_ms(&r, 1500);
	assert(r.rec_count == 1 && r.rec[0].result == PHONE_RES_BLOCKED);
	rig_close(&r);

	/* the waiting caller gives up: missed, the tone stops, the flash does nothing */
	waiting_rig(&r, 1, NULL);
	a_call_waits(&r);
	r.fm.nlegs = 1;
	run_ms(&r, 1500);
	assert(r.rec_count == 1 && r.rec[0].result == PHONE_RES_MISSED && status_of(&r).leg_count == 1);
	flash(&r);
	run_ms(&r, 4000);
	assert(acts_of(&r, "AT+CHLD=2") == 1 && count_in(log_text(&r), "flash: the call is held") == 1);	/* nothing waits any more: the flash holds the call */
	rig_close(&r);

	/* hanging up with a call waiting releases the active call only */
	waiting_rig(&r, 1, NULL);
	a_call_waits(&r);
	handset(&r, 0);
	run_ms(&r, 500);
	assert(acts_of(&r, "AT+CHLD=11") == 1 && acts_of(&r, "AT+CHUP") == 0);
	rig_close(&r);

	/* the waiting call was taken (hold): it is answered in the record; it is the line's call when the held one ends */
	waiting_rig(&r, 1, NULL);
	a_call_waits(&r);
	r.fm.legs[0].stat = LEG_HELD;
	r.fm.legs[1].stat = LEG_ACTIVE;
	run_ms(&r, 1500);
	assert(status_of(&r).line == PHONE_LINE_CALL && r.rec_count == 0);
	r.fm.legs[0] = r.fm.legs[1];			/* the held call ends: the taken call is the only one */
	r.fm.nlegs = 1;
	run_ms(&r, 1500);
	assert(r.rec_count == 1 && r.rec[0].dir == PHONE_DIR_IN && !strcmp(r.rec[0].number, "5551") && r.rec[0].result == PHONE_RES_ANSWERED);
	assert(status_of(&r).call_id == status_of(&r).legs[0].id);
	r.listed = CALL_GONE;
	r.fm.nlegs = 0;
	run_ms(&r, 1500);
	r.listed = CALL_NONE;
	run_ms(&r, 1500);
	assert(r.rec_count == 2 && !strcmp(r.rec[1].number, "5552") && r.rec[1].result == PHONE_RES_ANSWERED && r.rec[1].answered_ms);
	rig_close(&r);

	/* the active call ends while a call waits: the release tone; the waiting call rings when the handset goes down, and the lift answers it */
	waiting_rig(&r, 1, NULL);
	a_call_waits(&r);
	r.fm.legs[0] = r.fm.legs[1];
	r.fm.legs[0].stat = LEG_INCOMING;
	r.fm.nlegs = 1;
	r.listed = CALL_INCOMING;
	run_ms(&r, 2000);
	assert(status_of(&r).line == PHONE_LINE_RELEASE && acts_of(&r, "AT+CHUP") == 0 && acts_of(&r, "ATA") == 0);
	assert(r.rec_count == 1 && !strcmp(r.rec[0].number, "5551"));
	handset(&r, 0);
	run_ms(&r, 3000);
	assert(status_of(&r).line == PHONE_LINE_RINGING && count_in(log_text(&r), "ring started") >= 1);
	handset(&r, 1);
	run_ms(&r, 1500);
	assert(acts_of(&r, "ATA") >= 1);
	rig_close(&r);
	free(b);
}

/* hold, the second call and the swap */
static void legs_are(struct rig *r, int listed, unsigned int n, int stat0, int stat1)
{
	r->listed = listed;
	r->fm.nlegs = n;
	r->fm.legs[0] = (struct call_leg){ .idx = 1, .dir = 1, .stat = stat0, .number = "5551" };
	r->fm.legs[1] = (struct call_leg){ .idx = 2, .dir = 0, .stat = stat1, .number = "45" };
}

static void test_hold_and_second_call(void)
{
	struct rig r;
	struct phone_status st;

	/* a flash alone holds the call; the held call is all that is left: the recall dial tone */
	waiting_rig(&r, 1, NULL);
	flash(&r);
	run_ms(&r, 2000);
	assert(acts_of(&r, "AT+CHLD=2") == 0);
	run_ms(&r, 1500);
	assert(acts_of(&r, "AT+CHLD=2") == 1 && count_in(log_text(&r), "flash: the call is held") == 1);
	legs_are(&r, CALL_HELD, 1, LEG_HELD, 0);
	run_ms(&r, 2500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_DIAL_TONE && st.call_id == 0 && r.rec_count == 0);	/* the held call's record stays open */
	/* the second call is dialled with the keys; it goes out as an ordinary dial */
	key_after(&r, '4', 300);
	key_after(&r, '5', 600);
	key_after(&r, '#', 900);
	run_ms(&r, 2500);
	assert(acts_of(&r, "ATD45;") == 1 && status_of(&r).line == PHONE_LINE_SETUP && status_of(&r).dir == PHONE_DIR_OUT);
	legs_are(&r, CALL_DIALING, 2, LEG_HELD, LEG_DIALING);
	run_ms(&r, 1500);
	legs_are(&r, CALL_ALERTING, 2, LEG_HELD, LEG_ALERTING);
	run_ms(&r, 1500);
	legs_are(&r, CALL_ACTIVE, 2, LEG_HELD, LEG_ACTIVE);
	run_ms(&r, 2500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_CALL && st.dir == PHONE_DIR_OUT && st.leg_count == 2 && r.rec_count == 0);
	/* a flash alone swaps the two calls */
	flash(&r);
	run_ms(&r, 3500);
	assert(acts_of(&r, "AT+CHLD=2") == 2 && count_in(log_text(&r), "flash: the calls are swapped") == 1);
	/* the second call's remote hangs up: its record is made, and the held call is left: the recall dial tone again */
	legs_are(&r, CALL_HELD, 1, LEG_HELD, 0);
	run_ms(&r, 2500);
	assert(status_of(&r).line == PHONE_LINE_DIAL_TONE && r.rec_count == 1);
	assert(r.rec[0].dir == PHONE_DIR_OUT && !strcmp(r.rec[0].number, "45") && r.rec[0].result == PHONE_RES_ANSWERED);
	/* a flash alone on that dial tone takes the held call back */
	flash(&r);
	run_ms(&r, 3500);
	assert(acts_of(&r, "AT+CHLD=2") == 3 && count_in(log_text(&r), "flash: the held call is taken back") == 1);
	legs_are(&r, CALL_ACTIVE, 1, LEG_ACTIVE, 0);
	run_ms(&r, 2500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_CALL && st.call_id && st.call_id == st.legs[0].id && st.dir == PHONE_DIR_IN);
	legs_are(&r, CALL_GONE, 0, 0, 0);
	run_ms(&r, 1200);
	legs_are(&r, CALL_NONE, 0, 0, 0);
	run_ms(&r, 1500);
	assert(r.rec_count == 2 && !strcmp(r.rec[1].number, "5551") && r.rec[1].result == PHONE_RES_ANSWERED);
	assert(!strstr(log_text(&r), "5551") && !strstr(log_text(&r), "ATD"));
	rig_close(&r);

	/* the second call does not come through: the release tone for it, the held call is taken back, the failed call is recorded with its cause */
	waiting_rig(&r, 1, NULL);
	r.fm.cause = cause_busy;
	flash(&r);
	key_after(&r, '2', 200);			/* flash and 2: held at once */
	run_ms(&r, 800);
	assert(acts_of(&r, "AT+CHLD=2") == 1);
	legs_are(&r, CALL_HELD, 1, LEG_HELD, 0);
	run_ms(&r, 2500);
	key_after(&r, '4', 300);
	key_after(&r, '5', 600);
	key_after(&r, '#', 900);
	run_ms(&r, 2500);
	legs_are(&r, CALL_DIALING, 2, LEG_HELD, LEG_DIALING);
	run_ms(&r, 1500);
	legs_are(&r, CALL_HELD, 1, LEG_HELD, 0);	/* the dialled call is gone */
	run_ms(&r, 2000);
	assert(status_of(&r).line == PHONE_LINE_RELEASE && acts_of(&r, "AT+CHLD=2") == 2);
	assert(r.rec_count == 1 && r.rec[0].dir == PHONE_DIR_OUT && r.rec[0].result == PHONE_RES_BUSY && r.rec[0].cause == 17);
	legs_are(&r, CALL_ACTIVE, 1, LEG_ACTIVE, 0);	/* the first call is back */
	run_ms(&r, 2500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_CALL && st.call_id && r.rec_count == 1);
	rig_close(&r);

	/* two calls: flash 1 releases the active call and takes the held one; flash 0 releases the held one; flash 3 is used up while three-way calling is off */
	waiting_rig(&r, 1, NULL);
	legs_are(&r, CALL_ACTIVE, 2, LEG_HELD, LEG_ACTIVE);
	run_ms(&r, 2500);
	flash(&r);
	key_after(&r, '1', 200);
	run_ms(&r, 800);
	assert(acts_of(&r, "AT+CHLD=1") == 1);
	flash(&r);
	key_after(&r, '0', 200);
	run_ms(&r, 800);
	assert(acts_of(&r, "AT+CHLD=0") == 1);
	flash(&r);
	key_after(&r, '3', 200);
	run_ms(&r, 800);
	assert(acts_of(&r, "AT+CHLD=3") == 0 && acts_of(&r, "AT+VTS=3") == 0);
	rig_close(&r);

	/* a key that is no code after a flash on a single call goes to the network */
	waiting_rig(&r, 1, NULL);
	flash(&r);
	key_after(&r, '1', 200);
	run_ms(&r, 800);
	assert(acts_of(&r, "AT+VTS=1") == 1 && acts_of(&r, "AT+CHLD=1") == 0 && acts_of(&r, "AT+CHLD=2") == 0);
	rig_close(&r);

	/* hanging up with an active and a held call releases the active one only; the held one rings back, and a lift takes it back */
	waiting_rig(&r, 1, NULL);
	legs_are(&r, CALL_ACTIVE, 2, LEG_HELD, LEG_ACTIVE);
	run_ms(&r, 2500);
	handset(&r, 0);
	run_ms(&r, 3000);			/* the modem takes its time over the release: the list still shows the active leg (as on the line) */
	assert(acts_of(&r, "AT+CHLD=12") == 1 && acts_of(&r, "AT+CHUP") == 0);
	legs_are(&r, CALL_HELD, 1, LEG_HELD, 0);
	run_ms(&r, 3000);
	assert(status_of(&r).line == PHONE_LINE_RINGING && count_in(log_text(&r), "a held call rings back") == 1);
	run_ms(&r, 8000);							/* it rings on until somebody answers */
	assert(status_of(&r).line == PHONE_LINE_RINGING && acts_of(&r, "AT+CHLD=2") == 0 && acts_of(&r, "ATA") == 0);
	handset(&r, 1);
	run_ms(&r, 1500);
	assert(acts_of(&r, "AT+CHLD=2") >= 1 && acts_of(&r, "ATA") == 0 && count_in(log_text(&r), "ring stopped (the handset was lifted)") == 2);
	legs_are(&r, CALL_ACTIVE, 1, LEG_ACTIVE, 0);
	run_ms(&r, 2500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_CALL && st.call_id && st.call_id == st.legs[0].id);
	rig_close(&r);

	/* as the modem does it on the line: after the release of the active call the network retrieves the held one, which
	 * comes back listed active with the handset down; it rings back all the same, and a lift reconnects it without a command */
	waiting_rig(&r, 1, NULL);
	legs_are(&r, CALL_ACTIVE, 2, LEG_HELD, LEG_ACTIVE);
	run_ms(&r, 2500);
	handset(&r, 0);
	run_ms(&r, 1000);
	assert(acts_of(&r, "AT+CHLD=12") == 1 && acts_of(&r, "AT+CHUP") == 0);
	assert(status_of(&r).line == PHONE_LINE_RINGING);		/* the recall rings from the hang-up on, before the list has moved */
	legs_are(&r, CALL_ACTIVE, 1, LEG_ACTIVE, 0);			/* the survivor, active again */
	run_ms(&r, 3000);
	assert(acts_of(&r, "AT+CHUP") == 0 && status_of(&r).line == PHONE_LINE_RINGING && count_in(log_text(&r), "a held call rings back") == 1);
	handset(&r, 1);
	run_ms(&r, 1500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_CALL && acts_of(&r, "AT+CHLD=2") == 0 && acts_of(&r, "ATA") == 0 && acts_of(&r, "AT+CHUP") == 0);
	handset(&r, 0);
	run_ms(&r, 1000);
	assert(acts_of(&r, "AT+CHUP") == 1);				/* the last call ends with the handset as any other */
	rig_close(&r);

	/* the handset goes down on the recall dial tone (only a held call): it rings back too, and its remote ending stops the ring and makes the record */
	waiting_rig(&r, 1, NULL);
	flash(&r);
	key_after(&r, '2', 200);
	run_ms(&r, 800);
	legs_are(&r, CALL_HELD, 1, LEG_HELD, 0);
	run_ms(&r, 2500);
	assert(status_of(&r).line == PHONE_LINE_DIAL_TONE);
	handset(&r, 0);
	run_ms(&r, 3000);
	assert(status_of(&r).line == PHONE_LINE_RINGING && acts_of(&r, "AT+CHUP") == 0);
	legs_are(&r, CALL_NONE, 0, 0, 0);
	run_ms(&r, 2500);
	assert(status_of(&r).line == PHONE_LINE_IDLE && r.rec_count == 1 && !strcmp(r.rec[0].number, "5551") && r.rec[0].result == PHONE_RES_ANSWERED);
	rig_close(&r);
}

/* three-way: flash 3 with two calls joins them when the option is on; a conference is one call for the line */
static void test_three_way(void)
{
	struct rig r;
	struct phone_status st;

	want_conference = 1;
	waiting_rig(&r, 1, NULL);
	flash(&r);
	key_after(&r, '2', 200);			/* hold the first call, dial a second one */
	run_ms(&r, 800);
	legs_are(&r, CALL_HELD, 1, LEG_HELD, 0);
	run_ms(&r, 2500);
	key_after(&r, '4', 300);
	key_after(&r, '5', 600);
	key_after(&r, '#', 900);
	run_ms(&r, 2500);
	legs_are(&r, CALL_ACTIVE, 2, LEG_HELD, LEG_ACTIVE);
	run_ms(&r, 2500);
	flash(&r);
	key_after(&r, '3', 200);
	run_ms(&r, 800);
	assert(acts_of(&r, "AT+CHLD=3") == 1 && acts_of(&r, "AT+VTS=3") == 0 && count_in(log_text(&r), "the two calls are joined") == 1);
	/* the network lists both calls as active members of one conference */
	legs_are(&r, CALL_ACTIVE, 2, LEG_ACTIVE, LEG_ACTIVE);
	r.fm.legs[0].mpty = r.fm.legs[1].mpty = 1;
	run_ms(&r, 2500);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_CALL && st.leg_count == 2 && r.rec_count == 0);
	/* in the conference a flash does nothing, and a key goes to the network */
	flash(&r);
	run_ms(&r, 4000);
	assert(acts_of(&r, "AT+CHLD=2") == 1);		/* the one from the hold, none from the flash in the conference */
	key_after(&r, '5', 100);
	run_ms(&r, 500);
	assert(acts_of(&r, "AT+VTS=5") == 1);
	/* the party of the first call leaves: the call goes on with the second party, and the first call's record is made */
	r.fm.legs[0] = r.fm.legs[1];
	r.fm.nlegs = 1;
	run_ms(&r, 2500);
	assert(status_of(&r).line == PHONE_LINE_CALL && r.rec_count == 1 && !strcmp(r.rec[0].number, "5551") && r.rec[0].result == PHONE_RES_ANSWERED);
	rig_close(&r);

	/* the option off: flash 3 is used up and nothing is sent */
	want_conference = 0;
	waiting_rig(&r, 1, NULL);
	legs_are(&r, CALL_ACTIVE, 2, LEG_HELD, LEG_ACTIVE);
	run_ms(&r, 2500);
	flash(&r);
	key_after(&r, '3', 200);
	run_ms(&r, 800);
	assert(acts_of(&r, "AT+CHLD=3") == 0 && acts_of(&r, "AT+VTS=3") == 0 && count_in(log_text(&r), "three-way calling is off") == 1);
	rig_close(&r);

	/* with a waiting call flash 3 is used up too: no join */
	want_conference = 1;
	waiting_rig(&r, 1, NULL);
	a_call_waits(&r);
	flash(&r);
	key_after(&r, '3', 200);
	run_ms(&r, 800);
	assert(acts_of(&r, "AT+CHLD=3") == 0 && acts_of(&r, "AT+VTS=3") == 0);
	rig_close(&r);
	want_conference = 0;
}

/* giving up the second call while one is held releases only the dialling leg; a third incoming call is left to the network and counted as missed */
static void second_call_dialling(struct rig *r)
{
	waiting_rig(r, 1, NULL);
	flash(r);
	key_after(r, '2', 200);
	run_ms(r, 800);
	legs_are(r, CALL_HELD, 1, LEG_HELD, 0);
	run_ms(r, 2500);
	key_after(r, '4', 300);
	key_after(r, '5', 600);
	key_after(r, '#', 900);
	run_ms(r, 2500);
	legs_are(r, CALL_ALERTING, 2, LEG_HELD, LEG_ALERTING);
	run_ms(r, 1500);
	assert(status_of(r).line == PHONE_LINE_SETUP);
}

static void test_abandoned_second_call(void)
{
	struct rig r;

	/* on this modem a dialling or alerting leg cannot be released alone: the hang-up ends every call, said once, and nothing rings back */
	second_call_dialling(&r);
	handset(&r, 0);
	run_ms(&r, 1500);
	assert(acts_of(&r, "AT+CHUP") == 1 && acts_of(&r, "AT+CHLD=1") == 0 && acts_of(&r, "AT+CHLD=12") == 0);
	assert(count_in(log_text(&r), "cannot be released alone, all calls are hung up") == 1);
	legs_are(&r, CALL_NONE, 0, 0, 0);
	run_ms(&r, 3000);
	assert(status_of(&r).line == PHONE_LINE_IDLE && acts_of(&r, "AT+CHUP") == 1);
	rig_close(&r);

	/* the hang-up with an active and a held call: the modem refuses the release of the active one, AT+CHUP follows, said once */
	waiting_rig(&r, 1, NULL);
	legs_are(&r, CALL_ACTIVE, 2, LEG_HELD, LEG_ACTIVE);
	run_ms(&r, 2500);
	r.refuse_chld1 = 1;
	handset(&r, 0);
	run_ms(&r, 1500);
	assert(acts_of(&r, "AT+CHLD=12") == 1 && acts_of(&r, "AT+CHUP") == 1 && count_in(log_text(&r), "all calls are hung up instead") == 1);
	rig_close(&r);
}

static void test_third_call(void)
{
	struct rig r;

	waiting_rig(&r, 1, NULL);
	a_call_waits(&r);
	r.fm.legs[2] = (struct call_leg){ .idx = 3, .dir = 1, .stat = LEG_WAITING, .number = "5553" };
	r.fm.nlegs = 3;
	run_ms(&r, 4000);
	assert(count_in(log_text(&r), "a further incoming call while two calls are in use: left to the network") == 1);
	assert(r.rec_count == 1 && !strcmp(r.rec[0].number, "5553") && r.rec[0].result == PHONE_RES_MISSED && r.rec[0].dir == PHONE_DIR_IN);
	assert(!strstr(log_text(&r), "5553") && acts_of(&r, "AT+CHLD=0") == 0 && acts_of(&r, "AT+CHUP") == 0);
	assert(count_in(log_text(&r), "a call is waiting") == 1);	/* the first waiting call is still the one offered */
	rig_close(&r);
}

static const char *ccwa_text(void *user, const char *cmd)
{
	(void)user;
	return !strcmp(cmd, "AT+CCWA=1,2,1") ? "+CCWA: 1,1\r\n\r\nOK\r\n" : "";
}

/* call waiting is switched on at the modem setup and read back once */
static void test_call_waiting_setup(void)
{
	struct rig r;

	rig_init(&r);
	r.ops.body = fm_body;
	r.fm.text = ccwa_text;
	r.cfg.call_waiting = 1;
	rig_open(&r);
	assert(status_of(&r).call_waiting_net == PHONE_UNKNOWN);
	run_ms(&r, 3000);
	assert(acts_of(&r, "AT+CCWA=1,1,1") == 1 && status_of(&r).call_waiting_net == 1);
	phone_modem_up(r.p);							/* a modem that came back: written and read again */
	run_ms(&r, 3000);
	assert(acts_of(&r, "AT+CCWA=1,1,1") == 2);
	rig_close(&r);

	rig_init(&r);
	r.ops.body = fm_body;
	r.fm.text = ccwa_text;
	rig_open(&r);
	run_ms(&r, 3000);
	assert(acts_of(&r, "AT+CCWA=1,1,1") == 0 && status_of(&r).call_waiting_net == PHONE_UNKNOWN);
	rig_close(&r);
}

/* 2. no service: the congestion tone instead of the dial tone (ITU-T E.180) while the modem is absent or the network says not registered; digits still go out when the modem is there */
static void test_no_service(void)
{
	struct rig r;
	struct phone_status st;
	unsigned int before;

	/* the modem port is missing */
	rig_init(&r);
	r.fm.lost = 1;
	r.m.dtmf_events[0].at = 1500; r.m.dtmf_events[0].code = 0x04;
	r.m.dtmf_events[1].at = 1600; r.m.dtmf_events[1].code = 0x0c;
	r.m.dtmf_event_count = 2;
	rig_open(&r);
	run_ms(&r, 200);
	handset(&r, 1);
	run_ms(&r, 100);
	st = status_of(&r);
	assert(st.line == PHONE_LINE_NO_SERVICE && st.off_hook);
	before = r.m.log_count;
	run_ms(&r, 2200);
	assert(writes_of(&r, before, REG_OCON, OCON_ENABLE_SET) >= 3);	/* a cadence, not a continuous tone: 0.25 s on, 0.25 s off */
	assert(writes_of(&r, before, REG_OCON, 0) >= 3);
	assert(acts_of(&r, "ATD4;") == 0 && status_of(&r).line == PHONE_LINE_NO_SERVICE);	/* no modem: the keys dial nothing */
	handset(&r, 0);
	run_ms(&r, 600);
	assert(status_of(&r).line == PHONE_LINE_IDLE);
	rig_close(&r);

	/* the modem is there but the network says not registered: congestion tone, and the keys are still dialled (emergency numbers) */
	memset(asked, 0, sizeof(asked));
	cereg_reply = "+CEREG: 0,2\r\n\r\nOK\r\n";			/* searching */
	rig_init(&r);
	with_questions(&r);
	r.m.dtmf_events[0].at = 17000; r.m.dtmf_events[0].code = 0x01;
	r.m.dtmf_events[1].at = 17100; r.m.dtmf_events[1].code = 0x0c;
	r.m.dtmf_event_count = 2;
	rig_open(&r);
	run_ms(&r, 14000);
	assert(status_of(&r).registered == 0 && status_of(&r).modem_up);
	handset(&r, 1);
	run_ms(&r, 100);
	assert(status_of(&r).line == PHONE_LINE_NO_SERVICE);
	run_ms(&r, 3500);
	assert(acts_of(&r, "ATD1;") == 1);
	assert(status_of(&r).line == PHONE_LINE_SETUP && status_of(&r).dir == PHONE_DIR_OUT);
	handset(&r, 0);
	run_ms(&r, 800);

	/* registered again: the dial tone is back for a lifted handset, and for one that was waiting in the no-service tone */
	cereg_reply = "+CEREG: 0,1\r\n\r\nOK\r\n";
	run_ms(&r, 14000);
	assert(status_of(&r).registered == 1);
	handset(&r, 1);
	run_ms(&r, 100);
	assert(status_of(&r).line == PHONE_LINE_DIAL_TONE);
	handset(&r, 0);
	run_ms(&r, 600);
	cereg_reply = "+CEREG: 0,0\r\n\r\nOK\r\n";
	run_ms(&r, 14000);
	handset(&r, 1);
	run_ms(&r, 100);
	assert(status_of(&r).line == PHONE_LINE_NO_SERVICE);
	cereg_reply = "+CEREG: 0,5\r\n\r\nOK\r\n";			/* roaming counts as service */
	r.listed = CALL_NONE;
	run_ms(&r, 14000);
	assert(status_of(&r).registered == 1 && status_of(&r).line == PHONE_LINE_DIAL_TONE);	/* it came back under the handset */
	cereg_reply = "+CEREG: 0,1\r\n\r\nOK\r\n";
	rig_close(&r);
}

/* 3. the modem's audio mode is read at every modem setup and only reported: slave (second field 1) is what the bridge needs */
static void test_audio_check(void)
{
	struct rig r;
	unsigned int k;

	rig_init(&r);
	with_questions(&r);
	rig_open(&r);
	assert(status_of(&r).audio_slave == PHONE_UNKNOWN);
	run_ms(&r, 3000);
	assert(status_of(&r).audio_slave == 1 && count_in(log_text(&r), "not in slave mode") == 0);
	rig_close(&r);

	qdai_reply = "+QDAI: x,0,0,4,0,0,1,1\r\n\r\nOK\r\n";		/* master */
	rig_init(&r);
	with_questions(&r);
	rig_open(&r);
	run_ms(&r, 15000);
	assert(status_of(&r).audio_slave == 0);
	assert(count_in(log_text(&r), "not in slave mode") == 1);		/* said once per modem open, not at every round */
	for (k = 0; k < r.act_count && k < ACTS_MAX; k++)
		assert(strncmp(r.acts[k], "AT+QDAI=", 8));			/* never written */
	r.act_count = 0;
	phone_modem_up(r.p);							/* the port came back: checked again, said again */
	qdai_reply = "+QDAI: x,1,0,4,0,0,1,1\r\n\r\nOK\r\n";
	run_ms(&r, 3000);
	assert(status_of(&r).audio_slave == 1);
	rig_close(&r);
}

/* 4. the call list is asked for every 250 ms while a call is being set up, while our hang-up is pending and for 2 s after an answer; every second otherwise */
static void test_fast_call_list(void)
{
	struct rig r;
	unsigned long long t;
	unsigned int polls;

	rig_init(&r);
	r.m.dtmf_events[0].at = 3500; r.m.dtmf_events[0].code = 0x04;
	r.m.dtmf_events[1].at = 3600; r.m.dtmf_events[1].code = 0x0c;
	r.m.dtmf_event_count = 2;
	rig_open(&r);
	run_ms(&r, 100);
	polls = r.fm.begins;
	run_ms(&r, 3000);
	assert(r.fm.begins - polls <= 4);					/* idle: about one a second (the setup commands are done) */
	handset(&r, 1);
	run_ms(&r, 800);
	assert(status_of(&r).line == PHONE_LINE_SETUP);
	r.listed = CALL_DIALING;
	run_ms(&r, 1200);
	polls = r.fm.begins;
	run_ms(&r, 2000);
	assert(r.fm.begins - polls >= 7 && r.fm.begins - polls <= 9);		/* set-up: every 250 ms */
	r.listed = CALL_ACTIVE;
	t = r.now;
	while (status_of(&r).line != PHONE_LINE_CALL && r.now - t < 2000)
		run_ms(&r, HOOK_POLL_MS);
	assert(status_of(&r).line == PHONE_LINE_CALL && r.now - t <= 300);	/* the answer shows within 300 ms of the modem's change */
	polls = r.fm.begins;
	run_ms(&r, 1900);
	assert(r.fm.begins - polls >= 6);					/* still fast for 2 s after the answer */
	run_ms(&r, 1500);							/* then back to once a second */
	polls = r.fm.begins;
	run_ms(&r, 4000);
	assert(r.fm.begins - polls <= 5);
	rig_close(&r);
}

/* 5. the modem setup: no echo, numeric errors, then the caller's number and the extended ring report; the two audio levels only when they are set, the earpiece level only inside the range the modem reports */
static void test_modem_setup(void)
{
	struct rig r;
	unsigned int k, first;

	rig_init(&r);
	with_questions(&r);
	rig_open(&r);
	run_ms(&r, 3000);
	assert(r.act_count >= 4);
	assert(!strcmp(r.acts[0], "ATE0") && !strcmp(r.acts[1], "AT+CMEE=1") && !strcmp(r.acts[2], "AT+CLIP=1") && !strcmp(r.acts[3], "AT+CRC=1"));
	for (k = 0; k < r.act_count && k < ACTS_MAX; k++)			/* unset = left as the modem has it */
		assert(strncmp(r.acts[k], "AT+CLVL=", 8) && strncmp(r.acts[k], "AT+QRXGAIN=", 11));
	rig_close(&r);

	/* both set and in range */
	rig_init(&r);
	with_questions(&r);
	r.cfg.earpiece_volume = 3; r.cfg.volume_set = 1;
	r.cfg.modem_rx_gain = 4606; r.cfg.rx_gain_set = 1;
	rig_open(&r);
	run_ms(&r, 6000);
	assert(acts_of(&r, "AT+QRXGAIN=4606") == 1 && acts_of(&r, "AT+CLVL=3") == 1);
	first = r.act_count;
	run_ms(&r, 15000);
	assert(r.act_count == first || acts_of(&r, "AT+CLVL=3") == 1);	/* written once per modem setup, not at every round of questions */
	r.act_count = 0;
	phone_modem_up(r.p);
	run_ms(&r, 6000);
	assert(acts_of(&r, "ATE0") == 1 && acts_of(&r, "AT+QRXGAIN=4606") == 1 && acts_of(&r, "AT+CLVL=3") == 1);	/* and again after a new setup */
	rig_close(&r);

	/* a level the modem does not have is refused with one line and not written */
	rig_init(&r);
	with_questions(&r);
	r.cfg.earpiece_volume = 9; r.cfg.volume_set = 1;
	rig_open(&r);
	run_ms(&r, 15000);
	assert(acts_of(&r, "AT+CLVL=9") == 0);
	assert(count_in(log_text(&r), "outside the modem's range") == 1);
	rig_close(&r);
}

/* 6. ringback 0: the network's ringing tone is all the caller hears; the local one is off */
static void test_ringback_option(void)
{
	struct rig r;
	unsigned int before;

	for (int off = 0; off < 2; off++) {
		rig_init(&r);
		r.cfg.ringback = off ? PHONE_RINGBACK_OFF : PHONE_RINGBACK_ON;
		r.m.dtmf_events[0].at = 300; r.m.dtmf_events[0].code = 0x04;
		r.m.dtmf_events[1].at = 375; r.m.dtmf_events[1].code = 0x0c;
		r.m.dtmf_event_count = 2;
		rig_open(&r);
		handset(&r, 1);
		run_ms(&r, 800);
		r.listed = CALL_DIALING;
		run_ms(&r, 1500);
		r.listed = CALL_ALERTING;
		run_ms(&r, 500);
		before = r.m.log_count;
		run_ms(&r, 6000);						/* one full cadence: 1 s on, 4 s off */
		assert(status_of(&r).line == PHONE_LINE_SETUP);
		if (off)
			assert(writes_of(&r, before, REG_OCON, OCON_ENABLE_SET) == 0);
		else
			assert(writes_of(&r, before, REG_OCON, OCON_ENABLE_SET) >= 1);
		rig_close(&r);
	}
}

/* the measuring option: the modem's audio level is logged every 100 ms while a call is being set up, and only then */
static unsigned int fake_windows, fake_peak;

static int fake_level(void *ctx, unsigned int *peak, unsigned int *windows)
{
	(void)ctx;
	*peak = fake_peak;
	*windows = fake_windows;
	return 0;
}

static void test_level_trace(void)
{
	struct rig r;
	unsigned int k;

	for (int on = 0; on < 2; on++) {
		rig_init(&r);
		r.cfg.level_trace = on;
		r.hooks.modem_level = fake_level;
		fake_windows = 0;
		fake_peak = 0;
		r.m.dtmf_events[0].at = 300; r.m.dtmf_events[0].code = 0x04;
		r.m.dtmf_events[1].at = 375; r.m.dtmf_events[1].code = 0x0c;
		r.m.dtmf_event_count = 2;
		rig_open(&r);
		handset(&r, 1);
		run_ms(&r, 800);
		r.listed = CALL_DIALING;
		for (k = 0; k < 10; k++) {			/* silence: a window every 100 ms */
			fake_windows++;
			run_ms(&r, 100);
		}
		r.listed = CALL_ALERTING;
		for (k = 0; k < 10; k++) {			/* a tune arrives */
			fake_windows++;
			fake_peak = 4000;
			run_ms(&r, 100);
		}
		r.listed = CALL_ACTIVE;
		run_ms(&r, 1500);
		if (on) {
			assert(count_in(log_text(&r), "network audio setup: window") >= 8);
			assert(count_in(log_text(&r), "network audio alerting: window") >= 8);
			assert(strstr(log_text(&r), "alerting: window 20 peak 4000"));
			assert(strstr(log_text(&r), "setup: window 10 peak 0"));
			for (k = 21; k < 30; k++) {		/* nothing once the call is answered */
				char text[48];

				snprintf(text, sizeof(text), "window %u peak", k);
				assert(!strstr(log_text(&r), text));
			}
		} else {
			assert(!strstr(log_text(&r), "network audio"));
		}
		rig_close(&r);
	}
}

/*
 * ringback auto: the local ringing tone plays from the alerting and stops for good in that call once the modem's audio stays above
 * the threshold for three windows (the network's own ringback tone or a caller's ringback tune). The levels come from a
 * scripted modem: a window every 100 ms, its peak by scenario.
 */
enum { LV_SILENCE, LV_COMFORT_NOISE, LV_TUNE_FROM_START, LV_TUNE_LATE };
static int lv_scenario;
static unsigned long long lv_alerting_at, lv_t0;
static const unsigned long long *lv_now;

static int scripted_level(void *ctx, unsigned int *peak, unsigned int *windows)
{
	unsigned long long t = *lv_now;
	unsigned long long k = (t - lv_t0) / 100, since;

	(void)ctx;
	*windows = (unsigned int)k;
	since = t > lv_alerting_at ? t - lv_alerting_at : 0;
	switch (lv_scenario) {
	case LV_COMFORT_NOISE:
		*peak = 6;
		break;
	case LV_TUNE_FROM_START:
	case LV_TUNE_LATE: {
		unsigned long long on = lv_scenario == LV_TUNE_FROM_START ? 0 : 3000;

		if (!lv_alerting_at || since < on)
			*peak = 1;
		else if (k % 17 == 0)
			*peak = 1;			/* a quiet instant in the music */
		else
			*peak = 3000 + (unsigned int)(k % 7) * 900;
		break;
	}
	default:
		*peak = 1;
	}
	return 0;
}

/* an outgoing call that is dialled, set up for 1.2 s, then alerting for 8 s; the number of times the local tone started in the alerting */
static unsigned int alerting_tone_starts(int scenario, int mode, int with_hook, struct rig *r)
{
	unsigned int before;

	lv_scenario = scenario;
	rig_init(r);
	r->cfg.ringback = mode;
	if (with_hook)
		r->hooks.modem_level = scripted_level;
	r->m.dtmf_events[0].at = 300; r->m.dtmf_events[0].code = 0x04;
	r->m.dtmf_events[1].at = 375; r->m.dtmf_events[1].code = 0x0c;
	r->m.dtmf_event_count = 2;
	rig_open(r);
	lv_now = &r->now;
	lv_t0 = r->now;
	lv_alerting_at = 0;
	handset(r, 1);
	run_ms(r, 800);
	r->listed = CALL_DIALING;
	run_ms(r, 1200);
	r->listed = CALL_ALERTING;
	before = r->m.log_count;
	lv_alerting_at = r->now;
	run_ms(r, 8100);
	assert(status_of(r).line == PHONE_LINE_SETUP);
	return writes_of(r, before, REG_OCON, OCON_ENABLE_SET);
}

static void test_ringback_auto(void)
{
	struct rig r;

	/* silence and comfort noise: nothing from the network, the local tone runs its cadence (a burst at the alerting and one 5 s later) */
	assert(alerting_tone_starts(LV_SILENCE, PHONE_RINGBACK_AUTO, 1, &r) == 2);
	assert(count_in(log_text(&r), "network's own audio") == 0);
	rig_close(&r);
	assert(alerting_tone_starts(LV_COMFORT_NOISE, PHONE_RINGBACK_AUTO, 1, &r) == 2);
	rig_close(&r);

	/* the tune plays from the start of the alerting: at most the first burst starts, it is cut within a third of a second, nothing starts again, quiet instants included */
	assert(alerting_tone_starts(LV_TUNE_FROM_START, PHONE_RINGBACK_AUTO, 1, &r) <= 1);
	assert(count_in(log_text(&r), "the network's own audio is heard") == 1);
	rig_close(&r);

	/* a tune that arrives late: the local tone was playing its cadence, stops at the tune, and the burst after it never comes */
	assert(alerting_tone_starts(LV_TUNE_LATE, PHONE_RINGBACK_AUTO, 1, &r) == 1);
	assert(count_in(log_text(&r), "the network's own audio is heard") == 1);
	rig_close(&r);

	/* on: always local, whatever the network plays; off: never local; no level source: the local tone as before */
	assert(alerting_tone_starts(LV_TUNE_FROM_START, PHONE_RINGBACK_ON, 1, &r) == 2);
	rig_close(&r);
	assert(alerting_tone_starts(LV_TUNE_FROM_START, PHONE_RINGBACK_OFF, 1, &r) == 0);
	rig_close(&r);
	assert(alerting_tone_starts(LV_TUNE_FROM_START, PHONE_RINGBACK_AUTO, 0, &r) == 2);
	rig_close(&r);
}

static void test_dead_chip_fails_loud(void)
{
	struct rig r;

	/* a unit whose line chip's own firmware never started (an erased chip flash): the stage
	 * refuses and names the cause loudly, instead of leaving a silent dead line */
	rig_init(&r);
	r.m.mstrstat = 0x00;
	assert(stage0_identify(&r.c, r.log) == -ENODEV);
	fflush(r.log);
	assert(strstr(r.lbuf, "master status 0x00"));
	assert(strstr(r.lbuf, "its own firmware did not start"));
	fclose(r.log);
	free(r.lbuf);				/* the close moves the buffer: free the one it leaves */
}

int main(void)
{
	test_open_close();
	test_dead_chip_fails_loud();
	test_modem_late();
	test_port_lost_in_call();
	test_cfg_in_call();
	test_cfg_offhook_idle();
	test_ring_test();
	test_hangup();
	test_events();
	test_no_number_in_log();
	test_late_step();
	test_busy_line_refuses();
	test_ring_trip_answers_at_once();
	test_machine_answer();
	test_machine_waiting();
	test_modem_timeouts();
	test_idle_questions();
	test_dtmf_in_call();
	test_no_service();
	test_audio_check();
	test_fast_call_list();
	test_modem_setup();
	test_ringback_option();
	test_level_trace();
	test_ringback_auto();
	test_result_mapping();
	test_call_records();
	test_speed_dial();
	test_blocking_calls();
	test_clir();
	test_service_codes();
	test_hotline();
	test_line_test();
	test_alarms();
	test_forwarding();
	test_legs();
	test_call_waiting();
	test_call_waiting_setup();
	test_hold_and_second_call();
	test_three_way();
	test_abandoned_second_call();
	test_third_call();
	printf("daemon: all tests passed\n");
	return 0;
}
