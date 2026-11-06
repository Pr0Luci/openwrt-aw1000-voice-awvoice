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
#include "dnd.h"
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

#define HD_ASK_MAX	2	/* how many times the modem is asked to change its PCM rate for one wish: each ask
				   restarts the modem and drops the mobile data, so a modem that answers the other rate
				   twice is not asked again until the setting changes or the service starts */

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
	enum phone_answered_by answered_by;	/* the door that took it, when a flash did (the handset's or the page's) */
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
	uint8_t ring_lcr;		/* its ring bits (trip, loop) as last read while the ring runs */
	const struct call_ops *call;	/* optional modem answering; 0 = the hold knows nothing of calls */
	const struct board *board;	/* the board file (the FSK words) */
	/* a call */
	int incoming;			/* a voice call is incoming: the handset is not given a dial tone */
	int offered;			/* the incoming call arrived while the handset was down: it rings, and a lift answers it */
	int refused;		/* the incoming call arrived while the handset was up, or is blocked: the caller is released and the phone does not ring */
	int blocked;		/* ... because the blocking rules say so (it is never offered, not even when the handset goes down) */
	int quieted;		/* quiet hours cover this caller: the call is offered, but the handset is not driven */
	int quiet_mode;		/* the window's own mode, kept for the answering machine and for the page's word */
	int quiet_said;		/* this call's silence has been said once: the verdict is worked out again every step */
	char quiet_peer[PHONE_NUMBER_MAX + 1];	/* the last caller quiet hours covered, for the repeat escape (it outlives the call) */
	uint64_t quiet_epoch;	/* its time in whole seconds, from the daemon's clock; 0 = nothing remembered */
	/* this call's own silence, held apart until it ends: the escape may only look at calls that went, or a caller
	 * answers its own ring. The number is staged here rather than read back at the end because the call list
	 * clearing (3GPP TS 27.007 7.18) wipes p->peer before the incoming call is closed. */
	char quiet_stage_peer[PHONE_NUMBER_MAX + 1];
	uint64_t quiet_stage_epoch;	/* 0 = this call was not silenced (or its number was never known) */
	int machine_answering;		/* the answering machine's answer went out: the ring is off and the call is not offered to the handset */
	int machine_ata_ok;		/* the modem accepted that answer: the call list is only catching up, so no retry and no ring */
	int machine_call;		/* the call in progress was answered by the machine: the handset is down and hears nothing */
	int machine_failed;		/* the modem refused every answer of this call: the phone rings on, the machine does not ask again */
	int fe_answering;		/* a front-end's answer went out: the call list has not shown the call active yet */
	int fe_ata_ok;			/* the modem accepted that answer */
	int fe_failed;			/* every answer of this call was refused: the phone rings on */
	int fe_dial;			/* the outgoing call was dialled by a front-end: the handset is not in it */
	int fe_call;			/* the call in progress is the front-end's: nobody at the handset hears it */
	enum phone_answered_by fe_via;		/* the door the front end's answer came in by: the page's own, or the installed service's */
	enum phone_answered_by answered_by;	/* who answered the current call */
	unsigned int tries;		/* answers (or refusals) sent for this incoming call */
	unsigned long long tried_ms;	/* when the last one was sent */
	int ending;			/* the hang-up was sent: the call list may still show the call for a poll or two */
	int gone_polls;			/* a live call was absent from one list: one more poll confirms its release */
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
	int sim_ready, registered, ims, rsrp_dbm, audio_slave, audio_rate, clir, clir_status;
	enum { RING_FOR_CALL, RING_FOR_ALARM } ring_for;	/* what the ring that is being started is for: an alarm sends no caller id */
	unsigned long long alarm_next_ms;	/* the next check of the alarm clock */
	/* call forwarding at the network: erase, register, read back (forward.h) */
	enum { FWD_STAGE_IDLE, FWD_STAGE_ERASING, FWD_STAGE_REGISTERING, FWD_STAGE_QUERY_DUE, FWD_STAGE_QUERYING } fwd_stage;
	int fwd_want;			/* the settings changed (or are new): the exchange starts when the line is idle */
	/* the HD voice change: the chip's wideband mode is applied at once, then the modem's PCM rate and its restart */
	enum { HD_IDLE, HD_QDAI_WRITE, HD_READBACK } hd_stage;
	int hd_want;			/* the HD setting changed: the modem exchange starts when the line is idle */
	int hd_inforce;			/* the layout this line was brought up with: 0 = 8 kHz, 1 = 16 kHz (the chip, the PCM slots and the bridge agree on it) */
	int hd_layout_due;		/* the modem has answered with the wanted rate: the daemon owes the line a bring-up at it */
	int hd_gave_up;			/* the modem was asked and answered something else: said once, asked again by the next settings change */
	int hd_waiting;			/* the "the rate has not been read yet" line has been said for this wish */
	unsigned int hd_asks;		/* rate writes sent for this wish: a modem that keeps answering the other rate is not pestered */
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
	enum phone_answered_by flash_via;	/* the door of that flash: the handset's key or the page */
	int ccwa_stage;			/* the read-back of the network's call waiting: 0 none, 1 due, 2 asked */
	int ccwa_net;
	unsigned int leg_primary;	/* the line call id that was given to a leg */
	int hot_armed;			/* the hotline may fire in this stretch of dial tone (a digit, a hang-up or a call ends it) */
	unsigned long long hot_since_ms;	/* the dial tone began (again) at this time */
	int audio_warned;		/* the "not in slave mode" warning went out for this modem setup */
	/* the audio path (src/core/phone.h, phone_audio_apply): the bridge is up while a leg, a call this line holds,
	 * or a consumer the daemon claimed needs the samples crossing, and for AUDIO_GRACE_MS after the last of them went */
	int audio_up;			/* the bridge was started and has not been stopped since */
	unsigned int audio_claims;	/* PHONE_AUDIO_* bits: which consumers the daemon says are alive */
	unsigned long long audio_need_ms;	/* when something last needed the path (0 = nothing has ever needed it since the stop) */
	unsigned long long audio_up_ms;	/* when the path last came up */
	unsigned long long audio_retry_ms;	/* while the time is below this, neither a start nor a stop is tried again */
	unsigned int audio_starts, audio_stops, audio_fails;
	int audio_error;		/* why the last attempt failed (0 = the last attempt worked) */
	int audio_error_said;		/* that failure has been said in the log, and nothing has changed since */
	int clvl_pending;		/* the earpiece level waits for the modem's range (asked once per setup) */
	int key_drop_logged;		/* a key found the queue full in this call: said once */
	unsigned long long level_ms;	/* the last reading of the modem's audio level (measuring option) */
	unsigned int level_windows;
	unsigned int net_run;		/* windows in a row above NET_AUDIO_PEAK in this call's set-up */
	int net_tone;			/* the network's own audio is heard in this call: the local ringing tone is off for good */
	char network[MQ_NETWORK_MAX], band[MQ_NETWORK_MAX];
	char mcc[MQ_MCC_MAX];		/* the three digits of the registered network (E.212), "" until +COPS? names one */
	int mcc_warned;			/* the modem named its network in text instead of digits: said once per registration */
	int mcc_named;			/* a reply carried a name where the digits were asked for */
	char netname[MQ_NETWORK_MAX];	/* the name the modem gives for its network, where it gives a name at all (the page and the log show it) */
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
static int service_ok(const struct phone *p);

/* what the line shows the API: the ringing phone is its own state, the rest follow the line state */
static enum phone_line phone_line_of(const struct phone *p)
{
	static const enum phone_line shown[] = {
		[LINE_IDLE] = PHONE_LINE_IDLE, [LINE_DIAL_TONE] = PHONE_LINE_DIAL_TONE, [LINE_DIALLING] = PHONE_LINE_DIALLING,
		[LINE_CALL] = PHONE_LINE_CALL, [LINE_SETUP] = PHONE_LINE_SETUP, [LINE_RELEASE] = PHONE_LINE_RELEASE,
		[LINE_NO_SERVICE] = PHONE_LINE_NO_SERVICE,
	};

	/* an answer still on its way is not an idle line: the ring has stopped, but the call is neither active nor
	 * gone (3GPP TS 27.007 lists a call the network still offers as incoming); saying idle here published a
	 * release to a front end that then hung the call up */
	/* a quieted call is not an idle line either: the network still offers it, a lifted handset answers it, and
	 * saying idle would publish a release to every front end */
	if (p->ringing || p->quieted || p->machine_answering || p->fe_answering)
		return PHONE_LINE_RINGING;
	return shown[p->state];
}

