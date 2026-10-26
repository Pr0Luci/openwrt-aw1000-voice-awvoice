// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The phone line's state machine: the hook watch, the dial tone, digit collection and dialling, the ring with the caller's
 * identity, the call control through the modem client, and the progress tones, cut into open / step / close (phone.h).
 * The stage tool's loop and the daemon's event loop drive the same code. */
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "call.h"
#include "cid.h"
#include "alarm.h"
#include "blocking.h"
#include "forward.h"
#include "wideband.h"
#include "line.h"
#include "numbers.h"
#include "phonebook.h"
#include "modemq.h"
#include "ring.h"
#include "slic_map.h"
#include "stages.h"

/*
 * The line state of a handset held on a fed line (stages 6..8). The hook engine tells what the handset does; this
 * says what the line is showing it.
 */
enum line_state {
	LINE_IDLE,		/* waiting: the handset is down, or up with nothing to show yet */
	LINE_DIAL_TONE,		/* the dial tone runs */
	LINE_DIALLING,		/* a digit stopped the tone: the caller is dialling */
	LINE_CALL,		/* a call is in progress */
	LINE_SETUP,		/* the network is setting an outgoing call up (ringing tone from the alerting) */
	LINE_RELEASE,		/* a call was released while the caller is on the line: the busy or the congestion tone */
	LINE_NO_SERVICE,	/* the handset is up while the modem is absent or the network says not registered: the congestion tone */
};

/* the record and the policy of the waiting call (a second incoming call during a call): one at a time */
struct wait_call {
	enum phone_dir dir;		/* of the call the record is about */
	int open;			/* a waiting call is being followed */
	unsigned int id, tries;
	char number[PHONE_NUMBER_MAX + 1];
	unsigned long long start_ms, answered_ms, tried_ms;
	int toned;			/* the waiting tone has sounded for this call (said once in the log) */
	int refused, blocked;		/* it is being refused (the line option is off, or the blocking rules say so, or the user refused it) */
};

struct leg_entry {
	int idx, stat;
	unsigned int id;
	char number[PHONE_NUMBER_MAX + 1];
};

struct phone {
	enum line_state state;
	struct hookwatch hw;		/* the hook engine: break durations on the monotonic clock */
	uint8_t lcr;			/* the last hook register value read */
	const struct call_ops *call;	/* optional modem answering; 0 = the hold knows nothing of calls */
	const struct board *board;	/* the board file (the FSK words) */
	/* a call */
	int incoming;			/* a voice call is incoming: the handset is not given a dial tone */
	int offered;			/* the incoming call arrived while the handset was down: it rings, and a lift answers it */
	int refused;		/* the incoming call arrived while the handset was up, or is blocked: the caller is released and the phone does not ring */
	int blocked;		/* ... because the blocking rules say so (it is never offered, not even when the handset goes down) */
	unsigned int tries;		/* answers (or refusals) sent for this incoming call */
	unsigned long long tried_ms;	/* when the last one was sent */
	int ending;			/* the hang-up was sent: the call list may still show the call for a poll or two */
	int alerting;			/* the network reported the remote ringing (the ringing tone starts then) */
	int setup_said;			/* "the network is setting the call up" was logged for this attempt (once) */
	int cause;			/* the network's release cause of the released call (TS 24.008) */
	int cause_pending;		/* the cause is asked for: the release tone waits for it */
	int cause_want;			/* the cause query is not started yet */
	enum line_state gone_from;	/* the state the call was in when the network released it */
	/* dialling */
	char dial[DIAL_NUMBER_MAX + 1];	/* the outgoing digits collected so far */
	unsigned long long dial_last_ms;	/* the clock time of the last DTMF event of the dial session (the pause between digits is timed from it) */
	/* progress tones */
	unsigned long long prog_start_ms;	/* the clock time the current progress tone's cadence started */
	int prog_on;			/* the progress tone is on (1), off (0), or not set yet (-1) */
	/* ringing */
	int ring_ready;			/* the ring generator is loaded */
	int ringing;			/* the line feed is set to ringing */
	unsigned long long ring_since_ms, ring_log_ms, ring_end_ms;	/* the ring started, last supply log, a ring test's end (0 = rings with the call) */
	unsigned long long gap_since_ms;	/* the ring burst ended at this time (0 = it is on, or none was seen) */
	int cid_pending;		/* the caller's identity is still to be sent during this ring */
	unsigned long long cid_wait_ms;	/* the silent gap of a caller id sent before the first ring: the ring voltage waits for it */
	int burst_seen;			/* the line feed register showed the ring burst */
	char cid_number[CID_NUMBER_MAX + 1];
	uint8_t last_lf;		/* the last line feed register value seen while ringing (logged on change) */
	/* the modem client: one request open at a time, the others wait their turn */
	int mq_busy;			/* a request is open */
	enum call_req mq_kind;		/* of the open request */
	char mq_cmd[MODEM_CMD_MAX];	/* the open command request's text */
	char mq_wait[MODEM_QUEUE][MODEM_CMD_MAX];	/* commands waiting, oldest first */
	unsigned int mq_waiting;
	unsigned long long poll_due_ms;	/* the next call-list query */
	unsigned int poll_bad;		/* call-list queries in a row that got no answer */
	int port_lost;			/* the modem port is gone: no request starts again */
	/* what the modem said to the idle questions (modemq.h); MQ_UNKNOWN / "" until it did */
	unsigned int info_next;		/* the question to ask next */
	unsigned long long info_due_ms;
	int sim_ready, registered, ims, rsrp_dbm, audio_slave, clir, clir_status;
	enum { RING_FOR_CALL, RING_FOR_ALARM } ring_for;	/* what the ring that is being started is for: an alarm sends no caller id */
	unsigned long long alarm_next_ms;	/* the next check of the alarm clock */
	/* call forwarding at the network: erase, register, read back (forward.h) */
	enum { FWD_STAGE_IDLE, FWD_STAGE_ERASING, FWD_STAGE_REGISTERING, FWD_STAGE_QUERY_DUE, FWD_STAGE_QUERYING } fwd_stage;
	int fwd_want;			/* the settings changed (or are new): the exchange starts when the line is idle */
	/* the HD voice change: the chip's wideband mode is applied at once, then the modem's PCM rate and its restart */
	enum { HD_IDLE, HD_QDAI_WRITE, HD_CFUN } hd_stage;
	int hd_want;			/* the HD setting changed: the modem exchange starts when the line is idle */
	char qdai_head[8];		/* the AT+QDAI? first field as the modem prints it (a digit or the literal "x"), written back verbatim */
	struct phone_forward fwd_job;	/* the settings the running exchange sets */
	struct phone_forward_state fwd_net;
	struct alarm_watch alarm_watch;	/* the minute that rang last and whether the clock was reported untrusted */
	struct wideband wb;		/* the HD voice state: the chip's wideband mode and the line's own narrowband snapshot */
	int lt_active, lt_done;		/* the line test runs / has a result to hand over */
	unsigned int lt_n;
	unsigned long long lt_next_ms;
	long lt_battery_sum, lt_loop_sum;
	int lt_expected, lt_off, lt_interrupted, lt_error;
	struct phone_line_test_result lt_res;
	struct leg_entry leg[CALL_LEGS_MAX];		/* the voice calls of the last call list, each with its own call id */
	unsigned int nlegs;
	struct wait_call wait;
	int out_listed;			/* the second call being set up has been seen in the call list */
	int abandon_cmd;		/* AT+CHLD=1x was sent to release one call at the hang-up: AT+CHUP follows if the modem refuses it */
	unsigned int extra_id;		/* the last further waiting call that was left to the network */
	int primary_listed;		/* the line's own call is among the listed calls */
	int rec_defer;			/* the record of a failed second call waits for the release cause */
	int wait_tone_on;		/* the waiting tone's burst is sounding */
	int recall;			/* a held call is ringing the phone back (the handset went down with it on hold) */
	int recall_active;		/* the recall's survivor came back as an active call: a lift only reconnects the audio */
	int released_one;		/* the hang-up asked for the release of one call (AT+CHLD=1x): its survivor is not a call that connected with the handset down */
	int wait_promoted;		/* the call that is incoming now was the waiting call: it is offered even with the handset up */
	int flash_open;			/* a flash was made in a call: the next key within FLASH_CODE_MS is a code */
	unsigned long long flash_until_ms;
	int ccwa_stage;			/* the read-back of the network's call waiting: 0 none, 1 due, 2 asked */
	int ccwa_net;
	unsigned int leg_primary;	/* the line call id that was given to a leg */
	int hot_armed;			/* the hotline may fire in this stretch of dial tone (a digit, a hang-up or a call ends it) */
	unsigned long long hot_since_ms;	/* the dial tone began (again) at this time */
	int audio_warned;		/* the "not in slave mode" warning went out for this modem setup */
	int clvl_pending;		/* the earpiece level waits for the modem's range (asked once per setup) */
	int key_drop_logged;		/* a key found the queue full in this call: said once */
	unsigned long long level_ms;	/* the last reading of the modem's audio level (measuring option) */
	unsigned int level_windows;
	unsigned int net_run;		/* windows in a row above NET_AUDIO_PEAK in this call's set-up */
	int net_tone;			/* the network's own audio is heard in this call: the local ringing tone is off for good */
	char network[MQ_NETWORK_MAX], band[MQ_NETWORK_MAX];
	/* the watch's own timing record: the hook must be read every few ms whatever else the loop does */
	unsigned long long hook_at_ms;	/* the clock time of the last hook read */
	unsigned long long slow_ms;	/* the slowest task since the last hook read ... */
	const char *slow_what;		/* ... and its name */
	unsigned int hook_reads, hook_gaps, hook_gap_max, glitch_logged;
	unsigned int read_fail, irq_fail;	/* hook / event register reads that failed in a row */
	unsigned long long tone_retry_ms;	/* a failed tone start or stop is not tried again before this time */
	/* the line the core serves and the front end's side */
	struct chip *c;
	unsigned int ch;
	FILE *log;
	struct phone_cfg cfg, pending_cfg;	/* the settings in force; new settings wait until the line is idle with the handset down */
	int cfg_pending;
	struct phone_hooks hooks;
	int det_on;			/* the tone detector's events are enabled (phone_open) */
	/* calls, for the status and the events */
	unsigned int call_seq;		/* calls seen since the start (the id of the last one) */
	unsigned int call_id;		/* the current call's id, 0 = none */
	enum phone_dir dir;
	unsigned int calls_in, calls_out;
	enum phone_line reported;	/* the line state the event hook was last told */
	int call_seen;			/* the modem's last reported call state (enum call_state), CALL_NONE at the start */
	unsigned long long call_since_ms, answered_ms;
	char peer[PHONE_NUMBER_MAX + 1];	/* the call's remote number (outgoing: dialled; incoming: from the call list): for the status reply and the call log only, never logged */
	/* the call log's record of the current call */
	int rec_open;			/* a call began and its record was not handed on yet */
	int rec_refused;		/* an incoming call was refused because the line was in use */
	int rec_blocked;		/* an incoming call was refused by the blocking rules */
	int rec_cause;
	unsigned long long rec_start_ms, rec_gone_ms;	/* the call began; the network released it (0 = not seen) */
	unsigned int modem_timeouts;
	unsigned long long modem_reply_ms;
	unsigned int faults;
	int battery_mv, loop_ua;	/* the last guard sample */
	/* the step */
	unsigned long long step_ms;	/* the time of the step that is running (or ran last) */
	unsigned long long next_ring_ms, next_sample_ms;
	unsigned int sample;
};

static void legs_text(const struct phone *p, char *out, size_t cap);

/* what the line shows the API: the ringing phone is its own state, the rest follow the line state */
static enum phone_line phone_line_of(const struct phone *p)
{
	static const enum phone_line shown[] = {
		[LINE_IDLE] = PHONE_LINE_IDLE, [LINE_DIAL_TONE] = PHONE_LINE_DIAL_TONE, [LINE_DIALLING] = PHONE_LINE_DIALLING,
		[LINE_CALL] = PHONE_LINE_CALL, [LINE_SETUP] = PHONE_LINE_SETUP, [LINE_RELEASE] = PHONE_LINE_RELEASE,
		[LINE_NO_SERVICE] = PHONE_LINE_NO_SERVICE,
	};

	return p->ringing ? PHONE_LINE_RINGING : shown[p->state];
}

static void emit(struct phone *p, enum phone_event_kind kind, unsigned long long now)
{
	struct phone_event ev = { .kind = kind, .line = phone_line_of(p), .call_id = p->call_id, .dir = p->dir,
		.call = (enum call_state)p->call_seen, .at_ms = now };

	if (p->hooks.event)
		p->hooks.event(p->hooks.ctx, &ev);
}

/* the event hook hears of a change of what the line shows, not of every write */
static void report_line(struct phone *p, unsigned long long now)
{
	if (phone_line_of(p) == p->reported)
		return;
	p->reported = phone_line_of(p);
	emit(p, PHONE_EV_LINE, now);
}

static void set_line(struct phone *p, enum line_state state, unsigned long long now)
{
	if (state == LINE_DIAL_TONE && p->state != LINE_DIAL_TONE) {
		p->hot_since_ms = now;			/* the hotline's wait runs from the dial tone, and again from a fresh one after a flash */
		p->hot_armed = 1;
	}
	p->state = state;
	report_line(p, now);
}

/*
 * Ringing. The hook is read in every step of the loop and the line goes back to forward active at the first
 * off-hook read (the chip's ring-trip interrupt is not used); the modem client watches the call and
 * the ring stops when the call is no longer incoming. A ring test stops by itself after its time.
 */
static int ring_stop(struct chip *c, unsigned int ch, struct phone *p, const char *why, FILE *log)
{
	int err = ring_set(c, ch, 0, log);

	p->ringing = 0;
	p->ring_end_ms = 0;
	p->cid_wait_ms = 0;		/* a stop during a pre-ring caller id's gap: no delayed ring start */
	report_line(p, p->step_ms);
	fprintf(log, "  channel %u: ring stopped (%s)\n", ch, why);
	return err;
}

/* build and send one caller-id message for the number (the date and time are the router's clock) */
static int cid_deliver(struct chip *c, struct phone *p, unsigned int ch, const char *number, FILE *log);

/* build and send one caller-id message for the number (the date and time are the router's clock) */
static int cid_deliver(struct chip *c, struct phone *p, unsigned int ch, const char *number, FILE *log);