/* what the answering machine is doing with the current call */
static int machine_state(const struct phone *p)
{
	if (p->machine_call)
		return PHONE_MACHINE_ANSWERED;
	if (p->machine_answering)
		return PHONE_MACHINE_ANSWERING;
	return PHONE_MACHINE_NONE;
}

static void emit(struct phone *p, enum phone_event_kind kind, unsigned long long now)
{
	struct phone_event ev = { .kind = kind, .line = phone_line_of(p), .call_id = p->call_id, .dir = p->dir,
		.call = (enum call_state)p->call_seen, .machine = machine_state(p), .answered_by = p->answered_by,
		.quiet = p->quieted, .at_ms = now };

	if (p->hooks.event)
		p->hooks.event(p->hooks.ctx, &ev);
}

/* the answering machine's own news: the answer went out, the handset took the call, the call is over */
static void emit_machine(struct phone *p, int why, unsigned long long now)
{
	struct phone_event ev = { .kind = PHONE_EV_MACHINE, .line = phone_line_of(p), .call_id = p->call_id, .dir = p->dir,
		.call = (enum call_state)p->call_seen, .machine = why, .answered_by = p->answered_by,
		.quiet = p->quieted, .at_ms = now };

	if (p->hooks.event)
		p->hooks.event(p->hooks.ctx, &ev);
}

/* the quiet-hours code the handset dialled: what it asked for, for the daemon to keep */
static void emit_dnd(struct phone *p, int manual, unsigned long long now)
{
	struct phone_event ev = { .kind = PHONE_EV_DND, .line = phone_line_of(p), .call_id = p->call_id, .dir = p->dir,
		.call = (enum call_state)p->call_seen, .machine = machine_state(p), .answered_by = p->answered_by,
		.manual = manual, .quiet = p->quieted, .at_ms = now };

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
	p->ring_lcr = 0;
	p->cid_wait_ms = 0;		/* a stop during a pre-ring caller id's gap: no delayed ring start */
	report_line(p, p->step_ms);
	fprintf(log, "  channel %u: ring stopped (%s)\n", ch, why);
	return err;
}

/* build and send one caller-id message for the number (the date and time are the router's clock) */
static int cid_deliver(struct chip *c, struct phone *p, unsigned int ch, const char *number, FILE *log);