static int ring_start(struct chip *c, unsigned int ch, struct phone *p, unsigned long long now, FILE *log)
{
	int err;

	/* the European placement (the FSK message before the first burst): the identity goes out first, the ring
	 * voltage starts CID_PRERING_GAP_MS later so the message is not clipped; a lift during the send answers */
	if (p->cfg.cid_prering && p->ring_for == RING_FOR_CALL && p->call && p->call->cid >= CID_BELLCORE
	    && p->call->cid <= CID_ETSI && p->call->number) {
		char number[PHONE_NUMBER_MAX + 1];

		if (!p->call->number(p->call->ctx, number, sizeof(number)) && number[0]) {
			cid_deliver(c, p, ch, number, log);
			p->ringing = 1;
			report_line(p, now);
			p->ring_since_ms = p->ring_log_ms = now;
			p->burst_seen = 0;
			p->gap_since_ms = 0;
			p->last_lf = 0xff;
			p->cid_pending = 0;
			p->cid_wait_ms = now + CID_PRERING_GAP_MS;
			stamped(log, "  channel %u: the caller id goes out before the first ring; the ring starts %u ms later\n", ch, (unsigned)CID_PRERING_GAP_MS);
			return 0;
		}
	}
	err = ring_set(c, ch, 1, log);

	if (!err) {
		p->ringing = 1;
		report_line(p, now);
		p->ring_since_ms = p->ring_log_ms = now;	/* the loop's own step time: the ring's elapsed time is counted from the same clock reading as every later step */
		p->burst_seen = 0;
		p->gap_since_ms = 0;
		p->last_lf = 0xff;
		p->cid_pending = p->ring_for == RING_FOR_CALL && p->call && p->call->cid && p->call->number;
		p->cid_number[0] = 0;
		if (p->cid_pending && p->call->number(p->call->ctx, p->cid_number, sizeof(p->cid_number)))
			p->cid_number[0] = 0;
		fprintf(log, "  channel %u: ring started\n", ch);
	}
	return err;
}

static int hook_step(struct chip *c, unsigned int ch, struct phone *p, FILE *log);

/* the caller-id send asks between its units whether the handset was lifted: the hook is read there too, so a lift
 * during the send answers the call without waiting for the send to end (ring trip) */
struct lift_check {
	struct chip *c;
	unsigned int ch;
	struct phone *p;
	FILE *log;
};

static int lifted(void *ctx)
{
	struct lift_check *l = ctx;

	hook_step(l->c, l->ch, l->p, l->log);
	return l->p->hw.off_hook;
}

static int cid_deliver(struct chip *c, struct phone *p, unsigned int ch, const char *number, FILE *log)
{
	struct lift_check check = { c, ch, p, log };
	const struct cid_guard guard = { lifted, &check };

	struct cid_info info = { 0 };
	char name[CID_NAME_MAX + 1] = "";
	uint8_t msg[CID_MESSAGE_MAX];
	time_t now = time(NULL);
	struct tm tm;
	int len;

	localtime_r(&now, &tm);
	info.month = tm.tm_mon + 1;
	info.day = tm.tm_mday;
	info.hour = tm.tm_hour;
	info.minute = tm.tm_min;
	/* the handset shows the national form (ITU-T E.123), as an exchange's caller id does; the records keep the international one */
	char shown[PHONE_NUMBER_MAX + 1];

	if (number_national(number, p->cfg.region, shown, sizeof(shown)))
		snprintf(shown, sizeof(shown), "%s", number);
	if (p->call->cid == CID_DTMF || p->call->cid == CID_DTMF_TIME) {
		if (p->call->service_log)
			fprintf(log, "  channel %u: dtmf caller id\n", ch);
		else
			fprintf(log, "  channel %u: dtmf caller id, number '%s'\n", ch, shown);
		return cid_dtmf_send(c, p->board, ch, p->call->cid == CID_DTMF_TIME ? &tm : NULL, shown, &guard, log);
	}
	info.number = shown;
	if (number[0] && p->call->name && !p->call->name(p->call->ctx, number, name, sizeof(name)))
		info.name = name;
	len = cid_message(&info, msg, sizeof(msg));
	if (len <= 0)
		return len < 0 ? len : -EIO;
	return cid_send(c, p->board, ch, p->call->cid, msg, (size_t)len, &guard, log);
}

/* the network causes (3GPP TS 24.008, reported by AT+CEER) that mean the dialed number or the network itself is
 * in trouble -- they take the fast congestion cadence; a busy or an absent caller takes the slow busy one */
static int is_congestion(int cause)
{
	return cause == 1 || cause == 3 || cause == 28 || cause == 34 || cause == 38 || cause == 42;
}

/* one ring supervision step (every RING_POLL_MS): the supply log, the caller-id timing and the ring test's time. The
 * hook is read by the loop's own step, and the modem client watches the call. */
static int ring_tick(struct chip *c, unsigned int ch, struct phone *p, unsigned long long now, FILE *log)
{
	uint8_t lf;

	/* a caller id that went out before the first ring holds the ring voltage back for its silent gap */
	if (p->cid_wait_ms) {
		if ((long long)(now - p->cid_wait_ms) < 0)
			return 0;
		p->cid_wait_ms = 0;
		if (ring_set(c, ch, 1, log))
			return -EIO;
		p->ring_since_ms = p->ring_log_ms = now;
		p->last_lf = 0xff;
		fprintf(log, "  channel %u: ring started (the caller id went out first)\n", ch);
	}

	/* the supply readings: every RING_LOG_MS in a bring-up run, in a service only for the first seconds of each ring
	 * (what the ring supply does at the start decides when the first burst begins) */
	if (elapsed_ms(now, p->ring_log_ms) >= RING_LOG_MS && (!(p->call && p->call->service_log) || elapsed_ms(now, p->ring_since_ms) < RING_SUPPLY_LOG_MS)) {
		int32_t mv, expected, ua;

		p->ring_log_ms = now;
		if (!read_battery(c, ch, &mv, &expected) && !read_iloop(c, ch, &ua))
			fprintf(log, "  channel %u: ringing, %llu ms: battery %d mV, loop current %d uA, hook reg 0x%02x\n", ch,
				elapsed_ms(now, p->ring_since_ms), mv, ua, p->lcr);
	}
	/* the line feed register shows the burst phase; its changes are the ring's own timeline in the log */
	if (!chip_reg_read(c, ch, REG_LINEFEED, &lf)) {
		if (lf != p->last_lf) {
			stamped(log, "  channel %u: ringing, %llu ms: line feed register 0x%02x\n", ch, elapsed_ms(now, p->ring_since_ms), lf);
			p->last_lf = lf;
		}
		if ((lf & LF_RING_PHASE_MASK) == LF_RING_PHASE_ON) {
			p->burst_seen = 1;
			p->gap_since_ms = 0;
		} else if (p->burst_seen && !p->gap_since_ms) {
			p->gap_since_ms = now;
		}
	}
	if (p->cid_pending) {
		/* the silence after the first burst (or, if the register never shows a burst, the cadence's own timing) */
		if ((p->burst_seen && p->gap_since_ms && elapsed_ms(now, p->gap_since_ms) >= CID_GAP_MS) ||
		    (!p->burst_seen && elapsed_ms(now, p->ring_since_ms) >= RING_ON_MS + CID_GAP_MS)) {
			p->cid_pending = 0;
			fprintf(log, "  channel %u: caller id starts %llu ms after the ring started, after %llu ms of silence%s\n", ch,
				elapsed_ms(now, p->ring_since_ms), p->gap_since_ms ? elapsed_ms(now, p->gap_since_ms) : 0ull,
				p->burst_seen ? "" : " (no burst seen, the cadence's own timing)");
			/* a failed send is logged by cid_send; the ring goes on */
			(void)cid_deliver(c, p, ch, p->cid_number, log);
		}
	}
	if (p->ring_end_ms && now >= p->ring_end_ms)
		return ring_stop(c, ch, p, "the test time is over", log);
	return 0;
}

/* the time a task took; the slowest one since the last hook read is remembered with its name */
static void task_done(struct chip *c, struct phone *p, const char *what, unsigned long long t0)
{
	unsigned long long took = clock_ms(c) - t0;

	if (p && took > p->slow_ms) {
		p->slow_ms = took;
		p->slow_what = what;
	}
}

/*
 * The modem client. The loop never waits for the modem: a request is started, then moved on a little at every step
 * until it finishes (the modem answers within tens of milliseconds, or the request is given up on). One request is
 * open at a time; commands wait their turn in the order they were given; the call-list query and the release-cause
 * query are started only when no command waits.
 */
static int has_modem(const struct phone *p)
{
	return p->call && p->call->begin && p->call->pump && !p->port_lost;
}

static void modem_cmd(struct phone *p, const char *cmd, unsigned int ch, FILE *log)
{
	unsigned int k;

	if (!has_modem(p))
		return;
	if (!strcmp(cmd, "AT+CHUP") || !strcmp(cmd, "ATA")) {		/* one of these at a time is enough */
		if (p->mq_busy && p->mq_kind == CALL_REQ_CMD && !strcmp(p->mq_cmd, cmd))
			return;
		for (k = 0; k < p->mq_waiting; k++)
			if (!strcmp(p->mq_wait[k], cmd))
				return;
	}
	if (p->mq_waiting >= MODEM_QUEUE) {
		stamped(log, "  channel %u: the modem command queue is full, %.*s dropped\n", ch, strncmp(cmd, "ATD", 3) && strncmp(cmd, "AT+VTS", 6) && strncmp(cmd, "AT+CCFC", 7) ? (int)strlen(cmd) : 3, cmd);
		return;
	}
	snprintf(p->mq_wait[p->mq_waiting++], MODEM_CMD_MAX, "%s", cmd);
}

/*
 * The modem setup, at every modem open (the daemon's start, a port that came back): no echo of our own commands, numeric error
 * codes, the caller's number with the ring and the extended ring report (3GPP TS 27.007: +CLIP, +CRC, +CMEE), then the two audio levels
 * but only the ones that are set. The earpiece level (+CLVL) is written once the modem has said which range it accepts
 * (the test command), the receive gain (+QRXGAIN) as it is. The PCM mode is only read (AT+QDAI?), never written.
 */
static void modem_setup(struct phone *p, unsigned long long now)
{
	char cmd[MODEM_CMD_MAX];

	p->audio_slave = MQ_UNKNOWN;
	p->audio_warned = 0;
	p->info_next = 0;				/* the audio mode is the first question */
	p->info_due_ms = now + INFO_EVERY_MS / 4;
	p->clvl_pending = p->cfg.volume_set;
	if (!p->call || !p->call->setup)
		return;
	modem_cmd(p, "ATE0", p->ch, p->log);
	modem_cmd(p, "AT+CMEE=1", p->ch, p->log);
	modem_cmd(p, "AT+CLIP=1", p->ch, p->log);
	modem_cmd(p, "AT+CRC=1", p->ch, p->log);
	if (p->cfg.rx_gain_set) {
		snprintf(cmd, sizeof(cmd), "AT+QRXGAIN=%d", p->cfg.modem_rx_gain);
		modem_cmd(p, cmd, p->ch, p->log);
	}
	if (p->cfg.call_waiting) {
		modem_cmd(p, "AT+CCWA=1,1,1", p->ch, p->log);		/* the unsolicited report, the network service on, voice (TS 27.007 +CCWA) */
		p->ccwa_stage = 1;
	}
	if (p->cfg.clir_set) {
		/* the caller-id restriction for every call (TS 27.007 +CLIR); the idle questions read it back and the page shows the read-back */
		snprintf(cmd, sizeof(cmd), "AT+CLIR=%d", p->cfg.clir);
		modem_cmd(p, cmd, p->ch, p->log);
	}
}

/* a key pressed in a call goes to the network as a DTMF tone (3GPP TS 27.007 +VTS); a key that finds the queue full is dropped, said once per call and never with the digit */
static void send_key(struct phone *p, char digit, unsigned int ch, FILE *log)
{
	char cmd[16];

	if (!has_modem(p))
		return;
	if (p->mq_waiting >= MODEM_QUEUE) {
		if (!p->key_drop_logged)
			stamped(log, "  channel %u: a key was dropped: the modem is not keeping up\n", ch);
		p->key_drop_logged = 1;
		return;
	}
	snprintf(cmd, sizeof(cmd), "AT+VTS=%c", digit);
	modem_cmd(p, cmd, ch, log);
}

/* the dial session ended in the busy or congestion tone: it starts now, once the cause is known */
/* a call begins: it gets the next id (the number of an outgoing call is kept for the status reply only) */
static void record_close(struct phone *p, unsigned long long now);

static void call_begin(struct phone *p, enum phone_dir dir, unsigned long long now)
{
	p->out_listed = 0;
	record_close(p, now);		/* a call that was never listed by the modem (a refused dial, an abandoned attempt) ends here at the latest */
	p->rec_open = 1;
	p->rec_refused = p->rec_blocked = 0;
	p->rec_cause = 0;
	p->rec_start_ms = now;
	p->rec_gone_ms = 0;
	p->call_id = ++p->call_seq;
	p->key_drop_logged = 0;
	p->net_run = 0;
	p->net_tone = 0;
	p->dir = dir;
	p->call_since_ms = now;
	p->answered_ms = 0;
}

static void release_tone_start(struct phone *p, unsigned long long now)
{
	p->prog_start_ms = now;
	p->prog_on = -1;
}

/* the call is over: its record goes to the front end's call log, once */
static void record_close(struct phone *p, unsigned long long now)
{
	struct phone_call_record rec;

	if (!p->rec_open)
		return;
	p->rec_open = 0;
	if (!p->hooks.call_ended)
		return;
	memset(&rec, 0, sizeof(rec));
	rec.id = p->call_id;
	rec.dir = p->dir;
	snprintf(rec.number, sizeof(rec.number), "%s", p->peer);
	rec.start_ms = p->rec_start_ms;
	rec.answered_ms = p->answered_ms;
	rec.end_ms = p->rec_gone_ms ? p->rec_gone_ms : now;
	rec.cause = p->rec_cause;
	rec.result = phone_result_of(p->dir, p->answered_ms != 0, p->rec_refused, p->rec_blocked, rec.cause);
	p->hooks.call_ended(p->hooks.ctx, &rec);
}

/* the incoming call is over (not listed, or gone): nothing about it is kept */
static void call_over(struct phone *p)
{
	p->incoming = p->offered = p->refused = p->blocked = p->wait_promoted = p->recall = 0;
	p->recall_active = p->released_one = 0;
	p->tries = 0;
}

/* answer the offered incoming call (ATA, 3GPP TS 27.007); a refused answer is tried again at the next call list, a few times */
static void answer_incoming(struct phone *p, unsigned int ch, FILE *log)
{
	if (p->recall_active)
		return;			/* the survivor of a release-one is up already: the lift reconnects it at the next call list, no command */
	if (p->recall && p->nlegs != 1)
		return;			/* the release of the other call is still under way: the list says in a moment what is left */
	if (p->tries && elapsed_ms(p->step_ms, p->tried_ms) < TRY_SPACING_MS)
		return;			/* the modem has just been told; its call list needs a moment to show the answer */
	p->tries++;
	p->tried_ms = p->step_ms;
	modem_cmd(p, p->recall ? "AT+CHLD=2" : "ATA", ch, log);		/* a held call is taken back, not answered */
}

/*
 * What an incoming call finds. This is the one place that decides it (the add-on's routing, blocking and call waiting
 * of the later milestones attach here):
 *   handset down  -> offered: the phone rings, and the lift answers it (ring trip);
 *   handset up    -> the line is in use: the caller is released as busy and the phone does not ring. A line that is off-hook
 *                    is busy for a caller (the exchange does the same: ITU-T Q.31 / E.182 busy; 3GPP TS 22.030 user
 *                    determined user busy). AT+CHUP on a call that is only incoming rejects it (TS 27.007 +CHUP); which
 *                    cause the network shows the caller is TO-VERIFY on this modem. A second call during a call (call
 *                    waiting, TS 22.083 / AT+CHLD) is not decided here: the call list sees only the first voice call.
 * Never answered by itself: a handset that was already up when the call came does not answer it.
 */