static int ring_start(struct chip *c, unsigned int ch, struct phone *p, unsigned long long now, FILE *log)
{
	int err;

	/* the European placement (the FSK message before the first burst): the identity goes out first, the ring
	 * voltage starts CID_PRERING_GAP_MS after the message so the message is not clipped; a lift during the send answers */
	if (p->cfg.cid_prering && p->ring_for == RING_FOR_CALL && p->call && p->call->cid >= CID_BELLCORE
	    && p->call->cid <= CID_ETSI && p->call->number) {
		char number[PHONE_NUMBER_MAX + 1];

		if (!p->call->number(p->call->ctx, number, sizeof(number)) && number[0]) {
			unsigned long long sent_at;

			cid_deliver(c, p, ch, number, log);
			/* the silence is owed after the message's last byte, not after the moment its send began: the
			 * seizure, the mark and the bytes at 1200 baud take longer than CID_PRERING_GAP_MS, so the step's
			 * own time is already past its deadline when the send returns */
			sent_at = clock_ms(c);
			p->ringing = 1;
			report_line(p, now);
			p->ring_since_ms = p->ring_log_ms = now;
			p->burst_seen = 0;
			p->gap_since_ms = 0;
			p->last_lf = 0xff;
			p->ring_lcr = p->lcr & (LCR_RTP | LCR_ON_HOOK);
			p->cid_pending = 0;
			p->cid_wait_ms = sent_at + CID_PRERING_GAP_MS;
			stamped(log, "  channel %u: the caller id goes out before the first ring; the ring starts %u ms after the message\n", ch,
				(unsigned)CID_PRERING_GAP_MS);
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
		p->ring_lcr = p->lcr & (LCR_RTP | LCR_ON_HOOK);
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
	char net[PHONE_NUMBER_MAX + 1], shown[PHONE_NUMBER_MAX + 1];

	/* the modem reports a caller as the network gave it (the national significant number alone here); the international
	 * form comes first so that the national one carries the trunk prefix. The number is bounded by cid_number's own
	 * buffer, so neither conversion can overflow here */
	if (number_from_network(number, p->cfg.region, net, sizeof(net)) || number_national(net, p->cfg.region, shown, sizeof(shown)))
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
	/* the running service's log carries no number (phone.h); a bring-up run's does, as the dtmf branch above */
	if (!p->call->service_log)
		fprintf(log, "  channel %u: caller id, number '%s'\n", ch, shown);
	return cid_send(c, p->board, ch, p->call->cid, msg, (size_t)len, &guard, log);
}

/* the network causes (3GPP TS 24.008, reported by AT+CEER) that mean the dialed number or the network itself is
 * in trouble -- they take the fast congestion cadence; a busy or an absent caller takes the slow busy one */
static int is_congestion(int cause)
{
	return cause == 1 || cause == 3 || cause == 28 || cause == 34 || cause == 38 || cause == 42;
}

/*
 * The ring after a caller id sent between the bursts. The send leaves the line in the pause, forward active (the chip
 * starts a ring burst whenever the line enters the ringing state, so the send never puts a ringing line back). The
 * line is held silent for what is left of the natural pause (the burst's end plus the cadence's own off time) and the
 * cadence resumes there, as the exchange's would: the phone's decoder works the message out in silence.
 */
static int ring_hold(struct chip *c, unsigned int ch, struct phone *p, unsigned long long now, FILE *log)
{
	unsigned long long pause = (p->burst_seen && p->gap_since_ms ? p->gap_since_ms : p->ring_since_ms + RING_ON_MS) + RING_OFF_MS;

	p->burst_seen = 0;
	p->gap_since_ms = 0;
	if (now >= pause) {
		/* the send outlasted the pause: the next burst is due now */
		p->ring_since_ms = p->ring_log_ms = now;
		p->last_lf = 0xff;
		if (ring_set(c, ch, 1, log))
			return -EIO;
		fprintf(log, "  channel %u: ring started (the pause was over when the caller id ended)\n", ch);
	} else {
		p->cid_wait_ms = pause;
		fprintf(log, "  channel %u: the ring rests %llu ms more (the caller id ended inside the pause)\n", ch,
			elapsed_ms(pause, now));
	}
	return 0;
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
		p->burst_seen = 0;
		p->gap_since_ms = 0;
		fprintf(log, "  channel %u: ring started (after the caller id)\n", ch);
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
		/* the chip's own ring trip with the loop still open: it left the ringing state on the ring current the
		 * line's load draws, not on a lifted handset. No further burst follows, so the ring is stopped and the
		 * cause is said; the call stays offered and a lift still answers it. */
		if (!p->cid_wait_ms && (lf & LF_STATE_MASK) != LF_CODE_RING
		    && (p->ring_lcr & LCR_RTP) && !(p->ring_lcr & LCR_ON_HOOK)) {
			stamped(log, "  channel %u: the chip's ring trip fired before any lift (hook register 0x%02x)\n", ch, p->ring_lcr);
			return ring_stop(c, ch, p, "the chip tripped the ring before any lift", log);
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
			/* a failed send is logged by cid_send; the ring goes on either way */
			(void)cid_deliver(c, p, ch, p->cid_number, log);
			/* what is left of the pause is counted from the message's last byte: the step's own time was
			 * taken before a send of some hundreds of milliseconds, and a deadline out of it promises back the
			 * silence the message already used up */
			if (p->ringing && ring_hold(c, ch, p, clock_ms(c), log))
				return -EIO;
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
 * The modem setup, at every modem open (the daemon's start, a port that came back): no echo of the commands sent here, numeric error
 * codes, the caller's number with the ring and the extended ring report (3GPP TS 27.007: +CLIP, +CRC, +CMEE), then the two audio levels
 * but only the ones that are set. The earpiece level (+CLVL) is written once the modem has said which range it accepts
 * (the test command), the receive gain (+QRXGAIN) as it is. The PCM mode is only read (AT+QDAI?), never written.
 */
static void modem_setup(struct phone *p, unsigned long long now)
{
	char cmd[MODEM_CMD_MAX];

	p->audio_slave = p->audio_rate = MQ_UNKNOWN;
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
	/* the operator is read as digits, so its mobile country code can be read out of it (TS 27.007 +COPS format 0) */
	modem_cmd(p, "AT+COPS=3,0", p->ch, p->log);
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
	p->answered_by = PHONE_ANSWERED_NONE;
	p->fe_via = PHONE_ANSWERED_WEB;
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
	rec.answered_by = p->answered_by;
	p->hooks.call_ended(p->hooks.ctx, &rec);
}

/* the incoming call is over (not listed, or gone): nothing about it is kept */
static void call_over(struct phone *p)
{
	/* the caller quiet hours covered on this call is remembered now, and only now: while a call is listed the
	 * escape must not find that same call, or a caller answers its own ring. The number comes from the stage, not
	 * from p->peer - the caller id can arrive after the call was first listed, and the list clearing wipes p->peer
	 * before the call is closed */
	if (p->quiet_stage_epoch) {
		snprintf(p->quiet_peer, sizeof(p->quiet_peer), "%s", p->quiet_stage_peer);
		p->quiet_epoch = p->quiet_stage_epoch;
		p->quiet_stage_peer[0] = 0;
		p->quiet_stage_epoch = 0;
	}
	p->incoming = p->offered = p->refused = p->blocked = p->wait_promoted = p->recall = 0;
	p->quieted = 0;			/* the quiet caller and its time stay: the escape outlives the call */
	p->quiet_said = 0;		/* the next call is a new one to speak about */
	p->quiet_mode = DND_MODE_SILENT;
	p->recall_active = p->released_one = 0;
	p->tries = 0;
	p->machine_failed = 0;
	/* the per-call front-end flags go with the call; fe_call does not, because it belongs to the call in
	 * progress and is cleared only where that call genuinely ends */
	p->fe_answering = 0;
	p->fe_ata_ok = 0;
	p->fe_failed = 0;
	p->fe_dial = 0;
}

/* the answering machine's flags off; true when the call was the machine's (the front end then finishes its message).
 * An answer that never reached the call list is dropped here, said once and without an event. */
static int machine_clear(struct phone *p)
{
	const int was = p->machine_call;

	if (!p->machine_call && !p->machine_answering)
		return 0;
	if (!p->machine_call)
		stamped(p->log, "  channel %u: the answering machine's answer did not reach the call\n", p->ch);
	p->machine_call = 0;
	p->machine_answering = 0;
	p->machine_ata_ok = 0;
	return was;
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
 * attach here):
 *   handset down  -> offered: the phone rings, and the lift answers it (ring trip);
 *   handset up    -> the line is in use: the caller is released as busy and the phone does not ring. A line that is off-hook
 *                    is busy for a caller (the exchange does the same: ITU-T Q.31 / E.182 busy; 3GPP TS 22.030 user
 *                    determined user busy). AT+CHUP on a call that is only incoming rejects it (TS 27.007 +CHUP); which
 *                    cause the network shows the caller is TO-VERIFY on this modem. A second call during a call (call
 *                    waiting, TS 22.083 / AT+CHLD) is not decided here: the call list sees only the first voice call.
 * Never answered by itself: a handset that was already up when the call came does not answer it. The answering
 * machine is the one deliberate exception, and only for an offered call with the handset down (on_call_state).
 */
/*
 * Quiet hours, judged every step while the network lists the call. It is judged again rather than once because the
 * caller's identity arrives with the caller id, after the call is first listed: a call whose number was not known
 * yet is quieted, and the step that learns the number rings it if that caller is an emergency number, is on the
 * always-ring list, or is the same caller trying again inside the escape. The escape looks only at calls that have
 * gone, so a caller never answers its own repeat: this call's own silence is staged, and call_over commits it when
 * the call ends. A window that asked for a refusal releases the caller through the same path the blocking rules
 * use. Nothing here drives or stops the handset: the caller of the ring below reads the verdict this leaves behind.
 */
static void quiet_judge(struct phone *p, unsigned int ch, FILE *log)
{
	struct dnd_now now;
	int mode = DND_MODE_SILENT, reason = DND_R_NOT_QUIET;

	p->quieted = 0;
	p->quiet_mode = DND_MODE_SILENT;
	if (!p->cfg.dnd)
		return;
	now = p->cfg.dnd->now;
	/* the caller of the call that went last, if this is that same caller. An empty number never matches the
	 * other (number_same, pinned in tests/test_numbers.c), so a withheld caller leaves nothing to answer, and
	 * dnd_repeat_due reads 0 as nothing remembered */
	now.last_peer_epoch = number_same(p->quiet_peer, p->peer, p->cfg.dnd->region) ? p->quiet_epoch : 0;
	if (dnd_quiet(p->cfg.dnd, p->peer, &now, &mode, &reason)) {
		p->quieted = 1;
		p->quiet_mode = mode;
		/* hold this call's silence until it ends. A withheld caller is staged as an empty number, and an
		 * unknown clock stages 0, which is the value that means nothing remembered */
		snprintf(p->quiet_stage_peer, sizeof(p->quiet_stage_peer), "%s", p->peer);
		p->quiet_stage_epoch = now.epoch_s;
		if (mode == DND_MODE_REJECT) {
			p->refused = p->rec_refused = 1;
			stamped(log, "  channel %u: an incoming call was refused: the line is quiet\n", ch);
			return;
		}
		/* the plain silence is the only quiet that leaves no sound and no answer behind, so it leaves a line: said once
		 * per call, because the same verdict is worked out again every step while the network lists it. A refusal and
		 * the machine's answer each say themselves what became of the call. */
		if (mode == DND_MODE_SILENT && !p->quiet_said) {
			p->quiet_said = 1;
			stamped(log, "  channel %u: quiet hours hold the phone off the incoming call\n", ch);
		}
	} else {
		p->quiet_stage_peer[0] = 0;
		p->quiet_stage_epoch = 0;
	}
}

static void incoming_policy(struct phone *p, int hook_off, FILE *log, unsigned int ch)
{
	if (p->wait_promoted) {
		/* the call that waited during a call: the user has been told of it and the blocking rules have let it through */
		p->offered = 1;
		return;
	}
	/* blocking first: the caller is released as soon as the list shows the call, before any ring; an emergency number is exempt
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
 * is kept here; what a second leg means for the line is the call waiting and hold code below.
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
	rec.answered_by = w->answered_by;
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
	p->answered_by = w->answered_by;
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
 * The waiting call: a second incoming call while a call is going on, listed with stat 5. It is offered (the waiting tone,
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
	if (w->open && p->leg[listed].stat == LEG_ACTIVE && !w->answered_ms) {
		w->answered_ms = now;
		w->answered_by = p->flash_via;	/* a flash took it: the wait record says which door */
	}
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
		} else if (p->machine_call) {
			/* nobody is at the handset to take it, and the tone would only play into the caller's message: the waiting
			 * caller is released as the line in use (AT+CHLD=0 leaves the machine's call alone) */
			w->refused = 1;
			stamped(p->log, "  channel %u: a waiting call was refused: the answering machine has the line\n", p->ch);
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

/* the waiting tone: a short burst now and then on the tone oscillators, over the voice path, and only to a person
 * at the handset (a front-end call has nobody there to hear it); cadence and the oscillator's effect on speech are TO-VERIFY */
static void waiting_tone(struct phone *p, unsigned long long now)
{
	const struct wait_call *w = &p->wait;
	const int live = w->open && !w->refused && !w->answered_ms && p->state == LINE_CALL && wait_listed_waiting(p) && p->hw.off_hook;
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
 * What a flash can do now: take the waiting call, put the one call on hold (and get the dial tone for a second call),
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
	if (phone_flash_resume_ok(phone_line_of(p)) && held >= 1 && !active)
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
	w->answered_by = p->answered_by;	/* the door that answered it goes with the record */
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

	/* AT+CLCC is a snapshot (3GPP TS 27.007 7.18), not an event: right after ATA a modem can list no call for
	 * one poll and the answered call on the next. A live call that disappears is therefore asked about again at
	 * once, once, before the release is taken -- the same rule for every way a call can end. */
	if (st == CALL_GONE || st == CALL_NONE) {
		if (p->call_seen != CALL_NONE && p->call_seen != CALL_GONE && !p->gone_polls) {
			p->gone_polls = 1;
			p->poll_due_ms = p->step_ms;
			return err;
		}
	} else {
		p->gone_polls = 0;
	}
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
		/* a call that has just appeared is judged for quiet hours before it is announced: the announcement is the
		 * event a front end rings its own phones on, and a verdict worked out later in the same step reaches it too
		 * late to hold them back. The branch below judges the call again once the caller is known. */
		if (st == CALL_INCOMING && !p->incoming)
			quiet_judge(p, ch, log);
		emit(p, PHONE_EV_CALL, now);
		if (st == CALL_NONE) {
			p->call_id = 0;
			p->dir = PHONE_DIR_NONE;
			p->call_since_ms = p->answered_ms = 0;
			p->peer[0] = 0;
		}
	}
	if (st == CALL_NONE || st == CALL_GONE)
		p->ending = 0;		/* the call list has caught up with the hang-up sent here */
	if (p->ringing && !p->ring_end_ms && st != CALL_INCOMING && !(p->recall && st != CALL_NONE && st != CALL_GONE))
		(void)ring_stop(c, ch, p, "the call is no longer incoming", log);
	if (st == CALL_ACTIVE && p->state != LINE_CALL && !p->ending && !p->hw.off_hook
	    && (p->fe_answering || p->fe_dial)) {
		/* the front end's answer or dial reached the call list: the line is in a call nobody at the handset
		 * hears. The ring is already off and the audio is the front end's */
		const int was_in = p->dir == PHONE_DIR_IN;

		p->fe_answering = 0;
		p->fe_dial = 0;
		p->fe_call = 1;
		p->answered_by = p->fe_via;
		set_line(p, LINE_CALL, now);
		if (was_in)
			p->calls_in++;
		call_over(p);
		stamped(log, "  channel %u: the front end has the call\n", ch);
	} else if (st == CALL_ACTIVE && p->state != LINE_CALL && p->hw.cleared && !p->hw.off_hook && !p->ending
	    && !p->machine_answering && !p->fe_call && !p->fe_answering && !p->fe_dial
	    && !(p->nlegs > 1 && (legs_with(p, LEG_HELD) || legs_with(p, LEG_WAITING)))
	    && !p->released_one && !p->recall) {
		/* the call connected after the handset went down (the answer was sent, then the caller hung up): end it.
		 * Not while the answering machine's answer is on its way: that call is the machine's (the branch below).
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
		p->answered_by = PHONE_ANSWERED_HANDSET;	/* the handset is on the call now */
		set_line(p, LINE_CALL, now);
		if (p->dir == PHONE_DIR_IN)
			p->calls_in++;
		call_over(p);
	} else if (st == CALL_ACTIVE && p->state != LINE_CALL && p->machine_answering && !p->ending && !p->hw.off_hook) {
		/* the answering machine's answer came through: the line is in a call nobody at the handset hears. The ring
		 * is already off; the front end starts the greeting from the ANSWERED event and records the caller */
		p->machine_call = 1;
		p->machine_answering = 0;
		p->answered_by = PHONE_ANSWERED_MACHINE;
		set_line(p, LINE_CALL, now);
		if (p->dir == PHONE_DIR_IN)
			p->calls_in++;
		call_over(p);
		stamped(log, "  channel %u: the answering machine has the call\n", ch);
		emit_machine(p, PHONE_MACHINE_ANSWERED, now);
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
		const int machine_was = machine_clear(p);
		const int fe_was = p->fe_call || p->fe_answering || p->fe_dial;

		if (machine_was) {
			/* the machine's call is over: the line goes quiet at once (nobody at the handset waits for a tone),
			 * and the front end finishes the message */
			p->cause = p->cause_pending = p->cause_want = 0;
			call_over(p);
			set_line(p, LINE_IDLE, now);
			stamped(log, "  channel %u: the answering machine's call ended\n", ch);
			emit_machine(p, PHONE_MACHINE_ENDED, now);
		} else if (fe_was) {
			/* the front end's call is over: the line goes quiet at once, with no release tone into an empty
			 * handset; the record keeps who answered it */
			p->fe_call = 0;
			p->fe_answering = 0;
			p->fe_ata_ok = 0;
			p->fe_dial = 0;
			p->cause = p->cause_pending = p->cause_want = 0;
			call_over(p);
			set_line(p, LINE_IDLE, now);
			stamped(log, "  channel %u: the front end's call ended\n", ch);
		} else if (p->state == LINE_SETUP || p->state == LINE_CALL) {
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
			quiet_judge(p, ch, log);	/* the identity, the clock and the window, as they stand this step */
			/* the answering machine: an offered call left alone for the delay is answered by the line itself.
			 * Never a call that waited for the user's flash, or one ringing back from hold: the user was already
			 * told of those. The delay counts from the call's first listing; 0 answers at once, without a ring. */
			/* a silent window leaves the call alone; a machine window hands it to the machine */
			if (p->cfg.machine && !(p->quieted && p->quiet_mode != DND_MODE_MACHINE)
			    && !hook_off && p->offered && !p->recall && !p->wait_promoted
			    && !p->machine_answering && !p->machine_failed && !p->fe_answering && !p->fe_call
			    && p->state == LINE_IDLE && !p->ring_end_ms
			    && elapsed_ms(now, p->rec_start_ms) >= p->cfg.machine_delay_ms) {
				p->machine_answering = 1;
				p->machine_ata_ok = 0;
				p->answered_by = PHONE_ANSWERED_MACHINE;
				if (p->ringing)
					(void)ring_stop(c, ch, p, "the answering machine answers", log);
				stamped(log, "  channel %u: the answering machine answers (%llu ms after the call came)\n", ch,
					elapsed_ms(now, p->rec_start_ms));
				emit(p, PHONE_EV_MACHINE, now);
			}
			/* quiet hours: the call is offered and a lift answers it, but nothing on this side is driven */
			if (!hook_off && !p->machine_answering && !p->fe_answering && !p->ringing && p->ring_ready && p->call->ring && !p->ring_end_ms
			    && !p->quieted
			    && p->state == LINE_IDLE && !(err = ring_start(c, ch, p, now, log)))
				stamped(log, "  channel %u: the phone rings for the incoming call\n", ch);
			/* a handset lifted after the call was offered answers it; one who is dialling keeps the line */
			if (hook_off && p->offered && p->state <= LINE_DIAL_TONE && p->tries < CALL_TRY_MAX)
				answer_incoming(p, ch, log);
			/* once the modem has accepted the answer the list is only catching up: no retry, no ring */
			if (p->machine_answering && !p->machine_ata_ok) {
				if (p->tries < CALL_TRY_MAX) {
					answer_incoming(p, ch, log);
				} else {
					/* the modem never took the answer: the call goes back to the handset, and the machine leaves it alone
					 * (it does not stop the ring again for a call it cannot answer) */
					p->machine_answering = 0;
					p->machine_failed = 1;
					stamped(log, "  channel %u: the answering machine could not answer: the phone rings on\n", ch);
				}
			}
			/* the front end's answer on the same retry gate; when the modem never takes it the call is the
			 * handset's again and the ring returns at the next list */
			if (p->fe_answering && !p->fe_ata_ok) {
				if (p->tries < CALL_TRY_MAX) {
					answer_incoming(p, ch, log);
				} else {
					p->fe_answering = 0;
					p->fe_failed = 1;
					p->answered_by = PHONE_ANSWERED_NONE;	/* nobody answered after all */
					stamped(log, "  channel %u: the front end could not answer: the phone rings on\n", ch);
				}
			}
		}
	} else if (st == CALL_NONE) {
		const int machine_was = machine_clear(p);
		const int fe_was = p->fe_call || p->fe_answering || p->fe_dial;

		call_over(p);
		if (machine_was) {
			/* a list that drops the call without the gone step still brings the line back to idle */
			set_line(p, LINE_IDLE, now);
			stamped(log, "  channel %u: the answering machine's call ended\n", ch);
			emit_machine(p, PHONE_MACHINE_ENDED, now);
		} else if (fe_was) {
			/* the list dropped the front end's call without the gone step: the line is free at once, no tone */
			p->fe_call = 0;
			set_line(p, LINE_IDLE, now);
			stamped(log, "  channel %u: the front end's call ended\n", ch);
		} else if (p->state == LINE_CALL || (p->state == LINE_SETUP && p->setup_said)) {
			/* the list dropped the call without the gone step (3GPP TS 27.007 lists only the calls it has):
			 * a connected call the handset was on, or an outgoing attempt the list had already shown being set
			 * up (a list that never listed a fresh dial is the modem catching up, not a release). The line
			 * hears the release tone (ITU-T E.182) instead of silence. */
			(void)osc_stop_once(c, ch, log);
			p->gone_from = p->state;
			p->cause = 0;
			p->cause_pending = 1;
			p->cause_want = 1;
			release_tone_start(p, now);
			set_line(p, LINE_RELEASE, now);
		}
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
			if (p->fe_dial) {
				/* the front end's attempt: no tone into the empty handset, the line is free at once, and the
				 * record says the call never came through */
				p->fe_dial = 0;
				p->cause = p->cause_pending = 0;
				p->rec_cause = 0;
				p->rec_gone_ms = now;
				record_close(p, now);
				p->call_id = 0;
				p->dir = PHONE_DIR_NONE;
				p->call_since_ms = p->answered_ms = 0;
				p->peer[0] = 0;
				set_line(p, LINE_IDLE, now);
				stamped(log, "  channel %u: the network refused the front end's dial (%d: %s)\n", ch, res, why);
				emit(p, PHONE_EV_CALL, now);
				return;
			}
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
		if (!res && p->machine_answering)
			p->machine_ata_ok = 1;		/* the machine's answer is taken: the list follows, and no second answer goes out */
		if (!res && p->fe_answering)
			p->fe_ata_ok = 1;		/* the same accepted answer serves the front end's answer */
	} else if (!strcmp(p->mq_cmd, "AT+CHUP")) {
		if (!res) {
			stamped(log, "  channel %u: hang-up sent\n", ch);
		} else {
			/* a refused hang-up is not lost: the call list still shows the call, and the next query sends it again */
			p->ending = 0;
			stamped(log, "  channel %u: the hang-up was refused (%d: %s); it is sent again at the next call list\n", ch, res, why);
		}
	} else if (!strncmp(p->mq_cmd, "AT+QDAI=", 8) && p->hd_stage == HD_QDAI_WRITE) {
		p->hd_stage = HD_READBACK;
		if (res)
			stamped(log, "  channel %u: the PCM rate write was refused (%d: %s): the modem is restarted as it is\n", ch, res, why);
		modem_cmd(p, "AT+CFUN=1,1", ch, log);	/* the reply never comes: the modem restarts and the port is found again */
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
	/* the wish survives the lost port: the restart that took it away is usually the service's own AT+CFUN, and the modem's
		   answer when the port returns is exactly what the exchange is waiting for. The asks are bounded, so a
		   port that never comes back cannot keep asking. */
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
	if (p->machine_call || p->machine_answering) {
		const int machine_was = machine_clear(p);

		/* the machine's call died with the port: no tone for an empty handset, straight back to idle */
		call_over(p);
		set_line(p, LINE_IDLE, now);
		if (machine_was) {
			stamped(log, "  channel %u: the answering machine's call ended\n", ch);
			emit_machine(p, PHONE_MACHINE_ENDED, now);
		}
		return;
	}
	if (was != LINE_CALL && was != LINE_SETUP)
		return;
	/* a call nobody can control any more is over for the caller: the release tone says the line is free */
	(void)osc_stop_once(c, ch, log);
	p->gone_from = was;
	p->cause = p->cause_pending = 0;
	release_tone_start(p, now);
	set_line(p, LINE_RELEASE, now);
}

/* how soon the call list is asked for again: fast while the call changes (set-up, a hang-up sent from here, just answered), else once a second */
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
		char head[8];

		keep_known(&p->audio_slave, modemq_audio_slave(text));
		keep_known(&p->audio_rate, modemq_audio_rate(text));
		if (modemq_qdai_head(text, head, sizeof(head)) && !p->qdai_head[0])
			memcpy(p->qdai_head, head, sizeof(p->qdai_head));	/* the first field, written back as the modem prints it */
		if (p->audio_slave == 0 && !p->audio_warned) {
			/* master mode is not a fault the operator has to fix by hand: the write the HD exchange below owns
			   carries the slave bit with the wanted rate, and the restart that follows is what applies it */
			p->audio_warned = 1;
			p->hd_want = 1;
			p->hd_gave_up = 0;
			p->hd_waiting = 0;
			p->hd_asks = 0;
			p->hd_stage = HD_IDLE;
			stamped(p->log, "  channel %u: the modem is not in slave mode (AT+QDAI?): the slave write is owed once the line is quiet\n", p->ch);
		}
		/* the rate the modem puts on the bus is kept here for the status and for the exchange below. Nothing is
		   decided from it: the question is read-only, and the answer may land after the handset was lifted -
		   a bring-up must never be owed against a line somebody is using. */
		if (p->hd_stage == HD_READBACK && p->audio_rate != MQ_UNKNOWN) {
			/* the modem came back at the rate it was not asked for: the exchange is over for this round and
			   the idle step decides whether to ask once more or to leave it */
			p->hd_stage = HD_IDLE;
		}
	} else if (!strcmp(cmd, "AT+CLVL=?")) {
		clvl_range_known(p, text);
	} else if (!strcmp(cmd, "AT+CPIN?")) {
		keep_known(&p->sim_ready, modemq_sim_ready(text));
	} else if (!strcmp(cmd, "AT+CEREG?")) {
		int reg = modemq_registered(text);

		keep_known(&p->registered, reg);
		if (reg == 0) {
			p->mcc[0] = 0;		/* off the network: the plan in force stays, the code is nothing until it camps again */
			p->netname[0] = 0;
			p->mcc_named = 0;
			p->mcc_warned = 0;
		}
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
		char net[MQ_NETWORK_MAX], band[MQ_NETWORK_MAX], mcc[MQ_MCC_MAX];

		modemq_network(text, net, sizeof(net), band, sizeof(band));
		if (net[0]) {
			memcpy(p->network, net, sizeof(net));
			memcpy(p->band, band, sizeof(band));
		}
		/* the same network in the modem's own query, where this one carries the PLMN as digits; it says nothing
		 * when the standard +COPS? read already gave them */
		if (modemq_operator(text, "+QNWINFO:", 1, mcc, sizeof(mcc), NULL, 0) == 1) {
			memcpy(p->mcc, mcc, sizeof(mcc));
			p->mcc_named = 0;
			p->mcc_warned = 0;
		} else if (!p->mcc[0] && p->mcc_named && !p->mcc_warned) {
			p->mcc_warned = 1;
			stamped(p->log, "  channel %u: the modem names its network '%s': no digits to choose a numbering plan from\n",
				p->ch, p->netname);
		}
	} else if (!strcmp(cmd, "AT+COPS?")) {
		char mcc[MQ_MCC_MAX], name[MQ_NETWORK_MAX];
		int verdict = modemq_operator(text, "+COPS:", 2, mcc, sizeof(mcc), name, sizeof(name));

		if (verdict == 1) {
			memcpy(p->mcc, mcc, sizeof(mcc));
			p->mcc_named = 0;
			p->mcc_warned = 0;
		} else if (verdict == 0) {
			/* the network as the modem names it. This board answers the standard read with a name where the numeric
			 * form was asked for: the digits then come from its own query above, and the name is kept for the page
			 * and for the line that says no digits arrived from either */
			p->mcc_named = 1;
			snprintf(p->netname, sizeof(p->netname), "%s", name);
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

/* the number a front end may dial: what the keypad can send, plus a leading + for which the keypad has no key;
 * a lone star or hash is no dial string there either */
static int dial_number_ok(const char *number)
{
	size_t n, k;

	if (!number)
		return 0;
	n = strlen(number);
	if (!n || n > DIAL_NUMBER_MAX)
		return 0;
	if ((number[0] == '*' || number[0] == '#') && n == 1)
		return 0;
	for (k = 0; k < n; k++) {
		const char d = number[k];

		if ((d >= '0' && d <= '9') || d == '*' || d == '#')
			continue;
		if (d == '+' && k == 0)
			continue;
		return 0;
	}
	return 1;
}

/*
 * The quiet-hours code, from whichever door it arrives: the line answers it, nothing goes to the network, and the
 * daemon - which owns the clock and the windows - is told on the event. An incoming call needs the network, the wish
 * does not, so this is the one ask the line answers with no modem to talk to. The handset that dialled it is handed
 * its tone back; the typed code comes with the handset down and the line idle, where there is nothing to hand over.
 */
static void quiet_ask(struct phone *p, enum dnd_code code, unsigned int ch, unsigned long long now, FILE *log)
{
	int manual = code == DND_CODE_ON ? DND_MANUAL_ON : DND_MANUAL_OFF;

	p->dial[0] = 0;
	p->dial_last_ms = 0;
	if (p->state == LINE_DIALLING)
		set_line(p, has_modem(p) ? LINE_DIAL_TONE : LINE_NO_SERVICE, now);
	stamped(log, "  channel %u: the phone's code asks for quiet hours %s\n", ch,
		manual == DND_MANUAL_ON ? "on" : "off");
	emit_dnd(p, manual, now);
}

/* the collected digits go to the network as ATD...; (3GPP TS 27.007); the bridge already carries both ways, so
 * the speech opens by itself when the remote answers. A dial the network refuses takes the release tone */
static void dial_out(unsigned int ch, struct phone *p, unsigned long long now, FILE *log)
{
	char cmd[5 + DIAL_NUMBER_MAX + 1];
	enum dnd_code code = dnd_code_of(p->dial);

	/* the codes the handset dials for itself are answered before anything else: they are not a dial and they go
	 * nowhere. A code collected here is the handset's own ask, so no front end is in the line's hand. */
	if (code != DND_CODE_NONE) {
		quiet_ask(p, code, ch, now, log);
		return;
	}
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
		if (p->fe_dial)
			stamped(log, "  channel %u: the front end asked for a blocked number: no call is made\n", ch);
		else
			stamped(log, "  channel %u: a blocked number was dialled: the congestion tone, no call is made\n", ch);
		p->dial[0] = 0;
		p->dial_last_ms = 0;
		if (p->fe_dial) {
			/* nothing was dialled: the line stays idle and silent, and phone_dial() reports the refusal */
			p->fe_dial = 0;
			return;
		}
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

/* the line holds nothing a dial would have to share: no call in any hand, no dial half-typed, nobody at the handset */
static int line_free_for_dial(const struct phone *p)
{
	return p->state == LINE_IDLE && !p->call_id && !p->incoming && !p->offered && !p->rec_open &&
		!p->hw.off_hook && !p->ringing && !p->machine_answering && !p->fe_answering && !p->fe_call && !p->dial[0];
}

/* a front end dials a number the line would dial from the keypad: the same path, with the handset down */
int phone_dial(struct phone *p, const char *number, enum phone_answered_by via)
{
	enum dnd_code code;

	if (!dial_number_ok(number))
		return -EINVAL;
	code = dnd_code_of(number);
	/* a quiet-hours code typed on the page is the ask the handset's keys make, so it goes to the same door and is
	 * answered the same way - off network too, because a wish about this line's bell needs no call control. The
	 * guards a dial needs still hold, so a code never lands on a line already busy. It is not a dial and it leaves no
	 * dialling in the line's hand: the code answers here rather than through dial_out, which is where a front-end
	 * flag would otherwise be set and stay set, and every call after it read as the page's own. */
	if (code != DND_CODE_NONE) {
		if (!line_free_for_dial(p))
			return -EBUSY;
		quiet_ask(p, code, p->ch, clock_ms(p->c), p->log);
		return 0;
	}
	if (!has_modem(p) || !service_ok(p))
		return -ENODEV;
	if (!line_free_for_dial(p))
		return -EBUSY;
	snprintf(p->dial, sizeof(p->dial), "%s", number);
	p->fe_dial = 1;
	dial_out(p->ch, p, clock_ms(p->c), p->log);
	p->fe_via = via;	/* the dial began a call and call_begin reset the door: the dialler's stands after it */
	return p->fe_dial ? 0 : -EPERM;		/* the blocked branch dials nothing and clears the flag */
}

/* the outgoing call's progress tones on the board's own tone preset (the dial tone's oscillators): the ringing
 * tone from the alerting until the answer, the busy or the congestion tone after a release before the answer,
 * the ITU-T E.180 cadences on the clock; a release tone stops after half a minute so the waiting state returns */
#define CAUSE_WAIT_MS	1200u	/* the release tone waits this long for AT+CEER, then the busy tone is its fallback */

static void progress_tick(struct chip *c, unsigned int ch, struct phone *p, unsigned long long now, FILE *log)
{
	unsigned long long on, pos;
	int want;

	if (p->state == LINE_RELEASE && p->cause_pending) {
		if (elapsed_ms(now, p->prog_start_ms) < CAUSE_WAIT_MS)
			return;				/* the cause decides the cadence: the tone waits for it, briefly */
		/* no cause came (the query is stuck or the modem is silent): ITU-T E.182 still wants a release tone, and
		 * the busy tone is its fallback */
		p->cause = 0;
		p->cause_pending = 0;
		stamped(log, "  channel %u: the release cause did not come: the busy tone\n", ch);
	}
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
	if (p->fe_dial || (p->fe_call && !p->hw.off_hook)) {
		/* the front end's call: the peer hears the network's own ringback through the audio path, and the
		 * handset is not in the call, so no local tone plays */
		if (p->prog_on == 1 && !osc_stop_once(c, ch, log))
			p->prog_on = 0;
		return;
	}
	if (p->state == LINE_SETUP && (p->net_tone || p->cfg.ringback == PHONE_RINGBACK_OFF)) {
		if (p->prog_on == 1 && !osc_stop_once(c, ch, log))	/* the network's audio (or the option) has the ringing: the local tone is off, mid-burst too */
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
 * is the rule. With one: the modem port is open and answering, and a network carries voice — the EPS path (+CEREG 1/5, the
 * VoLTE and CSFB route) or the IMS one (+QCFG="ims", the 5G route: on a 5G SA network the EPS answer legitimately stays
 * "not registered" and the call goes out as a network-triggered fallback). An unknown answer counts as service so that a lift
 * right after the start is not refused. ITU-T E.180: the congestion tone says the call cannot be set up.
 */
static int service_ok(const struct phone *p)
{
	if (!p->call || !p->call->begin)
		return 1;
	return has_modem(p) && p->poll_bad < POLL_BAD_MAX && (p->registered != 0 || p->ims == 1);
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
		stamped(log, "  channel %u: no service (%s): the congestion tone\n", ch,
			has_modem(p) ? "no network carries voice yet" : "no modem");
		p->info_next = info_index("AT+CEREG?");
		p->info_due_ms = now;
		return;
	}
	if (osc_start_once(c, ch, log)) {
		p->tone_retry_ms = now + TONE_RETRY_MS;		/* a start that failed is tried again, but not at every read */
		return;
	}
	set_line(p, LINE_DIAL_TONE, now);
	stamped(log, "  channel %u: dial tone on (%s, hook reg 0x%02x)\n", ch,
		(lcr & LCR_ON_HOOK) ? "the handset is up" : "the handset is down: recall dial tone", lcr);
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
	const int machine_was = machine_clear(p);

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
	if (machine_was) {
		stamped(log, "  channel %u: the answering machine's call ended\n", ch);
		emit_machine(p, PHONE_MACHINE_ENDED, p->step_ms);
	}
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
		/* the answering machine gives way the moment the handset comes up: an answer still on its way is the
		 * caller's again, and an answered machine call carries on with the person now listening (the front end
		 * stops the greeting and the message from the TAKEN event) */
		{
			const int answered = (p->machine_answering && p->machine_ata_ok) || (p->fe_answering && p->fe_ata_ok);

			if (p->machine_answering) {
				p->machine_answering = 0;
				stamped(log, "  channel %u: the handset came up while the answering machine was answering: the call is the caller's\n", ch);
			}
			if (p->machine_call) {
				p->machine_call = 0;
				stamped(log, "  channel %u: the handset takes the answering machine's call\n", ch);
				emit_machine(p, PHONE_MACHINE_TAKEN, now);
			}
			if (p->fe_answering) {
				/* the handset wins the offered call: the answer on its way is the caller's now, and the lift
				 * below answers at once if nothing went out yet */
				p->fe_answering = 0;
				p->answered_by = PHONE_ANSWERED_HANDSET;
				stamped(log, "  channel %u: the handset came up while the front end was answering: the call is the handset's\n", ch);
			}
			if (p->fe_call && p->state == LINE_CALL) {
				/* the handset takes the front end's *active* call over: the call goes on, now with the person
				 * listening, and the front end is told of the new owner. A call on hold is not taken here: the
				 * lift gets the recall dial tone, and the held call stays the page's until it is taken back */
				p->fe_call = 0;
				p->answered_by = PHONE_ANSWERED_HANDSET;
				stamped(log, "  channel %u: the handset takes the front end's call\n", ch);
				emit(p, PHONE_EV_CALL, now);
			}
			/* the ring trip: a handset lifted while an offered call is incoming answers it now, not at the next call list
			 * (ETSI EN 300 001: the called party's answer is the loop closing). Not when the machine's answer is already
			 * accepted: that would put a second ATA on the call. The machine's failed tries are not the person's: the lift
			 * answers once more, while a call whose own answers were refused keeps its count (the machine is the only one
			 * that retries, so its flag is the only reason to reset) */
			if (p->machine_failed && p->incoming && p->offered && p->tries >= CALL_TRY_MAX)
				p->tries = 0;
			if (!answered && p->incoming && p->offered && p->tries < CALL_TRY_MAX && p->state <= LINE_DIAL_TONE) {
				answer_incoming(p, ch, log);
			}
			present_dial_tone(c, ch, p, lcr, log);
		}
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
			p->flash_via = PHONE_ANSWERED_HANDSET;
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
	if (p->ringing) {
		const uint8_t bits = lcr & (LCR_RTP | LCR_ON_HOOK);

		if (bits != p->ring_lcr) {
			stamped(log, "  channel %u: hook register 0x%02x -> 0x%02x while ringing%s\n", ch, p->ring_lcr, bits,
				(bits & LCR_RTP) && !(bits & LCR_ON_HOOK) ? " (the chip's ring trip)" :
				(bits & LCR_ON_HOOK) ? " (the loop closed)" : "");
			p->ring_lcr = bits;
		}
	}
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
 * Speed dial: two digits and the send key dial the first number of the contact that has that code. The same two
 * digits without the send key are an ordinary number that goes out after the pause. A number that is an emergency
 * number as dialled is never replaced and a stored number too long for the dial buffer is not used.
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
		/* the wish alone moves nothing: the chip's band, the PCM slots and the bridge are one layout and the modem
		   decides whether 16 kHz is even on the bus. The exchange below asks the modem; the layout follows its word. */
		p->hd_want = 1;
		p->hd_gave_up = 0;
		p->hd_waiting = 0;
		p->hd_asks = 0;
		p->hd_stage = HD_IDLE;
		stamped(p->log, "  channel %u: HD voice %s: the modem's PCM rate is asked first, the line follows its answer\n",
			p->ch, p->pending_cfg.hd ? "on" : "off");
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
	p->sim_ready = p->registered = p->ims = p->rsrp_dbm = p->audio_slave = p->audio_rate = p->clir = p->clir_status = p->ccwa_net = MQ_UNKNOWN;
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
	struct wideband wb;
	int err, leave, fed = 0, det_on = 0, ring_ok = 0, bridge_up = 0;

	memset(&wb, 0, sizeof(wb));

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
	if (fed && !err && cfg->hd && (err = wideband_apply(c, ch, &wb, 1, log)) != 0)
		return err;			/* the wideband registers did not take: the line is closed and brought up again */
	if (fed && !err && hooks && hooks->bridge_start) {
		err = hooks->bridge_start(hooks->ctx, BRIDGE_MODEM_RX, BRIDGE_MODEM_TX, BRIDGE_LINE_RX, BRIDGE_LINE_TX,
			cfg->hd ? PHONE_BRIDGE_WIDEBAND : 0);
		if (err) {
			fprintf(log, "  FAILED on channel %u: bridge start (%d)\n", ch, err);
		} else {
			bridge_up = 1;	/* the bring-up keeps the path up: a layout that cannot cross samples is said here,
					 * at the same moment it has always been said, and the watch lets it down when the
					 * line turns out to be quiet (phone_audio_apply) */
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
			p->wb = wb;		/* the wideband state was applied before the phone existed: it moves into it here */
			p->hd_inforce = cfg->hd ? 1 : 0;	/* the layout the hardware was actually brought up with, whatever the settings wish */
			p->audio_up = bridge_up;
			p->audio_starts = bridge_up;
			p->audio_need_ms = p->audio_up_ms = c->ops->now_ms(c->ctx);
		}
	}
	if (!err) {
		*out = p;
		return 0;
	}
	if (bridge_up && hooks->bridge_stop)		/* a bring-up that died halfway must not leave the samples crossing for nobody */
		(void)hooks->bridge_stop(hooks->ctx);
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

/*
 * The audio path's life, in one place. The bridge crosses the modem's samples and the line chip's, so it is work
 * worth doing while something exchanges them and noise when nothing does: a lifted handset with nothing behind it is
 * dial tone and digits, which the chip makes and the modem queue carries. The reason comes back as one fixed word
 * because the log and the health reply must say which of them is holding the path open, and a stranger reading the
 * page must be able to tell "the line is quiet" from "the audio path is broken".
 */
static const char *audio_reason(const struct phone *p)
{
	if (p->audio_claims & PHONE_AUDIO_RECORDER)
		return "recording";
	if (p->audio_claims & PHONE_AUDIO_MESSAGE)
		return "message";
	if (p->audio_claims & PHONE_AUDIO_PAGE)
		return "page";
	if (p->audio_claims & PHONE_AUDIO_FRONT_END)
		return "front end";
	if (p->machine_call || p->machine_answering)
		return "machine's call";
	if (p->fe_call || p->fe_answering || p->fe_dial)
		return "front end";
	if (p->nlegs || p->wait.open || p->recall)	/* the modem lists a call: active, held, dialling, alerting, incoming or waiting */
		return "call";
	if (p->call_id || p->incoming)		/* the line holds a call the list has not caught up with, or one is being offered */
		return "call";
	return NULL;
}

/*
 * Bring the path up while a reason exists, and let it down AUDIO_GRACE_MS after the last one went. The grace is what
 * makes this safe rather than clever: a hook flash, a call list catching up, a held call coming back and a second
 * call taking over all leave the line for a moment or two, and a restart inside one of those is a gap a person
 * hears. A start that fails is tried again after AUDIO_RETRY_MS and said once - it never fails the step, because the
 * handset, the tones, the digits and the call control all work with the crossing down.
 */
static void audio_update(struct phone *p, unsigned long long now)
{
	const char *why = audio_reason(p);
	int err;

	if (!p->hooks.bridge_start)
		return;				/* this front end has no audio device to manage (the stage tool installs no bridge) */
	if (why) {
		p->audio_need_ms = now;
		if (p->audio_up || now < p->audio_retry_ms)
			return;
		err = p->hooks.bridge_start(p->hooks.ctx, BRIDGE_MODEM_RX, BRIDGE_MODEM_TX, BRIDGE_LINE_RX, BRIDGE_LINE_TX,
			p->hd_inforce ? PHONE_BRIDGE_WIDEBAND : 0);
		if (err) {
			p->audio_retry_ms = now + AUDIO_RETRY_MS;
			p->audio_error = err;
			p->audio_fails++;
			if (!p->audio_error_said) {
				p->audio_error_said = 1;
				stamped(p->log, "  channel %u: the audio path did not come up for the %s (%d): it is tried again\n",
					p->ch, why, err);
			}
			return;
		}
		p->audio_up = 1;
		p->audio_starts++;
		p->audio_up_ms = now;
		p->audio_retry_ms = 0;
		if (p->audio_error) {
			stamped(p->log, "  channel %u: the audio path came up for the %s after %u failed tries\n",
				p->ch, why, p->audio_fails + 1);
			p->audio_error = 0;
			p->audio_fails = 0;
		}
		p->audio_error_said = 0;
		stamped(p->log, "  channel %u: the audio path is up for the %s\n", p->ch, why);
		return;
	}
	if (!p->audio_up || now < p->audio_need_ms + AUDIO_GRACE_MS || now < p->audio_retry_ms)
		return;
	if (!p->hooks.bridge_stop)
		return;				/* the front end cannot stop what it has no way to stop: the path stays as it was */
	err = p->hooks.bridge_stop(p->hooks.ctx);
	if (err) {
		p->audio_retry_ms = now + AUDIO_RETRY_MS;
		p->audio_error = err;
		p->audio_fails++;
		if (!p->audio_error_said) {
			p->audio_error_said = 1;
			stamped(p->log, "  channel %u: the audio path did not stand by (%d): it is tried again\n", p->ch, err);
		}
		return;
	}
	p->audio_up = 0;
	p->audio_stops++;
	p->audio_retry_ms = 0;
	p->audio_error = 0;
	p->audio_error_said = 0;
	stamped(p->log, "  channel %u: the audio path stands by: nothing needs the samples (%llu s up)\n",
		p->ch, (now > p->audio_up_ms ? now - p->audio_up_ms : 0) / 1000);
}

void phone_audio_claim(struct phone *p, unsigned int bit, int on)
{
	if (on)
		p->audio_claims |= bit;
	else
		p->audio_claims &= ~bit;
}

int phone_audio_apply(struct phone *p, unsigned long long now)
{
	audio_update(p, now);
	if (!audio_reason(p) || !p->hooks.bridge_start)
		return 0;
	return p->audio_up ? 0 : (p->audio_error ? p->audio_error : -EAGAIN);
}

void phone_audio_view_of(const struct phone *p, struct phone_audio_view *v)
{
	const char *why = audio_reason(p);

	memset(v, 0, sizeof(*v));
	v->reason = why ? why : "idle";
	v->up = p->audio_up;
	v->wanted = why != NULL;
	v->claims = p->audio_claims;
	v->since_ms = p->audio_up && p->step_ms >= p->audio_up_ms ? p->step_ms - p->audio_up_ms : 0;
	v->starts = p->audio_starts;
	v->stops = p->audio_stops;
	v->last_error = p->audio_error;
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

		if (p->audio_rate == MQ_UNKNOWN) {
			/* the modem has not answered with its rate yet, and the idle questions are the ones that ask:
			   restarting a modem that may already be at the wanted rate would drop the mobile data for
			   nothing, so the change waits for that one answer */
			if (!p->hd_waiting) {
				p->hd_waiting = 1;
				stamped(p->log, "  channel %u: HD voice %s: the modem's PCM rate has not been read yet; the change waits for it\n",
					p->ch, p->cfg.hd ? "on" : "off");
			}
		} else if (p->audio_rate == (p->cfg.hd ? 1 : 0) && p->audio_slave != 0) {
			/* the modem already keeps the wanted rate - it persists in the modem across its restarts - so
			   there is nothing to ask and no data to drop: the line is what owes the bring-up. A modem in
			   master mode is asked anyway (the slave bit above is what makes the voice path work), which is
			   why the rate matching alone is not enough to leave it alone */
			p->hd_want = 0;
			p->hd_layout_due = 1;
			stamped(p->log, "  channel %u: the modem already answers %d kHz: the line is brought up at that rate\n",
				p->ch, p->cfg.hd ? 16 : 8);
		} else if (p->hd_asks < HD_ASK_MAX) {
			p->hd_asks++;
			if (p->audio_slave == 0)
				stamped(p->log, "  channel %u: the modem's PCM stays master: the slave write goes out and the modem restarts (mobile data drops for about half a minute)\n", p->ch);
			else
				stamped(p->log, "  channel %u: HD voice %s: the modem's PCM rate changes and the modem restarts (mobile data drops for about half a minute)\n",
					p->ch, p->cfg.hd ? "on" : "off");
			snprintf(cmd, sizeof(cmd), "AT+QDAI=%s,1,0,4,0,%d,1,1", p->qdai_head[0] ? p->qdai_head : "x", p->cfg.hd ? 1 : 0);
			p->hd_stage = HD_QDAI_WRITE;
			modem_cmd(p, cmd, p->ch, p->log);
		} else if (!p->hd_gave_up) {
			/* a modem that answers the other rate is not asked again: the line keeps working at the rate it
			   has, and the setting is taken up again by the next change or the next start */
			p->hd_gave_up = 1;
			p->hd_want = 0;
			if (p->audio_slave == 0)
				stamped(p->log, "  channel %u: the modem keeps its PCM master mode: the voice path cannot work until the next settings change or service start\n", p->ch);
			else
				stamped(p->log, "  channel %u: the modem is left at %d kHz: HD voice waits for the next settings change\n",
					p->ch, p->audio_rate == MQ_UNKNOWN ? 0 : (p->audio_rate ? 16 : 8));
		}
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
	if (!err)
		audio_update(p, now);		/* last, after every state above settled: the audio follows what the line is
						 * really doing, and a path that cannot come up never fails the step */
	return err;
}

int phone_close(struct phone *p, int err)
{
	struct chip *c = p->c;
	unsigned int ch = p->ch;
	FILE *log = p->log;
	int det_on = p->det_on, leave;

	err = phone_watch_end(p, err);
	if (p->audio_up && p->hooks.bridge_stop) {
		/* whoever brought the samples crossing up brings them down again: the thread does not outlive the line it
		 * was started for, so a bring-up at another layout never finds one already running */
		int b_err = p->hooks.bridge_stop(p->hooks.ctx);

		p->audio_up = 0;
		p->audio_stops++;
		fprintf(log, "  channel %u: the audio path is stopped with the line\n", ch);
		if (b_err && !err)
			err = b_err;
	}
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
	if (p->state == LINE_RELEASE)
		return -ENOENT;			/* the network already released it: nothing is left to hang up */
	if (!p->call_id && !p->incoming && p->state != LINE_SETUP && p->state != LINE_CALL)
		return -ENOENT;
	p->ending = 1;
	modem_cmd(p, "AT+CHUP", p->ch, p->log);
	return 0;
}

/* a front end answers an offered call: the ring stops and the answer goes out from the call-list step, as the
 * machine's does, so that a refusal can be retried there */
int phone_answer(struct phone *p, enum phone_answered_by via)
{
	unsigned long long now;

	if (p->machine_answering || p->fe_answering || p->fe_call || p->hw.off_hook)
		return -EBUSY;			/* somebody already has it, or the line is not free for it */
	if (!p->incoming || !p->offered || p->refused || !p->call_id || p->dir != PHONE_DIR_IN)
		return -ENOENT;			/* nothing is ringing to answer */
	if (p->fe_failed)
		p->tries = 0;			/* the failed tries of an earlier request are not this one's */
	p->fe_answering = 1;
	p->fe_failed = 0;
	p->fe_via = via;
	p->answered_by = via;
	now = clock_ms(p->c);
	p->step_ms = now;
	p->poll_due_ms = now;			/* the answer goes out at the next step, not after a poll interval */
	if (p->ringing)
		(void)ring_stop(p->c, p->ch, p, "the front end answers", p->log);
	emit(p, PHONE_EV_CALL, now);
	return 0;
}

/*
 * A front end takes the call the answering machine holds: the same door a lift at the handset opens (hook_machine),
 * from the other side. Nothing goes to the modem - the call is already answered and on the line; only its listener
 * changes. The machine's own side closes on the TAKEN event with the message it has so far, and the recording is the
 * call's, not the machine's, so it carries on. Whoever already has the line is asked first: a person at the handset
 * or a front end that took the call a moment ago keeps it, and a second door does not move the caller twice.
 */
int phone_takeover(struct phone *p, enum phone_answered_by via)
{
	unsigned long long now;

	if (p->hw.off_hook || p->fe_call || p->fe_answering)
		return -EBUSY;			/* a person at the phone or a front end already has this call */
	if (!p->machine_call)
		return -ENOENT;			/* the machine holds nothing to hand over: none came, or its answer is still on its way */
	now = clock_ms(p->c);
	p->machine_call = 0;
	p->fe_call = 1;
	p->fe_via = via;
	p->answered_by = via;
	stamped(p->log, "  channel %u: the front end takes the answering machine's call\n", p->ch);
	emit_machine(p, PHONE_MACHINE_TAKEN, now);
	emit(p, PHONE_EV_CALL, now);
	return 0;
}

/* one digit on the cellular leg of a live call, whichever side is listening */
int phone_dtmf(struct phone *p, char digit)
{
	if (!p->call_id || p->state != LINE_CALL)
		return -ENOENT;
	if (!((digit >= '0' && digit <= '9') || digit == '*' || digit == '#' || (digit >= 'A' && digit <= 'D')))
		return -EINVAL;
	/* the key after a flash is a code (TS 22.030 in-call codes), as it is at the handset; a key that is no code
	 * goes on to the network as an ordinary tone */
	if (p->flash_open && flash_code(p, digit))
		return 0;
	p->flash_open = 0;
	send_key(p, digit, p->ch, p->log);
	return 0;
}

/* A front end's flash: what the handset's flash key does in a call, hold, swap, take the waiting call or take the
 * held call back. The flash alone acts at the window's end (flash_alone); a digit within FLASH_CODE_MS is a code. */
int phone_flash(struct phone *p)
{
	struct hookwatch_cfg hk;
	unsigned int width = FLASH_SOFT_MS;

	if (!has_modem(p))
		return -ENODEV;
	if (flash_use(p) == FLASH_NOTHING)
		return -ENOENT;
	/* the break the button stands in for sits inside the hook's own flash window, as a person's would */
	hk = hook_limits(p);
	if (width < hk.flash_min_ms)
		width = hk.flash_min_ms;
	if (width >= hk.flash_max_ms)
		width = hk.flash_max_ms - 1;
	p->flash_open = 1;
	p->flash_until_ms = clock_ms(p->c) + FLASH_CODE_MS;
	p->flash_via = PHONE_ANSWERED_WEB;
	stamped(p->log, "  channel %u: hook flash of %u ms (the page)\n", p->ch, width);
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

/* the layout is owed once the modem has answered with the rate the setting asks for, and only while the line is quiet */
int phone_layout_due(struct phone *p, int *want)
{
	if (want)
		*want = p->cfg.hd ? 1 : 0;
	if (!p->hd_layout_due)
		return 0;
	return p->state == LINE_IDLE && !p->hw.off_hook && !p->ringing && !p->incoming && p->call_seen == CALL_NONE;
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
	st->machine = machine_state(p);
	st->quiet = p->quieted;
	st->answered_by = p->answered_by;
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
	st->audio_rate = p->audio_rate == MQ_UNKNOWN ? PHONE_UNKNOWN : p->audio_rate;
	st->signal_dbm = p->rsrp_dbm == MQ_UNKNOWN ? PHONE_DBM_UNKNOWN : p->rsrp_dbm;
	st->fwd = p->fwd_net;
	if (!st->fwd.done)
		st->fwd.network = PHONE_UNKNOWN;
	st->call_waiting_net = p->ccwa_net == MQ_UNKNOWN ? PHONE_UNKNOWN : p->ccwa_net;
	st->clir = p->clir == MQ_UNKNOWN ? PHONE_UNKNOWN : p->clir;
	st->clir_status = p->clir_status == MQ_UNKNOWN ? PHONE_UNKNOWN : p->clir_status;
	snprintf(st->network, sizeof(st->network), "%s", p->network);
	snprintf(st->mcc, sizeof(st->mcc), "%s", p->mcc);
	snprintf(st->netname, sizeof(st->netname), "%s", p->netname);
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