static void incoming_policy(struct phone *p, int hook_off, FILE *log, unsigned int ch)
{
	if (p->wait_promoted) {
		/* the call that waited during a call: the user has been told of it and the blocking rules have let it through */
		p->offered = 1;
		return;
	}
	/* blocking first (the page's U19..U21): the caller is released as soon as the list shows the call, before any ring; an emergency number is exempt
	 * inside blocking_incoming. The number is not in the log. The way the network shows a withheld number is TO-VERIFY on the line. */
	if (p->cfg.block && blocking_incoming(p->cfg.block, p->peer)) {
		p->refused = p->blocked = p->rec_blocked = 1;
		stamped(log, "  channel %u: an incoming call was blocked\n", ch);
		return;
	}
	if (!hook_off) {
		p->offered = 1;
		return;
	}
	p->refused = 1;
	p->rec_refused = 1;
	stamped(log, "  channel %u: incoming call refused: the line is in use\n", ch);
}

/*
 * The legs of the call list: every listed voice call keeps its own call id from the moment it is first seen to the moment it leaves the list.
 * The first leg has the id of the line's call (the one the events and the call log speak about); another leg gets the next id. Only the table
 * is kept here; what a second leg means for the line is the call waiting and hold code (DESIGN.md, call waiting).
 */
static void legs_update(struct phone *p)
{
	struct call_leg now[CALL_LEGS_MAX];
	unsigned int n = p->call->legs ? p->call->legs(p->call->ctx, now, CALL_LEGS_MAX) : 0, k, j;
	struct leg_entry old[CALL_LEGS_MAX];
	unsigned int old_n = p->nlegs;

	memcpy(old, p->leg, sizeof(old));
	p->nlegs = n;
	for (k = 0; k < n; k++) {
		p->leg[k].idx = now[k].idx;
		p->leg[k].stat = now[k].stat;
		/* the network's form (national digits, or international with the plus) becomes the one comparison form here,
		 * so the records, the blocking rules and the phone book match a number however it was typed */
		if (number_from_network(now[k].number, p->cfg.region, p->leg[k].number, sizeof(p->leg[k].number)))
			snprintf(p->leg[k].number, sizeof(p->leg[k].number), "%s", now[k].number);
		p->leg[k].id = 0;
		for (j = 0; j < old_n; j++)
			if (old[j].idx == now[k].idx && old[j].id)
				p->leg[k].id = old[j].id;
	}
	for (k = 0; k < n; k++) {
		if (p->leg[k].id)
			continue;
		if (k == 0 && !p->call_id && p->leg[k].stat != LEG_WAITING)
			continue;			/* the line's own call gets its id when the call begins (legs_update runs again after that) */
		if (p->call_id && p->leg_primary != p->call_id && p->leg[k].stat != LEG_WAITING && p->leg[k].stat != LEG_HELD) {
			p->leg_primary = p->call_id;		/* the line's own call id goes to the first leg once */
			p->leg[k].id = p->call_id;
		} else {
			p->leg[k].id = ++p->call_seq;
		}
	}
}

/* the waiting call's record goes to the call log */
static void wait_record_close(struct phone *p, unsigned long long now)
{
	struct wait_call *w = &p->wait;
	struct phone_call_record rec;

	if (!w->open)
		return;
	w->open = 0;
	if (!p->hooks.call_ended)
		return;
	memset(&rec, 0, sizeof(rec));
	rec.id = w->id;
	rec.dir = w->dir;
	snprintf(rec.number, sizeof(rec.number), "%s", w->number);
	rec.start_ms = w->start_ms;
	rec.answered_ms = w->answered_ms;
	rec.end_ms = now;
	rec.result = phone_result_of(w->dir, w->answered_ms != 0, w->refused, w->blocked, 0);
	p->hooks.call_ended(p->hooks.ctx, &rec);
}

/* the line's own call ended while the waiting one went on: the waiting call becomes the line's call (its record continues as the line's record) */
static void wait_promote(struct phone *p, unsigned long long now, int stat)
{
	struct wait_call *w = &p->wait;

	p->rec_gone_ms = now;
	record_close(p, now);
	p->call_id = w->id;
	p->dir = w->dir;
	snprintf(p->peer, sizeof(p->peer), "%s", w->number);
	p->answered_ms = w->answered_ms;
	p->call_since_ms = now;
	p->rec_open = 1;
	p->rec_refused = p->rec_blocked = 0;
	p->rec_cause = 0;
	p->rec_start_ms = w->start_ms;
	p->rec_gone_ms = 0;
	p->leg_primary = w->id;
	p->wait_promoted = stat == LEG_INCOMING || stat == LEG_WAITING;
	w->open = 0;
}

/*
 * The waiting call (DESIGN.md, call waiting): a second incoming call while a call is going on, listed with stat 5. It is offered (the waiting tone,
 * the flash keys) unless call waiting is off or the blocking rules refuse it; a refused one is released with AT+CHLD=0 (AT+CHUP would end the active
 * call too). It has its own call id and call log record.
 */
static void waiting_update(struct phone *p, unsigned long long now)
{
	struct wait_call *w = &p->wait;
	int listed = -1, fresh = -1, primary = 0;
	unsigned int k;

	for (k = 0; k < p->nlegs; k++) {
		if (p->call_id && p->leg[k].id == p->call_id)
			primary = 1;
		if (w->open && p->leg[k].id == w->id)
			listed = (int)k;
		else if (p->leg[k].stat == LEG_WAITING && fresh < 0)
			fresh = (int)k;
	}
	if (w->open && listed < 0)
		wait_record_close(p, now);			/* the caller gave up, or the call was released */
	if (w->open && p->leg[listed].stat == LEG_ACTIVE && !w->answered_ms)
		w->answered_ms = now;
	p->primary_listed = primary;
	if (primary && p->state == LINE_SETUP)
		p->out_listed = 1;			/* the dialled call has been listed: a list without it later means it did not come through */
	if (w->open && !primary && p->leg[listed].stat != LEG_HELD && p->leg[listed].stat != LEG_WAITING) {
		wait_promote(p, now, p->leg[listed].stat);	/* the line's own call is gone and this one is going on: it is the line's call now */
		return;
	}
	if (w->open && fresh >= 0 && p->leg[fresh].id != p->extra_id) {
		/* a further incoming call while two calls are in use: nothing can be offered, the network keeps ringing the caller; the call log shows it as missed */
		struct phone_call_record rec;

		p->extra_id = p->leg[fresh].id;
		stamped(p->log, "  channel %u: a further incoming call while two calls are in use: left to the network\n", p->ch);
		if (p->hooks.call_ended) {
			memset(&rec, 0, sizeof(rec));
			rec.id = p->extra_id;
			rec.dir = PHONE_DIR_IN;
			snprintf(rec.number, sizeof(rec.number), "%s", p->leg[fresh].number);
			rec.start_ms = rec.end_ms = now;
			rec.result = PHONE_RES_MISSED;
			p->hooks.call_ended(p->hooks.ctx, &rec);
		}
	}
	if (!w->open && fresh >= 0) {
		memset(w, 0, sizeof(*w));
		w->open = 1;
		w->dir = PHONE_DIR_IN;
		w->id = p->leg[fresh].id;
		snprintf(w->number, sizeof(w->number), "%s", p->leg[fresh].number);
		w->start_ms = now;
		if (p->cfg.block && blocking_incoming(p->cfg.block, w->number)) {
			w->refused = w->blocked = 1;
			stamped(p->log, "  channel %u: a waiting call was blocked\n", p->ch);
		} else if (!p->cfg.call_waiting) {
			w->refused = 1;
			stamped(p->log, "  channel %u: a waiting call was refused: call waiting is off\n", p->ch);
		} else {
			stamped(p->log, "  channel %u: a call is waiting\n", p->ch);
		}
		listed = fresh;
	}
	if (w->open && w->refused && p->leg[listed].stat == LEG_WAITING && w->tries < CALL_TRY_MAX &&
	    (!w->tries || elapsed_ms(now, w->tried_ms) >= TRY_SPACING_MS)) {
		w->tries++;
		w->tried_ms = now;
		modem_cmd(p, "AT+CHLD=0", p->ch, p->log);
	}
}

/* the tracked call is listed as waiting (stat 5) */
static int wait_listed_waiting(const struct phone *p)
{
	unsigned int k;

	for (k = 0; k < p->nlegs; k++)
		if (p->wait.open && p->leg[k].id == p->wait.id)
			return p->leg[k].stat == LEG_WAITING;
	return 0;
}

/* the waiting tone: a short burst now and then on the tone oscillators, over the voice path (cadence and the oscillator's effect on speech are TO-VERIFY) */
static void waiting_tone(struct phone *p, unsigned long long now)
{
	const struct wait_call *w = &p->wait;
	const int live = w->open && !w->refused && !w->answered_ms && p->state == LINE_CALL && wait_listed_waiting(p);
	const int want = live && elapsed_ms(now, w->start_ms) % WAIT_TONE_PERIOD_MS < WAIT_TONE_ON_MS;

	if (want == p->wait_tone_on || now < p->tone_retry_ms)
		return;
	if (want ? osc_start_once(p->c, p->ch, p->log) : osc_stop_once(p->c, p->ch, p->log))
		p->tone_retry_ms = now + TONE_RETRY_MS;
	else
		p->wait_tone_on = want;
	if (want && p->wait_tone_on && !p->wait.toned) {
		p->wait.toned = 1;
		stamped(p->log, "  channel %u: the waiting tone sounds\n", p->ch);
	}
}

/* is there a waiting call the user can act on? */
static int wait_pending(const struct phone *p)
{
	return p->wait.open && !p->wait.refused && !p->wait.answered_ms && p->state == LINE_CALL && wait_listed_waiting(p);
}

static unsigned int legs_with(const struct phone *p, int stat)
{
	unsigned int k, n = 0;

	for (k = 0; k < p->nlegs; k++)
		n += p->leg[k].stat == stat;
	return n;
}

/*
 * What a flash can do now (DESIGN.md, call waiting): take the waiting call, put the one call on hold (and get the dial tone for a second call),
 * swap the active and the held call, or take the held call back when nothing is active. It needs the call list's legs.
 */
enum flash_use { FLASH_NOTHING, FLASH_TAKE_WAITING, FLASH_HOLD, FLASH_SWAP, FLASH_RESUME };

static enum flash_use flash_use(const struct phone *p)
{
	const unsigned int active = legs_with(p, LEG_ACTIVE), held = legs_with(p, LEG_HELD);

	if (!p->call || !p->call->legs || !has_modem(p))
		return FLASH_NOTHING;
	if (wait_pending(p))
		return FLASH_TAKE_WAITING;
	if (p->state == LINE_CALL && active == 1 && held == 1 && p->nlegs == 2)
		return FLASH_SWAP;
	if (p->state == LINE_CALL && active == 1 && p->nlegs == 1)
		return FLASH_HOLD;
	if (p->state == LINE_DIAL_TONE && held >= 1 && !active)
		return FLASH_RESUME;
	return FLASH_NOTHING;
}

/* a flash with no key after it (the window ended): the waiting call is taken, or the call held, swapped or taken back, all with AT+CHLD=2 (TS 27.007) */
static void flash_alone(struct phone *p)
{
	static const char *const what[] = { "", "the waiting call is taken, the call is held", "the call is held", "the calls are swapped", "the held call is taken back" };
	const enum flash_use use = flash_use(p);

	p->flash_open = 0;
	if (use == FLASH_NOTHING)
		return;
	stamped(p->log, "  channel %u: flash: %s\n", p->ch, what[use]);
	modem_cmd(p, "AT+CHLD=2", p->ch, p->log);
}

/*
 * The key after a flash, by what the flash can do (TS 22.030 in-call codes): 1 releases the active call and takes the other, 2 holds the active call
 * and takes the other (or swaps, or takes the held call back), 0 refuses the waiting call or releases the held one, 3 is the join (three-way, a later step;
 * the key is used up when it is not allowed). A key that is no code for the state goes on to the network as an ordinary key.
 */
static int flash_code(struct phone *p, char digit)
{
	const enum flash_use use = flash_use(p);
	const char *cmd = NULL;

	p->flash_open = 0;
	if (use == FLASH_NOTHING || (digit < '0' || digit > '3'))
		return 0;
	if (digit == '3') {
		/* the join (TS 22.030 "3": add the held call to the conversation, AT+CHLD=3): only with two calls, and only when the option allows it */
		if (use == FLASH_SWAP && p->cfg.conference) {
			stamped(p->log, "  channel %u: flash code: the two calls are joined\n", p->ch);
			modem_cmd(p, "AT+CHLD=3", p->ch, p->log);
		} else if (use == FLASH_SWAP) {
			stamped(p->log, "  channel %u: flash code: three-way calling is off\n", p->ch);
		}
		return use == FLASH_TAKE_WAITING || use == FLASH_SWAP;
	}
	if (use == FLASH_HOLD || use == FLASH_RESUME)
		cmd = digit == '2' ? "AT+CHLD=2" : NULL;
	else if (digit == '1')
		cmd = "AT+CHLD=1";
	else if (digit == '2')
		cmd = "AT+CHLD=2";
	else if (use == FLASH_TAKE_WAITING || use == FLASH_SWAP)
		cmd = "AT+CHLD=0";
	if (!cmd)
		return 0;
	stamped(p->log, "  channel %u: flash code: %s\n", p->ch, digit == '1' ? "the call ends, the other call is taken" :
		digit == '2' ? "the call is held, the other call is taken" : "the waiting call is refused or the held call released");
	if (digit == '0' && use == FLASH_TAKE_WAITING) {
		p->wait.refused = 1;
		p->wait.tries = 0;
	}
	modem_cmd(p, cmd, p->ch, p->log);
	return 1;
}

/* the line's own call goes to the second record slot (its call goes on hold): the line has no current call until another begins or this one is taken back */
static void stash_line_call(struct phone *p)
{
	struct wait_call *w = &p->wait;

	if (w->open || !p->call_id)
		return;
	memset(w, 0, sizeof(*w));
	w->open = 1;
	w->dir = p->dir;
	w->id = p->call_id;
	snprintf(w->number, sizeof(w->number), "%s", p->peer);
	w->start_ms = p->rec_start_ms;
	w->answered_ms = p->answered_ms;
	p->rec_open = 0;
	p->call_id = 0;
	p->dir = PHONE_DIR_NONE;
	p->peer[0] = 0;
	p->answered_ms = 0;
	p->call_since_ms = 0;
}

/* the call list answered: the call states drive the tones, the ring and the answer (3GPP TS 27.007 AT+CLCC) */
static void present_dial_tone(struct chip *c, unsigned int ch, struct phone *p, uint8_t lcr, FILE *log);

static int on_call_state(struct chip *c, unsigned int ch, struct phone *p, int st, unsigned long long now, FILE *log)
{
	int hook_off = p->hw.off_hook, err = 0;

	if (st == CALL_INCOMING && p->state == LINE_CALL)
		st = CALL_GONE;				/* the call in progress ended and another one is presented: the release tone first */
	if (st == CALL_HELD && p->state == LINE_CALL) {
		/* the call went on hold (or the other call ended): only a held call is left, and the caller gets a dial tone for a second call */
		if (p->primary_listed)
			stash_line_call(p);
		else if (p->call_id) {
			p->rec_gone_ms = now;
			record_close(p, now);		/* the line's own call ended while the held one stayed */
			p->call_id = 0;
			p->dir = PHONE_DIR_NONE;
			p->peer[0] = 0;
			p->answered_ms = p->call_since_ms = 0;
		}
		(void)osc_stop_once(c, ch, log);
		set_line(p, LINE_IDLE, now);
		present_dial_tone(c, ch, p, p->lcr, log);
	} else if (st == CALL_HELD && p->state == LINE_SETUP && !p->primary_listed && p->out_listed) {
		/* the second call did not come through: the release tone for it, and the held call is taken back */
		p->gone_from = p->state;
		(void)osc_stop_once(c, ch, log);
		p->cause = 0;
		p->cause_pending = 1;
		p->cause_want = 1;
		p->rec_defer = 1;
		p->rec_gone_ms = now;
		release_tone_start(p, now);
		set_line(p, LINE_RELEASE, now);
		modem_cmd(p, "AT+CHLD=2", ch, log);
	}
	if ((st == CALL_HELD || (st == CALL_ACTIVE && (p->released_one || p->recall_active) && p->nlegs == 1)) && p->state == LINE_IDLE) {
		/* a call is left and nobody is at the phone: it rings back until it is taken or its remote ends. The network
		 * retrieves the held call after the release of the other (it is listed active again), so a lift takes a held
		 * call back with AT+CHLD=2 and only reconnects the audio of a survivor */
		if (!hook_off && !p->recall && p->ring_ready && p->call->ring && !p->ring_end_ms && !ring_start(c, ch, p, now, log)) {
			p->recall = p->incoming = p->offered = 1;
			p->tries = 0;
			stamped(log, "  channel %u: a held call rings back\n", ch);
		}
		if (p->recall)
			p->recall_active = st == CALL_ACTIVE && p->nlegs == 1;	/* up again, alone: the lift reconnects; held: the lift takes it back */
		if (hook_off && p->recall && p->tries < CALL_TRY_MAX) {
			p->released_one = 0;
			if (p->recall_active) {
				if (p->ringing)
					(void)ring_stop(c, ch, p, "the handset was lifted", log);
				set_line(p, LINE_CALL, now);
				p->recall = p->recall_active = 0;
			} else
				answer_incoming(p, ch, log);
		}
	}
	if (st == CALL_INCOMING && !p->call_id)
		call_begin(p, PHONE_DIR_IN, now);
	if (p->dir == PHONE_DIR_IN && st != CALL_NONE && !p->peer[0] && p->call && p->call->number) {
		char raw[sizeof(p->peer)];

		if (p->call->number(p->call->ctx, raw, sizeof(raw)) || number_from_network(raw, p->cfg.region, p->peer, sizeof(p->peer)))
			p->peer[0] = 0;
	}
	if (st == CALL_GONE && !p->rec_gone_ms)
		p->rec_gone_ms = now;
	if (st == CALL_NONE && p->call_seen != CALL_NONE)
		record_close(p, now);
	if (st != p->call_seen) {
		p->call_seen = st;
		p->call_since_ms = now;
		if (st == CALL_ACTIVE && !p->answered_ms) {
			p->answered_ms = now;
			/* under which radio conditions the call was answered (the last idle answers of the modem, at most a few seconds old);
			 * a voice complaint is looked at against this line */
			char level[24] = "unknown";

			if (p->rsrp_dbm != MQ_UNKNOWN)
				snprintf(level, sizeof(level), "rsrp %d dBm", p->rsrp_dbm);
			stamped(log, "  channel %u: call answered on %s %s, signal %s, ims %s\n", ch, p->network[0] ? p->network : "unknown network", p->band, level,
				p->ims == 1 ? "registered" : p->ims == 0 ? "not registered" : "unknown");
		}
		emit(p, PHONE_EV_CALL, now);
		if (st == CALL_NONE) {
			p->call_id = 0;
			p->dir = PHONE_DIR_NONE;
			p->call_since_ms = p->answered_ms = 0;
			p->peer[0] = 0;
		}
	}
	if (st == CALL_NONE || st == CALL_GONE)
		p->ending = 0;		/* the call list has caught up with our hang-up */
	if (p->ringing && !p->ring_end_ms && st != CALL_INCOMING && !(p->recall && st != CALL_NONE && st != CALL_GONE))
		(void)ring_stop(c, ch, p, "the call is no longer incoming", log);
	if (st == CALL_ACTIVE && p->state != LINE_CALL && p->hw.cleared && !p->hw.off_hook && !p->ending
	    && !(p->nlegs > 1 && (legs_with(p, LEG_HELD) || legs_with(p, LEG_WAITING)))
	    && !p->released_one && !p->recall) {
		/* the call connected after the handset went down (the answer was sent, then the caller hung up): end it.
		 * Not while another call is held or waiting: then the active leg is being released on its own (AT+CHLD=1x
		 * from the hang-up) and stays listed until the modem has done it; AT+CHUP would end them all */
		char legs[96];

		legs_text(p, legs, sizeof(legs));
		p->ending = 1;
		modem_cmd(p, "AT+CHUP", ch, log);
		stamped(log, "  channel %u: the call connected with the handset down (the calls %s): hang-up sent\n", ch, legs);
	} else if (st == CALL_ACTIVE && p->state != LINE_CALL && !p->ending && p->hw.off_hook) {
		/* somebody is at the phone and the call is connected (with the handset down the active leg is the one
		 * being released beside a held or waiting call: the line stays idle until the list has caught up) */
		if (p->state == LINE_DIAL_TONE && !osc_stop_once(c, ch, log))
			fprintf(log, "  channel %u: dial tone stopped (the call is connected)\n", ch);
		if (p->state == LINE_SETUP && !osc_stop_once(c, ch, log))
			stamped(log, "  channel %u: the remote answered\n", ch);
		if (p->state == LINE_RELEASE)
			(void)osc_stop_once(c, ch, log);		/* the held call was taken back after a failed second call */
		set_line(p, LINE_CALL, now);
		if (p->dir == PHONE_DIR_IN)
			p->calls_in++;
		call_over(p);
	} else if (st == CALL_DIALING || st == CALL_ALERTING) {
		if (p->state == LINE_SETUP && !p->alerting) {
			p->alerting = st == CALL_ALERTING;
			if (p->alerting)
				release_tone_start(p, now);	/* the ringing tone's on-window starts at the alerting */
			if (p->alerting || !p->setup_said)
				stamped(log, "  channel %u: the network %s\n", ch,
					p->alerting ? "is ringing the remote" : "is setting the call up");
			p->setup_said = 1;
		}
	} else if (st == CALL_GONE) {
		if (p->state == LINE_SETUP || p->state == LINE_CALL) {
			/* the network released the call: an outgoing attempt before the answer, or the remote ended a
			 * connected call while the caller is still on the line -- the release tone says the line is free
			 * again (the busy or the congestion tone, by the network's cause, asked for now) */
			p->gone_from = p->state;
			(void)osc_stop_once(c, ch, log);	/* a tone window may be open */
			p->cause = 0;
			p->cause_pending = 1;
			p->cause_want = 1;
			release_tone_start(p, now);
			set_line(p, LINE_RELEASE, now);
		} else {
			call_over(p);
		}
	} else if (st == CALL_INCOMING) {
		if (!p->incoming) {
			p->incoming = 1;
			p->tries = 0;
			incoming_policy(p, hook_off, log, ch);
		}
		if (p->refused) {
			/* the network still lists the refused call: the release is sent again at the next list, a few times */
			if (p->tries < CALL_TRY_MAX && (!p->tries || elapsed_ms(now, p->tried_ms) >= TRY_SPACING_MS)) {
				p->tries++;
				p->tried_ms = now;
				modem_cmd(p, "AT+CHUP", ch, log);
			}
		} else {
			if (!hook_off && !p->ringing && p->ring_ready && p->call->ring && !p->ring_end_ms && p->state == LINE_IDLE && !(err = ring_start(c, ch, p, now, log)))
				stamped(log, "  channel %u: the phone rings for the incoming call\n", ch);
			/* a handset lifted after the call was offered answers it; one who is dialling keeps the line */
			if (hook_off && p->offered && p->state <= LINE_DIAL_TONE && p->tries < CALL_TRY_MAX)
				answer_incoming(p, ch, log);
		}
	} else if (st == CALL_NONE) {
		call_over(p);
	}
	return err;
}

/*
 * Call forwarding at the network (3GPP TS 27.007 +CCFC, TS 22.082): when the settings change (or are new) and the line is idle with the handset down,
 * everything the network holds for the line is erased, the new service is registered, and the query of its reason is the read-back the page shows.
 * A refusal (a network without the supplementary-service setup answers +CME ERROR) is kept as the reason and not retried. Numbers never reach the log.
 */
static void fwd_error(struct phone *p, int res, const char *why)
{
	if (res == -ETIMEDOUT)
		snprintf(p->fwd_net.error, sizeof(p->fwd_net.error), "no answer from the network");
	else
		snprintf(p->fwd_net.error, sizeof(p->fwd_net.error), "%s", why && *why ? why : "the modem refused");
}

static void fwd_start(struct phone *p)
{
	p->fwd_want = 0;
	p->fwd_job = p->cfg.forward;
	memset(&p->fwd_net, 0, sizeof(p->fwd_net));
	p->fwd_net.network = PHONE_UNKNOWN;
	p->fwd_net.applying = 1;
	p->fwd_stage = FWD_STAGE_ERASING;
	modem_cmd(p, forward_erase_cmd(), p->ch, p->log);
	stamped(p->log, "  channel %u: call forwarding: being set at the network\n", p->ch);
}

static void fwd_cmd_done(struct phone *p, int res, const char *why)
{
	char cmd[MODEM_CMD_MAX];

	if (p->fwd_stage == FWD_STAGE_ERASING) {
		if (res)
			stamped(p->log, "  channel %u: call forwarding: the erase was refused (%d)\n", p->ch, res);
		if (res && !p->fwd_job.enabled)
			fwd_error(p, res, why);
		if (p->fwd_job.enabled && forward_register_cmd(&p->fwd_job, cmd, sizeof(cmd)) == 0) {
			modem_cmd(p, cmd, p->ch, p->log);
			p->fwd_stage = FWD_STAGE_REGISTERING;
			return;
		}
		if (p->fwd_job.enabled)
			snprintf(p->fwd_net.error, sizeof(p->fwd_net.error), "the forwarding number or delay is not valid");
		p->fwd_stage = FWD_STAGE_QUERY_DUE;
	} else if (p->fwd_stage == FWD_STAGE_REGISTERING) {
		if (res) {
			stamped(p->log, "  channel %u: call forwarding: the network refused the setting (%d)\n", p->ch, res);
			fwd_error(p, res, why);
		}
		p->fwd_stage = FWD_STAGE_QUERY_DUE;
	}
}

static void fwd_query_done(struct phone *p, int res, const char *text)
{
	int status = -1;

	p->fwd_net.number[0] = 0;
	p->fwd_net.delay_s = 0;
	if (res == 0) {
		forward_parse(text, &status, p->fwd_net.number, sizeof(p->fwd_net.number), &p->fwd_net.delay_s);
	} else if (!p->fwd_net.error[0]) {
		fwd_error(p, res, "");
	}
	p->fwd_net.network = status < 0 ? PHONE_UNKNOWN : status;
	p->fwd_net.applying = 0;
	p->fwd_net.done = 1;
	p->fwd_stage = FWD_STAGE_IDLE;
	stamped(p->log, "  channel %u: call forwarding: the network reports it %s\n", p->ch, status < 0 ? "unknown" : status ? "on" : "off");
}

/* a command finished */
static void on_cmd_done(struct chip *c, unsigned int ch, struct phone *p, int res, unsigned long long now, FILE *log)
{
	const char *why = res && p->call->reply ? p->call->reply(p->call->ctx) : "";

	if (!strncmp(p->mq_cmd, "ATD", 3)) {
		if (res && p->state == LINE_SETUP) {
			/* a dial the network refuses takes the release tone */
			stamped(log, "  channel %u: the network refused the dial (%d: %s), the busy tone\n", ch, res, why);
			p->cause = 0;
			p->cause_pending = 0;
			(void)osc_stop_once(c, ch, log);
			release_tone_start(p, now);
			set_line(p, LINE_RELEASE, now);
			p->rec_cause = 0;
			p->rec_gone_ms = now;
			record_close(p, now);		/* the call never reached the call list */
		}
	} else if (!strcmp(p->mq_cmd, "ATA")) {
		if (!res)
			stamped(log, "  channel %u: answer sent (try %u)\n", ch, p->tries);
		else
			stamped(log, "  channel %u: the answer was refused (try %u; %d: %s)\n", ch, p->tries, res, why);
	} else if (!strcmp(p->mq_cmd, "AT+CHUP")) {
		if (!res) {
			stamped(log, "  channel %u: hang-up sent\n", ch);
		} else {
			/* a refused hang-up is not lost: the call list still shows the call, and the next query sends it again */
			p->ending = 0;
			stamped(log, "  channel %u: the hang-up was refused (%d: %s); it is sent again at the next call list\n", ch, res, why);
		}
	} else if (!strncmp(p->mq_cmd, "AT+QDAI=", 8) && p->hd_stage == HD_QDAI_WRITE) {
		p->hd_stage = HD_CFUN;
		if (res)
			stamped(log, "  channel %u: the PCM rate write was refused (%d: %s): the modem is restarted as it is\n", ch, res, why);
		modem_cmd(p, "AT+CFUN=1,1", ch, log);	/* the reply never comes: the modem restarts and the port is found again */
		p->hd_want = 0;
		stamped(log, "  channel %u: the modem restarts for the new PCM mode\n", ch);
	} else if (!strncmp(p->mq_cmd, "AT+CHLD=1", 9) && p->abandon_cmd) {
		p->abandon_cmd = 0;
		if (res) {
			stamped(log, "  channel %u: the modem refused to release one call: all calls are hung up instead\n", ch);
			modem_cmd(p, "AT+CHUP", ch, log);
		}
	} else if (!strncmp(p->mq_cmd, "AT+CCFC=", 8)) {
		fwd_cmd_done(p, res, why);
	} else if (res) {
		stamped(log, "  channel %u: the modem refused %.*s (%d: %s)\n", ch, (int)strcspn(p->mq_cmd, "="), p->mq_cmd, res, why);	/* a key's digit is not logged */
	}
}

/* the release cause answered (or did not): the release tone starts with the cadence it picks */
static void on_cause_done(unsigned int ch, struct phone *p, int res, unsigned long long now, FILE *log)
{
	if (p->state != LINE_RELEASE || !p->cause_pending)
		return;				/* the caller has hung up meanwhile */
	p->cause = res > 0 ? res : 0;		/* no answer: the cause stays unknown, the busy tone plays */
	p->rec_cause = p->cause;
	p->cause_pending = 0;
	if (p->rec_defer) {
		/* the failed second call: its record is made now that the cause is known, and the line has no current call until the held one is taken back */
		p->rec_defer = 0;
		record_close(p, now);
		p->call_id = 0;
		p->dir = PHONE_DIR_NONE;
		p->peer[0] = 0;
		p->answered_ms = p->call_since_ms = 0;
	}
	release_tone_start(p, now);
	stamped(log, "  channel %u: %s (cause %d), the %s tone\n", ch,
		p->gone_from == LINE_SETUP ? "released before the answer" : "the remote ended the call",
		p->cause, is_congestion(p->cause) ? "congestion" : "busy");
}

static void modem_lost(struct chip *c, unsigned int ch, struct phone *p, unsigned long long now, FILE *log)
{
	const enum line_state was = p->state;

	p->port_lost = 1;
	p->cause_want = 0;
	p->hd_stage = HD_IDLE;
	p->hd_want = 0;			/* an HD exchange in flight died with the port: the next settings change starts it over */
	if (p->fwd_stage != FWD_STAGE_IDLE) {
		p->fwd_stage = FWD_STAGE_IDLE;
		p->fwd_net.applying = 0;
		snprintf(p->fwd_net.error, sizeof(p->fwd_net.error), "the modem was lost");
		p->fwd_want = 1;		/* once more when the port is back */
	}
	p->mq_busy = p->mq_waiting = 0;
	stamped(log, "  channel %u: the modem port was lost: no more calls are controlled until the port is open again\n", ch);
	if (p->ringing && !p->ring_end_ms)
		(void)ring_stop(c, ch, p, "the modem is gone", log);
	if (was != LINE_CALL && was != LINE_SETUP)
		return;
	/* a call nobody can control any more is over for the caller: the release tone says the line is free */
	(void)osc_stop_once(c, ch, log);
	p->gone_from = was;
	p->cause = p->cause_pending = 0;
	release_tone_start(p, now);
	set_line(p, LINE_RELEASE, now);
}

/* how soon the call list is asked for again: fast while the call changes (set-up, our hang-up, just answered), else once a second */
static unsigned int poll_ms(const struct phone *p, unsigned long long now)
{
	if (p->state == LINE_SETUP || p->ending || (p->answered_ms && elapsed_ms(now, p->answered_ms) < FAST_AFTER_ANSWER_MS))
		return CALL_FAST_POLL_MS;
	return CALL_POLL_MS;
}

/* the idle questions: one every INFO_EVERY_MS while the line is quiet (handset down, nothing ringing, no call); never during a call or its set-up */
static int info_wanted(const struct phone *p, unsigned long long now)
{
	const int quiet = (p->state == LINE_IDLE && !p->hw.off_hook) || p->state == LINE_NO_SERVICE;	/* no service: the registration is asked on to hear it come back */

	return p->call->body && now >= p->info_due_ms && quiet && !p->ringing && !p->incoming &&
	       p->call_seen == CALL_NONE && !p->mq_waiting && !p->cause_want;
}

/* the earpiece level, once the modem has said which levels it has */
static void clvl_range_known(struct phone *p, const char *text)
{
	int low, high;
	char cmd[MODEM_CMD_MAX];

	p->clvl_pending = 0;
	if (!modemq_range(text, "+CLVL:", &low, &high)) {
		stamped(p->log, "  channel %u: the modem's earpiece range is not known: the level is not written\n", p->ch);
		return;
	}
	if (p->cfg.earpiece_volume < low || p->cfg.earpiece_volume > high) {
		stamped(p->log, "  channel %u: the earpiece level %d is outside the modem's range %d-%d: not written\n", p->ch, p->cfg.earpiece_volume, low, high);
		return;
	}
	snprintf(cmd, sizeof(cmd), "AT+CLVL=%d", p->cfg.earpiece_volume);
	modem_cmd(p, cmd, p->ch, p->log);
}

/* the question number of a command */
static unsigned int info_index(const char *cmd)
{
	unsigned int k;

	for (k = 0; k < modemq_count; k++)
		if (!strcmp(modemq_command[k], cmd))
			return k;
	return 0;
}

/* the question after this one; the range question is asked once per modem setup, only when a level is set */
static void info_advance(struct phone *p)
{
	do
		p->info_next = (p->info_next + 1) % modemq_count;
	while (!strcmp(modemq_command[p->info_next], "AT+CLVL=?") && !p->clvl_pending);
}

static void keep_known(int *value, int parsed)
{
	if (parsed != MQ_UNKNOWN)
		*value = parsed;
}

/* a question was answered: the value is kept for the status and for the log line of the next answered call */
static void on_info_done(struct phone *p, int res, unsigned long long now)
{
	const char *cmd = modemq_command[p->info_next];
	const char *text = p->call->body ? p->call->body(p->call->ctx) : "";

	p->info_due_ms = now + INFO_EVERY_MS;
	info_advance(p);
	if (p->state == LINE_NO_SERVICE)
		p->info_next = info_index("AT+CEREG?");		/* only the registration matters until service is back */
	if (res || !text)
		return;				/* no answer: the old value stays, the next round asks again */
	/* a reply of another shape is no verdict: the value that was known stays */
	if (!strcmp(cmd, "AT+QDAI?")) {
		keep_known(&p->audio_slave, modemq_audio_slave(text));
		if (p->audio_slave == 0 && !p->audio_warned) {
			p->audio_warned = 1;
			stamped(p->log, "  channel %u: warning: the modem is not in slave mode (AT+QDAI?): the voice path cannot work; it is not changed here\n", p->ch);
		}
	} else if (!strcmp(cmd, "AT+CLVL=?")) {
		clvl_range_known(p, text);
	} else if (!strcmp(cmd, "AT+CPIN?")) {
		keep_known(&p->sim_ready, modemq_sim_ready(text));
	} else if (!strcmp(cmd, "AT+QDAI?")) {
		char head[8];

		keep_known(&p->audio_slave, modemq_audio_slave(text));
		if (modemq_qdai_head(text, head, sizeof(head)) && !p->qdai_head[0])
			memcpy(p->qdai_head, head, sizeof(p->qdai_head));
	} else if (!strcmp(cmd, "AT+CEREG?")) {
		keep_known(&p->registered, modemq_registered(text));
	} else if (!strncmp(cmd, "AT+QCFG", 7)) {
		keep_known(&p->ims, modemq_ims(text));
	} else if (!strcmp(cmd, "AT+QCSQ")) {
		keep_known(&p->rsrp_dbm, modemq_rsrp_dbm(text));
	} else if (!strcmp(cmd, "AT+CLIR?")) {
		int status;
		int n = modemq_clir(text, &status);

		keep_known(&p->clir, n);
		if (n != MQ_UNKNOWN)
			p->clir_status = status;
	} else if (!strcmp(cmd, "AT+QNWINFO")) {
		char net[MQ_NETWORK_MAX], band[MQ_NETWORK_MAX];

		modemq_network(text, net, sizeof(net), band, sizeof(band));
		if (net[0]) {
			memcpy(p->network, net, sizeof(net));
			memcpy(p->band, band, sizeof(band));
		}
	}
}

/*
 * The modem's audio level while a call is being set up, read from the kernel bridge's window counters (at most every 100 ms, only in
 * the set-up). Two uses: the measuring option logs it (no number, no digit), and ringback "auto" decides from it whether the network
 * plays its own ringback tone or a caller's ringback tune: once NET_AUDIO_WINDOWS windows in a row are above NET_AUDIO_PEAK the local
 * ringing tone stops for the rest of the call, because two tones on the line are not what the caller should hear. The industry rule
 * (RFC 3960 early media; the local tone only until early media arrives); the numbers are from the line.
 */
static void level_watch(struct phone *p, unsigned long long now)
{
	unsigned int peak, windows;

	if (elapsed_ms(now, p->level_ms) < 100)
		return;
	p->level_ms = now;
	if (p->hooks.modem_level(p->hooks.ctx, &peak, &windows) || windows == p->level_windows)
		return;
	p->level_windows = windows;
	if (p->cfg.level_trace)
		stamped(p->log, "  channel %u: network audio %s: window %u peak %u\n", p->ch, p->alerting ? "alerting" : "setup", windows, peak);
	if (p->cfg.ringback != PHONE_RINGBACK_AUTO || p->net_tone)
		return;
	p->net_run = peak > NET_AUDIO_PEAK ? p->net_run + 1 : 0;
	if (p->net_run >= NET_AUDIO_WINDOWS) {
		p->net_tone = 1;
		stamped(p->log, "  channel %u: the network's own audio is heard: the local ringing tone stops for this call\n", p->ch);
	}
}

/* one step of the modem client: finish what is open, then start the next request */
static int modem_service(struct chip *c, unsigned int ch, struct phone *p, unsigned long long now, FILE *log)
{
	const struct call_ops *call = p->call;
	int res = 0, err = 0;

	if (!has_modem(p))
		return 0;
	if (p->mq_busy) {
		if (!call->pump(call->ctx, &res))
			return 0;
		p->mq_busy = 0;
		if (res == -ENODEV) {
			modem_lost(c, ch, p, now, log);
			return 0;
		}
		if (res == -ETIMEDOUT)
			p->modem_timeouts++;
		else
			p->modem_reply_ms = now;
		if (p->mq_kind == CALL_REQ_STATE) {
			p->poll_due_ms = now + poll_ms(p, now);
			if (res < 0) {
				if (!p->poll_bad)
					stamped(log, "  channel %u: the modem did not answer the call list (%d)\n", ch, res);
				p->poll_bad++;
				return 0;
			}
			if (p->poll_bad)
				stamped(log, "  channel %u: the modem answers the call list again\n", ch);
			p->poll_bad = 0;
			legs_update(p);
			waiting_update(p, now);
			err = on_call_state(c, ch, p, res, now, log);
			legs_update(p);
			return err;
		}
		if (p->mq_kind == CALL_REQ_CMD)
			on_cmd_done(c, ch, p, res, now, log);
		else if (p->mq_kind == CALL_REQ_INFO && p->ccwa_stage == 2) {
			p->ccwa_stage = 0;
			p->ccwa_net = res ? MQ_UNKNOWN : modemq_ccwa(call->body ? call->body(call->ctx) : "");
		} else if (p->mq_kind == CALL_REQ_INFO && p->fwd_stage == FWD_STAGE_QUERYING)
			fwd_query_done(p, res, call->body ? call->body(call->ctx) : "");
		else if (p->mq_kind == CALL_REQ_INFO)
			on_info_done(p, res, now);
		else
			on_cause_done(ch, p, res, now, log);
		return 0;
	}
	if (p->mq_waiting) {
		snprintf(p->mq_cmd, sizeof(p->mq_cmd), "%s", p->mq_wait[0]);
		err = call->begin(call->ctx, CALL_REQ_CMD, p->mq_cmd);
		if (err == -EBUSY)
			return 0;
		memmove(p->mq_wait[0], p->mq_wait[1], (MODEM_QUEUE - 1) * MODEM_CMD_MAX);
		p->mq_waiting--;
		if (err == -ENODEV) {
			modem_lost(c, ch, p, now, log);
		} else if (err) {
			on_cmd_done(c, ch, p, err, now, log);		/* refused at once: the same as a failed command */
		} else {
			p->mq_busy = 1;
			p->mq_kind = CALL_REQ_CMD;
		}
	} else if (p->cause_want) {
		err = call->begin(call->ctx, CALL_REQ_CAUSE, NULL);
		if (err == -EBUSY)
			return 0;
		p->cause_want = 0;
		if (err == -ENODEV) {
			modem_lost(c, ch, p, now, log);
		} else if (err) {
			on_cause_done(ch, p, err, now, log);
		} else {
			p->mq_busy = 1;
			p->mq_kind = CALL_REQ_CAUSE;
		}
	} else if (p->ccwa_stage == 1 && p->state == LINE_IDLE && !p->hw.off_hook && !p->ringing && !p->incoming && p->call_seen == CALL_NONE) {
		snprintf(p->mq_cmd, sizeof(p->mq_cmd), "AT+CCWA=1,2,1");
		err = call->begin(call->ctx, CALL_REQ_INFO, p->mq_cmd);
		if (err == -EBUSY)
			return 0;
		if (err == -ENODEV) {
			modem_lost(c, ch, p, now, log);
		} else if (err) {
			p->ccwa_stage = 0;			/* refused at once: the read-back stays unknown */
		} else {
			p->ccwa_stage = 2;
			p->mq_busy = 1;
			p->mq_kind = CALL_REQ_INFO;
		}
	} else if (p->fwd_stage == FWD_STAGE_QUERY_DUE) {
		char cmd[MODEM_CMD_MAX];

		(void)forward_query_cmd(p->fwd_job.when, cmd, sizeof(cmd));
		snprintf(p->mq_cmd, sizeof(p->mq_cmd), "%s", cmd);
		err = call->begin(call->ctx, CALL_REQ_INFO, p->mq_cmd);
		if (err == -EBUSY)
			return 0;
		if (err == -ENODEV) {
			modem_lost(c, ch, p, now, log);
		} else if (err) {
			fwd_query_done(p, err, "");
		} else {
			p->mq_busy = 1;
			p->mq_kind = CALL_REQ_INFO;
			p->fwd_stage = FWD_STAGE_QUERYING;
		}
	} else if (now >= p->poll_due_ms) {
		err = call->begin(call->ctx, CALL_REQ_STATE, NULL);
		if (err == -EBUSY)
			return 0;
		p->poll_due_ms = now + poll_ms(p, now);
		if (err == -ENODEV) {
			modem_lost(c, ch, p, now, log);
		} else if (!err) {
			p->mq_busy = 1;
			p->mq_kind = CALL_REQ_STATE;
		}
	} else if (info_wanted(p, now)) {
		/* one read-only question at a time, only while nothing else is going on; no reply is waited for here */
		snprintf(p->mq_cmd, sizeof(p->mq_cmd), "%s", modemq_command[p->info_next]);
		err = call->begin(call->ctx, CALL_REQ_INFO, p->mq_cmd);
		if (err == -ENODEV) {
			modem_lost(c, ch, p, now, log);
		} else if (!err) {
			p->mq_busy = 1;
			p->mq_kind = CALL_REQ_INFO;
		} else if (err != -EBUSY) {
			p->info_due_ms = now + INFO_EVERY_MS;		/* refused: try the next question later */
			info_advance(p);
		}
	}
	return 0;
}

/*
 * The number inside a dial string: the per-call caller-id codes `#31#` (hide my number) and `*31#` (show it) come before it (3GPP TS 22.030) and are
 * passed to the modem unchanged; the blocking rules and the emergency check look at the number behind them.
 */
static const char *dial_target(const char *dial)
{
	return !strncmp(dial, "#31#", 4) || !strncmp(dial, "*31#", 4) ? dial + 4 : dial;
}

/* the collected digits go to the network as ATD...; (3GPP TS 27.007); the bridge already carries both ways, so
 * the speech opens by itself when the remote answers. A dial the network refuses takes the release tone */
static void dial_out(unsigned int ch, struct phone *p, unsigned long long now, FILE *log)
{
	char cmd[5 + DIAL_NUMBER_MAX + 1];

	if (!has_modem(p))
		return;					/* no call control: the digits stay a log only */
	if (!p->dial[1] && (p->dial[0] == '*' || p->dial[0] == '#')) {
		p->dial[0] = 0;				/* a lone star or hash is no dial string: forgotten, as a stray key always was */
		p->dial_last_ms = 0;
		return;
	}
	if (p->cfg.block && blocking_outgoing(p->cfg.block, dial_target(p->dial))) {
		/* a blocked number is not dialled: the congestion tone (cause 34, no circuit available, TS 24.008 -> ITU-T E.180) and a record in the call log */
		struct phone_call_record rec;

		record_close(p, now);
		memset(&rec, 0, sizeof(rec));
		rec.id = ++p->call_seq;
		rec.dir = PHONE_DIR_OUT;
		snprintf(rec.number, sizeof(rec.number), "%s", p->dial);
		rec.start_ms = rec.end_ms = now;
		rec.result = phone_result_of(PHONE_DIR_OUT, 0, 0, 1, 0);
		if (p->hooks.call_ended)
			p->hooks.call_ended(p->hooks.ctx, &rec);
		stamped(log, "  channel %u: a blocked number was dialled: the congestion tone, no call is made\n", ch);
		p->dial[0] = 0;
		p->dial_last_ms = 0;
		p->cause = 34;
		p->cause_pending = 0;
		release_tone_start(p, now);
		set_line(p, LINE_RELEASE, now);
		return;
	}
	snprintf(cmd, sizeof(cmd), "ATD%s;", p->dial);
	if (p->call->service_log)
		stamped(log, "  channel %u: dialing (%zu digits)\n", ch, strlen(p->dial));
	else
		stamped(log, "  channel %u: dialing %s\n", ch, p->dial);
	modem_cmd(p, cmd, ch, log);
	call_begin(p, PHONE_DIR_OUT, now);
	p->calls_out++;
	snprintf(p->peer, sizeof(p->peer), "%s", p->dial);
	p->dial[0] = 0;
	p->dial_last_ms = 0;
	p->alerting = 0;
	p->setup_said = 0;
	release_tone_start(p, now);
	set_line(p, LINE_SETUP, now);
}

/* the outgoing call's progress tones on the board's own tone preset (the dial tone's oscillators): the ringing
 * tone from the alerting until the answer, the busy or the congestion tone after a release before the answer,
 * the ITU-T E.180 cadences on the clock; a release tone stops after half a minute so the waiting state returns */
static void progress_tick(struct chip *c, unsigned int ch, struct phone *p, unsigned long long now, FILE *log)
{
	unsigned long long on, pos;
	int want;

	if (p->state == LINE_RELEASE && p->cause_pending)
		return;					/* the cause decides the cadence: the tone waits for it */
	if (p->state == LINE_RELEASE && elapsed_ms(now, p->prog_start_ms) >= RELEASE_TONE_MAX_MS) {
		(void)osc_stop_once(c, ch, log);
		set_line(p, LINE_IDLE, now);			/* still off-hook: the dial tone returns at the next hook read */
		stamped(log, "  channel %u: the release tone gave up\n", ch);
		return;
	}
	if (p->state == LINE_NO_SERVICE && elapsed_ms(now, p->prog_start_ms) >= RELEASE_TONE_MAX_MS) {
		if (p->prog_on && !osc_stop_once(c, ch, log))	/* like the release tone: half a minute, then silence until the handset goes down or the service returns */
			p->prog_on = 0;
		return;
	}
	if (p->state == LINE_SETUP && (p->net_tone || p->cfg.ringback == PHONE_RINGBACK_OFF)) {
		if (p->prog_on == 1 && !osc_stop_once(c, ch, log))	/* the network's audio (or the option) has the ringing: ours is off, mid-burst too */
			p->prog_on = 0;
		return;
	}
	if (p->state == LINE_SETUP && !p->alerting)
		return;					/* the network is still setting the call up, or its own ringing tone is all the caller should hear (ringback 0) */
	on = p->state == LINE_SETUP ? RINGBACK_ON_MS : (p->state == LINE_NO_SERVICE || is_congestion(p->cause)) ? CONGEST_ON_MS : BUSY_ON_MS;
	pos = elapsed_ms(now, p->prog_start_ms) % (p->state == LINE_SETUP ? RINGBACK_ON_MS + RINGBACK_OFF_MS : 2 * on);
	want = pos < on;
	if (want == p->prog_on || now < p->tone_retry_ms)
		return;
	if (want ? osc_start_once(c, ch, log) : osc_stop_once(c, ch, log))
		p->tone_retry_ms = now + TONE_RETRY_MS;
	else
		p->prog_on = want;
}

/*
 * Is there service for a call? Without a modem client (the stage tool's bench stages) there is nothing to ask and the dial tone
 * is the rule. With one: the modem port is open and answering, and the network does not say "not registered" (+CEREG, an unknown
 * answer counts as service so that a lift right after the start is not refused). ITU-T E.180: the congestion tone says the call
 * cannot be set up.
 */
static int service_ok(const struct phone *p)
{
	if (!p->call || !p->call->begin)
		return 1;
	return has_modem(p) && p->poll_bad < POLL_BAD_MAX && p->registered != 0;
}

/* the dial tone for a lifted handset: only when the line is idle and no call is ringing in */
static void present_dial_tone(struct chip *c, unsigned int ch, struct phone *p, uint8_t lcr, FILE *log)
{
	unsigned long long now = clock_ms(c);

	if (p->state != LINE_IDLE || p->incoming || now < p->tone_retry_ms)
		return;
	if (!service_ok(p)) {
		/* no service: the congestion tone (the cadence runs from the progress tick); the keys are still collected when the modem is there (emergency numbers) */
		release_tone_start(p, now);
		set_line(p, LINE_NO_SERVICE, now);
		stamped(log, "  channel %u: no service (%s): the congestion tone\n", ch, has_modem(p) ? "not registered" : "no modem");
		p->info_next = info_index("AT+CEREG?");
		p->info_due_ms = now;
		return;
	}
	if (osc_start_once(c, ch, log)) {
		p->tone_retry_ms = now + TONE_RETRY_MS;		/* a start that failed is tried again, but not at every read */
		return;
	}
	set_line(p, LINE_DIAL_TONE, now);
	stamped(log, "  channel %u: dial tone on (the caller is off-hook, hook reg 0x%02x)\n", ch, lcr);
}

/* the listed legs as "idx:state" words, for the log of a hang-up decision (states only, never a number) */
static void legs_text(const struct phone *p, char *out, size_t cap)
{
	static const char *const word[] = { "active", "held", "dialing", "alerting", "incoming", "waiting" };
	size_t n = 0;
	unsigned int k;

	out[0] = 0;
	for (k = 0; k < p->nlegs && n < cap; k++)
		n += (size_t)snprintf(out + n, cap - n, "%s%d:%s", k ? " " : "", p->leg[k].idx,
			(unsigned int)p->leg[k].stat < 6 ? word[p->leg[k].stat] : "?");
}

/* a call is left on hold at the hang-up: the phone rings back from now, as a hold recall does, until the lift takes the
 * call or its remote ends (the list tells later whether the survivor is held or already up again) */
static void recall_start(struct chip *c, unsigned int ch, struct phone *p, FILE *log)
{
	if (p->recall || !p->ring_ready || !p->call->ring || ring_start(c, ch, p, p->step_ms, log))
		return;
	p->recall = p->incoming = p->offered = 1;
	p->recall_active = 0;
	p->tries = 0;
	stamped(log, "  channel %u: a held call rings back\n", ch);
}

/* the handset went down (a break of the hang-up window): whatever the line was doing ends, and the line is idle */
static void clear_down(struct chip *c, unsigned int ch, struct phone *p, FILE *log)
{
	const enum line_state was = p->state;

	p->flash_open = 0;
	if (was == LINE_CALL && p->nlegs > 1 && (legs_with(p, LEG_WAITING) || legs_with(p, LEG_HELD))) {
		/* a call is waiting or held: only the active call is released (a held call rings back, a waiting one is presented); AT+CHUP would end them all */
		unsigned int k;
		char cmd[MODEM_CMD_MAX];

		for (k = 0; k < p->nlegs; k++)
			if (p->leg[k].stat == LEG_ACTIVE) {
				char legs[96];

				legs_text(p, legs, sizeof(legs));
				stamped(log, "  channel %u: on hook with the calls %s: the active one is released\n", ch, legs);
				p->released_one = 1;
				snprintf(cmd, sizeof(cmd), "AT+CHLD=1%d", p->leg[k].idx);
				p->abandon_cmd = 1;		/* a refusal falls back to the hang-up of all */
				modem_cmd(p, cmd, ch, log);
				if (legs_with(p, LEG_HELD))
					recall_start(c, ch, p, log);
				break;
			}
	} else if (was == LINE_SETUP && p->nlegs > 1 && (legs_with(p, LEG_DIALING) || legs_with(p, LEG_ALERTING))) {
		/* the second call is given up while another is held. On this modem a dialling or alerting leg cannot be
		 * released on its own: AT+CHLD=1x is refused for it, and AT+CHLD=1 (TS 22.030 "1") left it alerting until
		 * it was answered while the held call was lost. So the hang-up ends every call, said once; a
		 * release of one leg by a documented command of this modem is TO-VERIFY */
		char legs[96];

		legs_text(p, legs, sizeof(legs));
		stamped(log, "  channel %u: on hook with the calls %s: the call being set up cannot be released alone, all calls are hung up\n", ch, legs);
		p->ending = 1;
		modem_cmd(p, "AT+CHUP", ch, log);
	} else if (was == LINE_CALL || was == LINE_SETUP) {
		/* the caller ended it: the network's own release brings no tone. A refused hang-up is sent again at the
		 * next call list (the call is still listed and the handset is down) */
		p->ending = 1;
		modem_cmd(p, "AT+CHUP", ch, log);
	}
	if (was == LINE_SETUP || was == LINE_RELEASE || was == LINE_NO_SERVICE)
		(void)osc_stop_once(c, ch, log);	/* a tone window may be open */
	if (was == LINE_SETUP) {
		stamped(log, "  channel %u: the call attempt was abandoned\n", ch);
		if (p->call_seen == CALL_NONE && p->call_id) {
			/* the network never listed it: the attempt is over for good here, so its record is made and the next call gets its own id */
			p->rec_gone_ms = p->step_ms;
			record_close(p, p->step_ms);
			emit(p, PHONE_EV_CALL, p->step_ms);
			p->call_id = 0;
			p->dir = PHONE_DIR_NONE;
			p->call_since_ms = p->answered_ms = 0;
			p->peer[0] = 0;
		}
	}
	if (was == LINE_DIAL_TONE && !osc_stop_once(c, ch, log))
		stamped(log, "  channel %u: dial tone stopped (the caller is on-hook)\n", ch);
	if (was == LINE_DIALLING)
		stamped(log, "  channel %u: dial session cleared (on hook)\n", ch);
	p->dial[0] = 0;
	p->dial_last_ms = 0;
	p->cause_pending = p->cause_want = 0;
	if (p->refused && p->incoming && !p->blocked) {
		/* the refused call is still alive in the network (the release did not take): with the handset down it is
		 * offered like a call that was not taken while the line was in use */
		p->refused = 0;
		p->offered = 1;
		p->tries = 0;
	}
	set_line(p, LINE_IDLE, p->step_ms);
}

/*
 * The hook machine. One sample of the chip's hook register goes into the hook engine with the monotonic time, and
 * the engine's event drives the line (ETSI TS 101 868 clause 4.7):
 *   lift      a new call: the dial tone;
 *   flash     a break between flash_min and the hang-up window (the handset's R key, or a quick hook toggle): the
 *             call and the line stay. While the caller is dialling the digits are cleared and a fresh dial tone is
 *             presented (the exchange's recall dial tone); during a call the flash is reserved for the call-control
 *             features (AT+CHLD, the daemon) and only logged;
 *   bounce    a break shorter than flash_min: ignored;
 *   on hook   the break reached the hang-up window: hang up, clear the dial session, stop the tones.
 */
static void hook_machine(struct chip *c, unsigned int ch, struct phone *p, uint8_t lcr, FILE *log)
{
	unsigned int width = 0;
	unsigned long long now = clock_ms(c);
	enum hookwatch_event ev;

	ev = hookwatch_feed(&p->hw, now, (lcr & LCR_ON_HOOK) != 0, &width);
	switch (ev) {
	case HW_LIFT:
		/* the ring trip: a handset lifted while an offered call is incoming answers it now, not at the next call list
		 * (ETSI EN 300 001: the called party's answer is the loop closing) */
		if (p->incoming && p->offered && p->tries < CALL_TRY_MAX && p->state <= LINE_DIAL_TONE)
			answer_incoming(p, ch, log);
		present_dial_tone(c, ch, p, lcr, log);
		break;
	case HW_FLASH:
		stamped(log, "  channel %u: hook flash of %u ms\n", ch, width);
		if (p->state == LINE_DIALLING) {
			p->dial[0] = 0;
			p->dial_last_ms = 0;
			set_line(p, LINE_DIAL_TONE, now);
			(void)osc_stop_once(c, ch, log);
			if (osc_start_once(c, ch, log))
				set_line(p, LINE_IDLE, now);	/* the tone could not start: the next hook read tries again */
			else
				stamped(log, "  channel %u: flash while dialling: digits cleared, fresh dial tone\n", ch);
		} else if (p->state == LINE_IDLE) {
			present_dial_tone(c, ch, p, lcr, log);
		} else if (flash_use(p) != FLASH_NOTHING) {
			p->flash_open = 1;			/* a key now is a code; none: the flash alone acts (take the waiting call, hold, swap, take back) */
			p->flash_until_ms = now + FLASH_CODE_MS;
		} else if (p->state == LINE_RELEASE && p->incoming && p->wait_promoted) {
			answer_incoming(p, ch, log);		/* the call that waited is now ringing in: a flash takes it */
		}
		break;
	case HW_BOUNCE:
		stamped(log, "  channel %u: hook bounce of %u ms ignored\n", ch, width);
		break;
	case HW_ONHOOK:
		clear_down(c, ch, p, log);
		break;
	case HW_NONE:
	case HW_GLITCH:
	default:
		break;
	}
	/* the line is level-driven too: a handset that is up on an idle line gets its dial tone, so a start that failed
	 * or a tone that ended (a release tone that gave up) is presented again at the next hook read */
	if ((lcr & LCR_ON_HOOK) && ev != HW_ONHOOK)
		present_dial_tone(c, ch, p, lcr, log);
}

/* the engine starts idle: a handset that is already up is a lift */
static struct hookwatch_cfg hook_limits(const struct phone *p)
{
	if (p->cfg.hook.flash_max_ms)
		return p->cfg.hook;
	if (p->call && p->call->hook_cfg.flash_max_ms)
		return p->call->hook_cfg;
	return (struct hookwatch_cfg){ HOOKWATCH_FLASH_MIN_DEFAULT_MS, HOOKWATCH_FLASH_MAX_DEFAULT_MS };
}

static void hook_start(struct chip *c, struct phone *p)
{
	struct hookwatch_cfg cfg = hook_limits(p);

	hookwatch_init(&p->hw, &cfg, 0, clock_ms(c));
}

/*
 * One hook read of the watch loop: the register goes into the machine, and the loop's own timing is recorded. The
 * hook must be read every few milliseconds whatever else the loop does; a gap longer than HOOK_GAP_LOG_MS is logged
 * with the slowest task of that gap, so a stall shows in the log instead of hiding as a missed press.
 */
static int hook_step(struct chip *c, unsigned int ch, struct phone *p, FILE *log)
{
	unsigned long long now;
	uint8_t lcr;
	int err = chip_reg_read(c, ch, REG_LCRRTP, &lcr);

	if (err) {
		/* a watch that cannot read the hook is blind: after a few failures in a row the hold stops (the line is released) */
		p->faults++;
		if (++p->read_fail >= READ_FAIL_MAX)
			fprintf(log, "  channel %u: the hook register could not be read %u times in a row (%d)\n", ch, p->read_fail, err);
		return p->read_fail >= READ_FAIL_MAX ? err : 0;
	}
	p->read_fail = 0;
	now = clock_ms(c);
	p->lcr = lcr;
	if (p->hook_at_ms) {
		unsigned int gap = (unsigned int)(now - p->hook_at_ms);

		if (gap > p->hook_gap_max)
			p->hook_gap_max = gap;
		if (gap > HOOK_GAP_LOG_MS && p->hook_gaps++ < HOOK_GAP_LOG_MAX)
			stamped(log, "  channel %u: the hook was not read for %u ms (slowest task: %s, %llu ms)\n", ch, gap,
				p->slow_what ? p->slow_what : "none", p->slow_ms);
	}
	p->hook_at_ms = now;
	p->slow_ms = 0;
	p->slow_what = NULL;
	p->hook_reads++;
	hook_machine(c, ch, p, lcr, log);
	return 0;
}

/*
 * Speed dial (the page's U28): two digits and the send key dial the first number of the contact that has that code. The same two digits
 * without the send key are an ordinary number that goes out after the pause. A number that is an emergency number as dialled is never replaced
 * and a stored number too long for the dial buffer is not used.
 */
static void speed_dial(struct phone *p, unsigned int ch, FILE *log)
{
	char number[PHONE_NUMBER_MAX + 1];

	if (!p->cfg.book || strlen(p->dial) != PB_QUICK_LEN || number_is_emergency(p->dial, p->cfg.region))
		return;
	if (phonebook_speed(p->cfg.book, p->dial, number, sizeof(number)))
		return;
	if (strlen(number) > DIAL_NUMBER_MAX) {
		stamped(log, "  channel %u: the speed-dial number is too long to dial: the digits go out as pressed\n", ch);
		return;
	}
	snprintf(p->dial, sizeof(p->dial), "%s", number);
	stamped(log, "  channel %u: speed dial\n", ch);
}

/* the tone detector's event: one key press (ITU-T Q.24 digits), the dial session's collection and the send key */
static void dtmf_event(struct chip *c, unsigned int ch, struct phone *p, unsigned long long now, FILE *log)
{
	uint8_t code;
	char digit = 0;

	if (!chip_reg_read(c, ch, REG_TONDTMF, &code)) {
		digit = dtmf_digit_map[code & 0xf];
		if (p->call && p->call->service_log)
			stamped(log, "  channel %u: a key was pressed\n", ch);
		else
			stamped(log, "  channel %u: dtmf %c\n", ch, digit);
	}
	if (p->state == LINE_CALL && digit) {
		if (p->flash_open && flash_code(p, digit))
			return;
		p->flash_open = 0;
		send_key(p, digit, ch, log);		/* the network hears the key as a tone of its own (TS 27.007 +VTS) */
		return;
	}
	if (p->state == LINE_DIAL_TONE && digit && p->flash_open && flash_code(p, digit))
		return;				/* the key after a flash on the recall dial tone is a code (take the held call back), not a digit */
	p->flash_open = p->flash_open && p->state == LINE_CALL;
	if (p->state == LINE_NO_SERVICE && digit && !has_modem(p))
		return;					/* nobody to dial to */
	if ((p->state == LINE_DIAL_TONE || p->state == LINE_NO_SERVICE) && digit) {
		if (!osc_stop_once(c, ch, log))
			stamped(log, "  channel %u: %s stopped (first digit)\n", ch, p->state == LINE_NO_SERVICE ? "congestion tone" : "dial tone");
		set_line(p, LINE_DIALLING, now);
	}
	if (p->state == LINE_DIALLING && digit) {
		p->dial_last_ms = now;		/* every DTMF event restarts the pause between digits */
		/* a dial string that begins with * or # is a service code (3GPP TS 22.030, e.g. #31#<number> hides the caller id for this call): its * and #
		 * are part of it, the send key does not work in it, and it goes out after the pause like any dial */
		const int service = p->dial[0] == '*' || p->dial[0] == '#' || (!p->dial[0] && (digit == '*' || digit == '#'));

		if (((digit >= '0' && digit <= '9') || (service && (digit == '*' || digit == '#'))) && strlen(p->dial) < DIAL_NUMBER_MAX) {
			size_t dn = strlen(p->dial);

			p->dial[dn] = digit;
			p->dial[dn + 1] = 0;
		} else if (digit == '#' && p->dial[0]) {
			speed_dial(p, ch, log);
			dial_out(ch, p, now, log);	/* 3GPP TS 22.030: # is the send key */
		}
		/* the other symbols are not dialable digits: they are logged above and dropped */
	}
}

/*
 * The chip's event register: the tone detector's event and the loop-change flag come in one read. It is read BEFORE
 * the hook register and its content is handled AFTER the hook read has gone into the engine: a hook change latches
 * the flag when it happens, so a flag read first belongs to a change that the hook read right after it (or an earlier
 * one) shows, and the engine can match them; handled the other way round, the very first flag of a lifted handset
 * would be called a loop change that no hook read showed. A flag that no hook read explains is a press shorter than
 * the read spacing (the engine counts it, the log shows the first few).
 */
static int irq_read(struct chip *c, unsigned int ch, struct phone *p, uint8_t *irq, FILE *log)
{
	int err = chip_reg_read(c, ch, REG_IRQ2, irq);

	if (err) {
		*irq = 0;
		p->faults++;
		if (++p->irq_fail >= READ_FAIL_MAX)
			fprintf(log, "  channel %u: the event register could not be read %u times in a row (%d)\n", ch, p->irq_fail, err);
		return p->irq_fail >= READ_FAIL_MAX ? err : 0;
	}
	p->irq_fail = 0;
	return 0;
}

static void irq_process(struct chip *c, unsigned int ch, struct phone *p, uint8_t irq, unsigned long long now, FILE *log)
{
	if ((irq & LCR_IRQ_BIT) && hookwatch_flag(&p->hw, 1) == HW_GLITCH && !p->ringing &&
	    p->glitch_logged++ < HOOK_GAP_LOG_MAX)
		stamped(log, "  channel %u: the chip flagged a loop change that no hook read showed (shorter than the %u ms read spacing)\n",
			ch, HOOK_POLL_MS);
	if (irq & DTMF_IRQ_BIT)
		dtmf_event(c, ch, p, now, log);
}

/*
 * The sample: the line feed state, the loop current and the battery -- the guard that the feed is sound -- logged.
 * Returns 0, or the error that must stop the hold (the caller releases the line).
 */
int observe(struct chip *c, unsigned int ch, unsigned int sample, unsigned int log_every, FILE *log, int *battery_mv, int *loop_ua)
{
	uint8_t lf;
	int32_t now, expected, ua = 0, floor;
	int err = chip_reg_read(c, ch, REG_LINEFEED, &lf);

	if (!err && (lf & LF_STATE_MASK) != LF_CODE_FWD_ACTIVE) {
		fprintf(log, "  channel %u: the line feed left the active state (0x%02x)\n", ch, lf);
		err = -EIO;
	}
	if (!err)
		err = read_iloop(c, ch, &ua);
	if (!err)
		err = read_battery(c, ch, &now, &expected);
	if (err) {
		fprintf(log, "  channel %u: hold observation failed (%d)\n", ch, err);
		return err;
	}
	if (battery_mv)
		*battery_mv = now;
	if (loop_ua)
		*loop_ua = ua;
	if (sample % log_every == 0)
		fprintf(log, "  channel %u: hold %u: battery %d mV, loop current %d uA\n", ch, sample + 1, now, ua);
	floor = expected - LF_HOLD_GUARD_MV;
	if (now < 0) {
		fprintf(log, "  channel %u: battery is negative\n", ch);
		return -EIO;
	}
	if (now < floor) {
		if (ua == 0) {
			fprintf(log, "  channel %u: battery fell below the guard (below %d mV) with no loop current\n", ch, floor);
			return -EIO;
		}
		/* the battery follows its load: normal feed behaviour (H25), logged with the periodic reading only */
		if (sample % log_every == 0)
			fprintf(log, "  channel %u: the battery follows its load (loop current %d uA), the line stays fed\n", ch, ua);
	}
	return 0;
}

/* one pause between dialled digits (the setting, or the default) */
static unsigned int dial_pause_ms(const struct phone *p)
{
	return p->cfg.dial_pause_ms ? p->cfg.dial_pause_ms : DIAL_IDLE_MS;
}

/* new settings take hold at a moment nobody is using the line */
static void apply_cfg(struct phone *p, unsigned long long now)
{
	struct hookwatch_cfg limits;
	const int clir_changed = p->pending_cfg.clir_set && (!p->cfg.clir_set || p->pending_cfg.clir != p->cfg.clir);

	if (p->pending_cfg.forward.configured && !forward_same(&p->cfg.forward, &p->pending_cfg.forward))
		p->fwd_want = 1;
	if (p->pending_cfg.hd != p->cfg.hd) {
		/* the chip side takes hold at once (the line is idle); the modem's PCM rate follows in the exchange */
		if (wideband_apply(p->c, p->ch, &p->wb, p->pending_cfg.hd, p->log) == 0)
			p->hd_want = 1;
		else
			stamped(p->log, "  channel %u: the wideband registers did not take: the modem is left as it was\n", p->ch);
	}
	p->cfg = p->pending_cfg;
	if (clir_changed) {
		char cmd[MODEM_CMD_MAX];

		snprintf(cmd, sizeof(cmd), "AT+CLIR=%d", p->cfg.clir);
		modem_cmd(p, cmd, p->ch, p->log);	/* a changed setting reaches the modem at once; the read-back follows in the idle questions */
	}
	p->cfg_pending = 0;
	limits = hook_limits(p);
	hookwatch_init(&p->hw, &limits, 0, now);
}

struct phone *phone_attach(struct chip *c, unsigned int ch, const struct board *b, const struct call_ops *call,
	const struct phone_cfg *cfg, const struct phone_hooks *hooks, FILE *log)
{
	struct phone *p = calloc(1, sizeof(*p));
	unsigned long long now;

	if (!p)
		return NULL;
	p->c = c;
	p->ch = ch;
	p->log = log;
	p->call = call;
	p->board = b;
	if (cfg)
		p->cfg = *cfg;
	p->cfg.channel = ch;
	if (hooks)
		p->hooks = *hooks;
	hook_start(c, p);
	now = p->step_ms = clock_ms(c);
	p->next_ring_ms = now + RING_POLL_MS;
	p->next_sample_ms = now + GUARD_MS;
	p->poll_due_ms = now;
	p->sim_ready = p->registered = p->ims = p->rsrp_dbm = p->audio_slave = p->clir = p->clir_status = p->ccwa_net = MQ_UNKNOWN;
	p->fwd_want = p->cfg.forward.configured;
	modem_setup(p, now);			/* the service's modem setup (volatile settings of the modem) */
	return p;
}

void phone_hook_feed(struct phone *p, uint8_t lcr)
{
	hook_machine(p->c, p->ch, p, lcr, p->log);
}

int phone_watch_end(struct phone *p, int err)
{
	struct chip *c = p->c;
	unsigned int ch = p->ch;
	FILE *log = p->log;

	if (p->ringing && ring_stop(c, ch, p, "the hold ended", log) && !err)
		err = -EIO;
	fprintf(log, "  channel %u: hook watch: %u reads, longest gap %u ms, %u gaps over %u ms, %u unexplained chip flags\n",
		ch, p->hook_reads, p->hook_gap_max, p->hook_gaps, HOOK_GAP_LOG_MS, p->hw.glitches);
	return err;
}

void phone_free(struct phone *p)
{
	free(p);
}

int phone_open(struct phone **out, struct chip *c, const struct board *b, const struct call_ops *call,
	const struct phone_cfg *cfg, const struct phone_hooks *hooks, FILE *log)
{
	const struct board_section *words = board_find(b, "converter");
	const struct board_section *imped = board_find(b, "imped");
	const struct board_section *tones = board_find(b, "tone");
	unsigned int ch = cfg->channel;
	struct phone *p = NULL;
	int err, leave, fed = 0, det_on = 0, ring_ok = 0;

	*out = NULL;
	if (!words || words->length != CONVERTER_SIZE) {
		fprintf(log, "  board file has no %u-byte converter section\n", CONVERTER_SIZE);
		return -EINVAL;
	}
	if (!imped || imped->length < IMPED_PRESET_OFFSET + IMPED_PRESET_SIZE) {
		fprintf(log, "  board file has no %u-byte impedance preset\n", IMPED_PRESET_SIZE);
		return -EINVAL;
	}
	if (!tones || tones->length < TONE_PRESET_OFFSET + TONE_PRESET_SIZE) {
		fprintf(log, "  board file has no %u-byte tone preset\n", TONE_PRESET_SIZE);
		return -EINVAL;
	}

	err = chip_user_mode(c, ch, 1);
	if (err)
		return fail_stage(log, "enter protected mode", ch, err);
	err = fed_line(c, ch, board_word(words, 0), log);
	if (!err)
		fed = 1;
	if (fed && !err)
		err = tone_up(c, ch, imped, tones, log);
	if (fed && !err) {
		err = dtmf_enable(c, ch, log);
		det_on = !err;
	}
	if (fed && !err) {
		err = voice_up(c, b, ch, log);
		if (err)
			fprintf(log, "  FAILED on channel %u: voice path (%d)\n", ch, err);
	}
	if (fed && !err && p && p->cfg.hd && (err = wideband_apply(c, ch, &p->wb, 1, log)) != 0)
		return err;			/* the wideband registers did not take: the line is closed and brought up again */
	if (fed && !err && hooks && hooks->bridge_start) {
		err = hooks->bridge_start(hooks->ctx, BRIDGE_MODEM_RX, BRIDGE_MODEM_TX, BRIDGE_LINE_RX, BRIDGE_LINE_TX,
			p && p->cfg.hd ? PHONE_BRIDGE_WIDEBAND : 0);
		if (err) {
			fprintf(log, "  FAILED on channel %u: bridge start (%d)\n", ch, err);
		} else {
			fprintf(log, "  bridge cross: modem %u/%u, line %u/%u (bit positions)\n",
				BRIDGE_MODEM_RX, BRIDGE_MODEM_TX, BRIDGE_LINE_RX, BRIDGE_LINE_TX);
		}
	}
	if (fed && !err && call && (call->ring || call->ring_test_s)) {
		if (call->ring_test_s > RING_TEST_MAX_SECONDS)
			err = -EINVAL;
		else
			err = ring_load(c, b, ch, log);
		ring_ok = !err;
	}
	if (fed && !err) {
		p = phone_attach(c, ch, b, call, cfg, hooks, log);	/* idle: the watch gives the dial tone at the first off-hook sample */
		if (!p) {
			err = -ENOMEM;
		} else {
			p->ring_ready = ring_ok;
			p->det_on = det_on;
		}
	}
	if (!err) {
		*out = p;
		return 0;
	}
	if (fed && det_on) {
		int d_err = dtmf_disable(c, ch, log);

		if (!err)
			err = d_err;
	}
	if (fed)
		err = tone_down(c, ch, err, log);
	leave = chip_user_mode(c, ch, 0);
	return err ? err : leave;
}

/* one sample of the line test: the hook, the battery and the loop current (the same readings as the feed guard) */
static void line_test_sample(struct phone *p, unsigned long long now)
{
	int32_t mv = 0, expected = 0, ua = 0;

	p->lt_next_ms = now + LINE_TEST_SPACING_MS;
	if (p->hw.off_hook)
		p->lt_off = 1;
	else if (p->state != LINE_IDLE || p->ringing || p->incoming)
		p->lt_interrupted = 1;
	if (read_battery(p->c, p->ch, &mv, &expected) || read_iloop(p->c, p->ch, &ua)) {
		p->lt_error = 1;
	} else {
		p->lt_battery_sum += mv;
		p->lt_loop_sum += ua;
		p->lt_expected = expected;
	}
	if (++p->lt_n < LINE_TEST_SAMPLES)
		return;
	/* the verdict: the hook first, then the battery (the guard of the feed), the loop current is only reported until the leak threshold is confirmed */
	memset(&p->lt_res, 0, sizeof(p->lt_res));
	p->lt_res.hook_off = p->lt_off;
	p->lt_res.battery_mv = (int)(p->lt_battery_sum / LINE_TEST_SAMPLES);
	p->lt_res.loop_ua = (int)(p->lt_loop_sum / LINE_TEST_SAMPLES);
	p->lt_res.expected_mv = p->lt_expected;
	p->lt_res.leak_suspect = !p->lt_off && p->lt_res.loop_ua > LINE_TEST_LEAK_UA;
	if (p->lt_error)
		p->lt_res.verdict = PHONE_LT_ERROR;
	else if (p->lt_off)
		p->lt_res.verdict = PHONE_LT_OFF_HOOK;
	else if (p->lt_interrupted)
		p->lt_res.verdict = PHONE_LT_INTERRUPTED;
	else if (p->lt_res.battery_mv < 0 || p->lt_res.battery_mv < p->lt_res.expected_mv - LF_HOLD_GUARD_MV)
		p->lt_res.verdict = PHONE_LT_NO_BATTERY;
	else
		p->lt_res.verdict = PHONE_LT_OK;
	p->lt_active = 0;
	p->lt_done = 1;
	stamped(p->log, "  channel %u: line test: %s (battery %d mV of %d, loop current %d uA%s)\n", p->ch, phone_line_verdict_name(p->lt_res.verdict),
		p->lt_res.battery_mv, p->lt_res.expected_mv, p->lt_res.loop_ua, p->lt_res.leak_suspect ? ", above the leak threshold" : "");
}

int phone_line_test(struct phone *p)
{
	if (p->lt_active || p->lt_done || p->state != LINE_IDLE || p->hw.off_hook || p->ringing || p->incoming)
		return -EBUSY;
	p->lt_active = 1;
	p->lt_n = 0;
	p->lt_battery_sum = p->lt_loop_sum = 0;
	p->lt_off = p->lt_interrupted = p->lt_error = 0;
	p->lt_next_ms = 0;			/* the first sample at the next step */
	return 0;
}

int phone_line_test_poll(struct phone *p, struct phone_line_test_result *res)
{
	if (!p->lt_done)
		return 0;
	p->lt_done = 0;
	*res = p->lt_res;
	return 1;
}

const char *phone_line_verdict_name(enum phone_line_verdict v)
{
	static const char *const names[] = { "ok", "off_hook", "leak", "no_battery", "interrupted", "error" };

	return (unsigned int)v < sizeof(names) / sizeof(names[0]) ? names[v] : "error";
}

/*
 * The alarm clock: every ALARM_CHECK_MS the front end's wall clock is asked; when an enabled alarm is set for this minute and weekday the phone
 * rings the normal cadence for at most ALARM_RING_MS with no caller id, and a lift stops it. Only an idle line with the handset down is rung; a busy
 * line skips the alarm (logged, not retried), and so does a clock that is not trusted (logged once per stretch). One ring per minute, whatever the
 * number of alarms set for it.
 */
static void alarm_check(struct phone *p, unsigned long long now)
{
	struct tm tm;
	const int trusted = p->hooks.wall_clock(p->hooks.ctx, &tm) == 0;
	int started;

	p->alarm_next_ms = now + ALARM_CHECK_MS;
	switch (alarms_poll(p->cfg.alarms, &p->alarm_watch, trusted ? &tm : NULL)) {
	case ALARM_CLOCK_NEW_BAD:
		stamped(p->log, "  channel %u: alarms wait: the clock is not set\n", p->ch);
		break;
	case ALARM_RING:
		if (p->state != LINE_IDLE || p->hw.off_hook || p->ringing || p->incoming || !p->ring_ready) {
			stamped(p->log, "  channel %u: alarm missed: line busy\n", p->ch);
			break;
		}
		p->ring_for = RING_FOR_ALARM;
		started = ring_start(p->c, p->ch, p, now, p->log);
		p->ring_for = RING_FOR_CALL;
		if (started)
			break;
		p->ring_end_ms = now + ALARM_RING_MS;
		stamped(p->log, "  channel %u: alarm: the phone rings\n", p->ch);
		break;
	case ALARM_QUIET:
		break;
	}
}

int phone_step(struct phone *p, unsigned long long now)
{
	struct chip *c = p->c;
	unsigned int ch = p->ch;
	FILE *log = p->log;
	unsigned long long t0;
	uint8_t irq;
	int err;

	p->step_ms = now;
	if (p->cfg_pending && p->state == LINE_IDLE && !p->hw.off_hook && !p->ringing)
		apply_cfg(p, now);

	/* the event register is read before the hook register and handled after it (see irq_read) */
	t0 = now;
	err = irq_read(c, ch, p, &irq, log);
	task_done(c, p, "event register", t0);
	t0 = clock_ms(c);
	if (!err)
		err = hook_step(c, ch, p, log);
	task_done(c, p, "hook read", t0);
	if (!err)
		irq_process(c, ch, p, irq, now, log);
	if (!err && p->ringing && p->hw.off_hook) {
		t0 = clock_ms(c);
		err = ring_stop(c, ch, p, "the handset was lifted", log);
		p->poll_due_ms = now;			/* ask the modem for the call right away: the answer follows */
		task_done(c, p, "ring stop", t0);
	}
	/* the pause between digits counts on the clock from the last key and only while the handset is up: a hook
	 * break ends the session first (a flash gives a fresh tone, on hook clears it) and never sends a number */
	if (p->state == LINE_DIALLING && p->dial[0] && p->hw.off_hook && elapsed_ms(now, p->dial_last_ms) >= dial_pause_ms(p))
		dial_out(ch, p, now, log);
	if (p->hot_armed && p->state == LINE_DIAL_TONE && p->cfg.hotline[0] && p->hw.off_hook &&
	    elapsed_ms(now, p->hot_since_ms) >= p->cfg.hotline_delay_ms && has_modem(p)) {
		/* the hotline: nothing was dialled for the delay, so the stored number goes out (any key before this ended the wait) */
		p->hot_armed = 0;
		(void)osc_stop_once(c, ch, log);
		snprintf(p->dial, sizeof(p->dial), "%.*s", DIAL_NUMBER_MAX, p->cfg.hotline);
		stamped(log, "  channel %u: hotline: nothing was dialled, the stored number goes out\n", ch);
		dial_out(ch, p, now, log);
	}
	if (p->fwd_want && p->fwd_stage == FWD_STAGE_IDLE && has_modem(p) && p->state == LINE_IDLE && !p->hw.off_hook &&
	    !p->ringing && !p->incoming && !p->mq_waiting)
		fwd_start(p);
	if (p->hd_want && p->hd_stage == HD_IDLE && has_modem(p) && p->state == LINE_IDLE && !p->hw.off_hook &&
	    !p->ringing && !p->incoming && !p->mq_waiting) {
		char cmd[MODEM_CMD_MAX];

		stamped(p->log, "  channel %u: HD voice %s: the modem's PCM rate changes and the modem restarts (mobile data drops for about half a minute)\n",
			p->ch, p->cfg.hd ? "on" : "off");
		snprintf(cmd, sizeof(cmd), "AT+QDAI=%s,1,0,4,0,%d,1,1", p->qdai_head[0] ? p->qdai_head : "x", p->cfg.hd ? 1 : 0);
		p->hd_stage = HD_QDAI_WRITE;
		modem_cmd(p, cmd, p->ch, p->log);
	}
	t0 = clock_ms(c);
	if (!err)
		err = modem_service(c, ch, p, now, log);
	task_done(c, p, "modem", t0);
	if (p->state == LINE_NO_SERVICE && service_ok(p)) {
		/* the service came back while the handset is up: the dial tone follows at the next hook read */
		(void)osc_stop_once(c, ch, log);
		stamped(log, "  channel %u: service is back\n", ch);
		set_line(p, LINE_IDLE, now);
	}
	if (p->ringing && now >= p->next_ring_ms) {
		t0 = clock_ms(c);
		p->next_ring_ms = now + RING_POLL_MS;
		if (!err)
			err = ring_tick(c, ch, p, now, log);
		task_done(c, p, "ring supervision", t0);
	}
	if (p->state == LINE_SETUP || p->state == LINE_RELEASE || p->state == LINE_NO_SERVICE)
		progress_tick(c, ch, p, now, log);
	if (p->state == LINE_SETUP && p->hooks.modem_level && (p->cfg.level_trace || p->cfg.ringback == PHONE_RINGBACK_AUTO))
		level_watch(p, now);
	if (p->flash_open && now >= p->flash_until_ms)
		flash_alone(p);
	if (p->wait.open || p->wait_tone_on)
		waiting_tone(p, now);
	if (!err && p->cfg.alarms && p->hooks.wall_clock && now >= p->alarm_next_ms)
		alarm_check(p, now);
	if (!err && p->lt_active && now >= p->lt_next_ms)
		line_test_sample(p, now);
	if (!err && !p->ringing && now >= p->next_sample_ms) {
		t0 = clock_ms(c);
		err = observe(c, ch, p->sample++, p->call && p->call->service_log ? GUARD_LOG_EVERY : 1, log, &p->battery_mv, &p->loop_ua);
		if (err)
			p->faults++;
		p->next_sample_ms = now + GUARD_MS;
		task_done(c, p, "sample", t0);
	}
	return err;
}

int phone_close(struct phone *p, int err)
{
	struct chip *c = p->c;
	unsigned int ch = p->ch;
	FILE *log = p->log;
	int det_on = p->det_on, leave;

	err = phone_watch_end(p, err);
	if (det_on) {
		int d_err = dtmf_disable(c, ch, log);

		if (!err)
			err = d_err;
	}
	err = tone_down(c, ch, err, log);
	leave = chip_user_mode(c, ch, 0);
	phone_free(p);
	return err ? err : leave;
}

int phone_hangup(struct phone *p)
{
	if (!p->call_id && !p->incoming && p->state != LINE_SETUP && p->state != LINE_CALL)
		return -ENOENT;
	p->ending = 1;
	modem_cmd(p, "AT+CHUP", p->ch, p->log);
	return 0;
}

int phone_ring_test(struct phone *p, unsigned int seconds)
{
	unsigned long long now;
	int err;

	if (seconds < 1 || seconds > RING_TEST_MAX_SECONDS || !p->ring_ready)
		return -EINVAL;
	if (p->state != LINE_IDLE || p->hw.off_hook || p->ringing)
		return -EBUSY;
	now = clock_ms(p->c);
	p->step_ms = now;
	err = ring_start(p->c, p->ch, p, now, p->log);
	p->ring_end_ms = now + seconds * 1000ull;
	return err;
}

/* 1 while the chip's wideband mode is applied (the HD voice setting in force) */
int phone_hd_active(const struct phone *p)
{
	return wideband_active(&p->wb);
}

int phone_set_cfg(struct phone *p, const struct phone_cfg *cfg)
{
	if (cfg->channel != p->cfg.channel)
		return -EINVAL;
	p->pending_cfg = *cfg;
	p->cfg_pending = 1;
	return 0;
}

void phone_modem_up(struct phone *p)
{
	p->port_lost = 0;
	p->poll_bad = 0;
	modem_setup(p, p->step_ms);
	p->poll_due_ms = p->step_ms;
}

void phone_status(const struct phone *p, struct phone_status *st)
{
	memset(st, 0, sizeof(*st));
	st->line = phone_line_of(p);
	st->off_hook = p->hw.off_hook;
	st->call_id = p->call_id;
	st->dir = p->dir;
	st->call = (enum call_state)p->call_seen;
	if (p->dir == PHONE_DIR_OUT)
		snprintf(st->number, sizeof(st->number), "%s", p->peer);
	else if (p->dir == PHONE_DIR_IN && p->call && p->call->number && p->call->number(p->call->ctx, st->number, sizeof(st->number)))
		st->number[0] = 0;
	st->leg_count = p->nlegs;
	for (unsigned int k = 0; k < p->nlegs; k++) {
		st->legs[k].id = p->leg[k].id;
		st->legs[k].stat = p->leg[k].stat;
		snprintf(st->legs[k].number, sizeof(st->legs[k].number), "%s", p->leg[k].number);
	}
	st->call_since_ms = p->call_since_ms;
	st->answered_ms = p->answered_ms;
	st->modem_up = has_modem(p) && p->poll_bad < POLL_BAD_MAX;
	st->sim_ready = p->sim_ready == MQ_UNKNOWN ? PHONE_UNKNOWN : p->sim_ready;
	st->registered = p->registered == MQ_UNKNOWN ? PHONE_UNKNOWN : p->registered;
	st->ims_registered = p->ims == MQ_UNKNOWN ? PHONE_UNKNOWN : p->ims;
	st->audio_slave = p->audio_slave == MQ_UNKNOWN ? PHONE_UNKNOWN : p->audio_slave;
	st->signal_dbm = p->rsrp_dbm == MQ_UNKNOWN ? PHONE_DBM_UNKNOWN : p->rsrp_dbm;
	st->fwd = p->fwd_net;
	if (!st->fwd.done)
		st->fwd.network = PHONE_UNKNOWN;
	st->call_waiting_net = p->ccwa_net == MQ_UNKNOWN ? PHONE_UNKNOWN : p->ccwa_net;
	st->clir = p->clir == MQ_UNKNOWN ? PHONE_UNKNOWN : p->clir;
	st->clir_status = p->clir_status == MQ_UNKNOWN ? PHONE_UNKNOWN : p->clir_status;
	snprintf(st->network, sizeof(st->network), "%s", p->network);
	st->modem_timeouts = p->modem_timeouts;
	st->modem_reply_ms = p->modem_reply_ms;
	st->faults = p->faults;
	st->battery_mv = p->battery_mv;
	st->loop_ua = p->loop_ua;
	st->calls_in = p->calls_in;
	st->calls_out = p->calls_out;
	st->hook_reads = p->hook_reads;
	st->hook_gap_max_ms = p->hook_gap_max;
	st->hook_gaps = p->hook_gaps;
}

const char *phone_line_name(enum phone_line line)
{
	switch (line) {
	case PHONE_LINE_IDLE: return "idle";
	case PHONE_LINE_RINGING: return "ringing";
	case PHONE_LINE_DIAL_TONE: return "dial_tone";
	case PHONE_LINE_DIALLING: return "dialling";
	case PHONE_LINE_SETUP: return "setup";
	case PHONE_LINE_CALL: return "call";
	case PHONE_LINE_RELEASE: return "release";
	case PHONE_LINE_NO_SERVICE: return "no_service";
	}
	return "unknown";
}
