/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONE_CORE_PHONE_H
#define AW_PHONE_CORE_PHONE_H

#include <stdio.h>
#include <time.h>

#include "board.h"
#include "chip.h"
#include "forward.h"
#include "hookwatch.h"
#include "stages.h"	/* struct call_ops, enum call_state */

/*
 * The phone line, served: one fed channel with the voice path up, the hook watched, the dial tone and the progress
 * tones, digit collection and dialling, the ring with the caller's identity, and the call control through the modem
 * ops. This is the stage 8 hold of the stage tool cut into open / step / close, so that the stage tool's loop and the
 * daemon's event loop drive the same code.
 *
 * The core does no I/O of its own and never waits: the chip is reached through struct chip (its ops), the modem
 * through struct call_ops (begin / pump, non-blocking), the time is the caller's `now` (monotonic milliseconds),
 * and everything it has to say goes to the log stream and the event hook. phone_step() must be called every
 * HOOK_POLL_MS (5 ms); a late call is logged as a gap and costs nothing else.
 *
 * Threading: one thread owns a struct phone and its chip; no function here may be called from another thread or from
 * a signal handler.
 */

/* What the line is doing (the daemon's status and events; the order is fixed, the API reports it by name). */
enum phone_line {
	PHONE_LINE_IDLE,	/* handset down, nothing going on */
	PHONE_LINE_RINGING,	/* an incoming call rings the phone */
	PHONE_LINE_DIAL_TONE,	/* handset up, the dial tone runs */
	PHONE_LINE_DIALLING,	/* digits are being collected */
	PHONE_LINE_SETUP,	/* the number went out; the network sets the call up (the ringing tone may play) */
	PHONE_LINE_CALL,	/* a call is in progress (two-way speech) */
	PHONE_LINE_RELEASE,	/* the call ended while the handset is up: busy or congestion tone */
	PHONE_LINE_NO_SERVICE	/* handset up while the modem is absent or not registered: congestion tone */
};

enum phone_dir { PHONE_DIR_NONE, PHONE_DIR_IN, PHONE_DIR_OUT };

#define PHONE_NUMBER_MAX	32	/* digits, '*', '#', a leading '+' (the UI's rule) */

/* The local ringing tone: auto plays it from the network's alerting until the network's own audio (its ringback tone or a caller's ringback tune) is heard from the modem, then never again in that call. */
enum phone_ringback { PHONE_RINGBACK_AUTO, PHONE_RINGBACK_ON, PHONE_RINGBACK_OFF };

/* Settings. Zeros mean the defaults, so `struct phone_cfg cfg = { .channel = 1 };` is a valid configuration. */
struct phonebook;
struct alarms;
struct blocking;
struct number_region;

struct phone_cfg {
	unsigned int channel;		/* the line chip's channel with the RJ11 jack (1 on this board) */
	struct hookwatch_cfg hook;	/* flash_min_ms / flash_max_ms (hookwatch_cfg_parse rules) */
	unsigned int dial_pause_ms;	/* the dial goes out this long after the last digit (default DIAL_IDLE_MS, 3000..15000) */
	int cid;			/* enum cid_standard; default CID_DTMF (the variant that works on this board) */
	int no_cid;			/* 1 = caller id off (cid is then ignored) */
	int cid_prering;		/* 1 = the FSK message goes out before the first ring burst (the European placement); the DTMF variants ignore it */
	int hd;				/* 1 = HD voice: the chip's wideband setup and a second sample per frame on the bridge (the modem's PCM rate is the daemon's apply) */
	int ringback;			/* enum phone_ringback: the local ringing tone while the remote rings (default auto) */
	const struct number_region *region;	/* how numbers are compared and which are emergency numbers (NULL = the default region) */
	int call_waiting;		/* offer a second incoming call during a call (AT+CCWA=1); 0 = refuse it with AT+CHLD=0 */
	int conference;			/* allow the join of two calls (flash and 3) */
	int clir_set;			/* write AT+CLIR at every modem setup (3GPP TS 27.007) */
	int clir;			/* the <n> of AT+CLIR=<n>: 0 as the network's subscription says, 1 hide my number, 2 show it */
	char hotline[PHONE_NUMBER_MAX + 1];	/* "" = no hotline; else dialled when the lifted handset dials nothing for hotline_delay_ms (at most DIAL_NUMBER_MAX characters) */
	unsigned int hotline_delay_ms;	/* 3000..99000 */
	const struct phonebook *book;	/* speed-dial codes (NULL = none); owned by the front end, changed only between two steps */
	struct phone_forward forward;	/* the network forwarding to set (forward.configured = 0: never touched) */
	const struct alarms *alarms;	/* alarm calls (NULL = none); owned by the front end, changed only between two steps */
	const struct blocking *block;	/* call blocking rules (NULL = none); owned by the front end, changed only between two steps */
	int service_log;		/* 1 = the running service's log: no digit, number or name ever appears in it */
	int level_trace;		/* 1 = log the modem's audio level every 100 ms while a call is being set up (measuring only) */
	/* the modem's audio levels, written at every modem setup when set (the flags: 0 is a valid level) */
	int volume_set, rx_gain_set;
	int earpiece_volume;		/* AT+CLVL (3GPP TS 27.007), only inside the range the modem reports */
	int modem_rx_gain;		/* AT+QRXGAIN, 0..65535 */
};

/*
 * How a finished call ended, for the call log (the page's contract v2 `result`). The release causes are 3GPP TS 24.008
 * numbers (as AT+CEER reports them): 17 user busy, 18 no user responding, 19 no answer from the user (alerting timeout).
 */
enum phone_result { PHONE_RES_ANSWERED, PHONE_RES_MISSED, PHONE_RES_REJECTED, PHONE_RES_BLOCKED, PHONE_RES_BUSY, PHONE_RES_NO_ANSWER, PHONE_RES_FAILED };

/*
 * incoming: answered; refused by the blocking rules -> blocked; refused while the line was in use -> rejected; else missed.
 * outgoing: refused by the blocking rules (never dialled) -> blocked.
 * outgoing: answered; before the answer cause 17 -> busy, 18 or 19 -> no_answer, anything else (0 = unknown) -> failed.
 */
enum phone_result phone_result_of(enum phone_dir dir, int answered, int refused, int blocked, int cause);

/* the contract's word for a result: answered, missed, rejected, blocked, busy, no_answer, failed */
const char *phone_result_name(enum phone_result r);

enum phone_event_kind { PHONE_EV_LINE, PHONE_EV_CALL };

struct phone_event {
	enum phone_event_kind kind;
	enum phone_line line;		/* the line state after the change */
	unsigned int call_id;		/* 0 = no call; counts up from 1 for each call (in or out) since the start */
	enum phone_dir dir;
	enum call_state call;		/* the modem's view of the call (CALL_NONE when there is none) */
	unsigned long long at_ms;	/* the step's clock time */
};

struct phone_call_record {
	unsigned int id;			/* the call's id (the same as in its events) */
	enum phone_dir dir;
	char number[PHONE_NUMBER_MAX + 1];	/* "" when the network withheld it */
	unsigned long long start_ms;		/* the step clock: the call began (incoming: first seen in the call list; outgoing: dialled) */
	unsigned long long answered_ms;		/* 0 = not answered */
	unsigned long long end_ms;		/* the network released it (or the last time it was seen, or the dial was refused) */
	int cause;				/* TS 24.008, 0 = unknown */
	enum phone_result result;
};

/* The front end's side. Every member is optional (NULL = not used). */
struct phone_hooks {
	void *ctx;
	/* start the kernel audio bridge at these bit positions (AWFXS_BRIDGE_CROSS); 0 or -errno */
	int (*bridge_start)(void *ctx, unsigned int modem_rx, unsigned int modem_tx, unsigned int line_rx, unsigned int line_tx, unsigned int flags);
	/* the modem's audio level now: the peak magnitude of its samples in the last completed 100 ms window and the number of windows so far
	 * (the kernel bridge's counters); 0 or -errno. Called from inside phone_step() at most every 100 ms, only while the measuring option is on. */
	int (*modem_level)(void *ctx, unsigned int *peak, unsigned int *windows);
	/* the local wall-clock time for the alarm calls: 0 and *out, or a negative errno when the clock cannot be trusted (not set to a plausible date).
	 * Called from inside phone_step() every ALARM_CHECK_MS, only when alarms are set; must not block. */
	int (*wall_clock)(void *ctx, struct tm *out);
	/* a state change: called from inside phone_step(), must not call back into the core and must not block */
	void (*event)(void *ctx, const struct phone_event *ev);
	/* a call is over: called once per call from inside phone_step(), with the record for the call log. The record holds the remote
	 * number: it goes only into the call log, never into a log line or an event. Must not call back into the core or block. */
	void (*call_ended)(void *ctx, const struct phone_call_record *rec);
};

#define PHONE_UNKNOWN		(-1)	/* a tri-state field that is not known yet */
#define PHONE_DBM_UNKNOWN	(-1000)

/* What the network's call forwarding is reported to be after the daemon set it (AT+CCFC, read back with the query), for the page. */
struct phone_forward_state {
	int applying;			/* the commands are being exchanged with the network */
	int done;			/* at least one exchange has finished since the start */
	int network;			/* the read-back: 1 active, 0 not active, PHONE_UNKNOWN */
	char number[PHONE_NUMBER_MAX + 1];	/* the target the network reports ("" = none or not told) */
	unsigned int delay_s;		/* the no-reply delay the network reports (0 = none) */
	char error[48];			/* why the network refused: the modem's own status line (never a number), "" = no refusal */
};

/*
 * A snapshot for the status and health API. `number` is the current call's remote number: it is for the
 * access-controlled API reply only (the phone page shows numbers in full); it never
 * goes to the log or to an event. All modem fields are the last answers of the modem's periodic queries.
 */
#define PHONE_LEGS_MAX	3

/* one voice call of the modem's call list (the first is the one the line is about; a second one is a waiting or held call) */
struct phone_leg_status {
	unsigned int id;			/* the call's id (the first leg has the id of the line's call) */
	int stat;				/* enum call_stat */
	char number[PHONE_NUMBER_MAX + 1];	/* for the access-controlled API reply only */
};

struct phone_status {
	enum phone_line line;
	int off_hook;
	unsigned int leg_count;			/* the voice calls the modem lists (0 when none) */
	struct phone_leg_status legs[PHONE_LEGS_MAX];
	unsigned int call_id;
	enum phone_dir dir;
	enum call_state call;
	char number[PHONE_NUMBER_MAX + 1];	/* "" when no call or the network withheld it */
	unsigned long long call_since_ms;	/* when the current call state began (0 = no call) */
	unsigned long long answered_ms;		/* when the call became active (0 = not answered) */
	int modem_up;				/* the modem port is open and answers */
	int sim_ready;				/* AT+CPIN? READY: 1 / 0 / PHONE_UNKNOWN */
	int registered;				/* AT+CEREG? stat 1 or 5: 1 / 0 / PHONE_UNKNOWN */
	int ims_registered;			/* the modem's IMS state (AT+QCFG="ims"): 1 / 0 / PHONE_UNKNOWN */
	int audio_slave;			/* AT+QDAI? second field 1 = the modem is the PCM slave (the bridge needs it): 1 / 0 / PHONE_UNKNOWN */
	int signal_dbm;				/* from AT+CESQ (RSRP) or PHONE_DBM_UNKNOWN */
	int call_waiting_net;			/* AT+CCWA=1,2 read-back: 1 the network has call waiting on, 0 off, PHONE_UNKNOWN */
	int clir;				/* AT+CLIR? setting: 0 network, 1 hide, 2 show, or PHONE_UNKNOWN */
	int clir_status;			/* AT+CLIR? provisioning: 0..4 (see modemq.h) or PHONE_UNKNOWN */
	char network[24];			/* the access technology for display ("4G", "5G", ...), "" when unknown */
	unsigned int modem_timeouts;		/* modem requests that got no reply in time, since the start */
	unsigned long long modem_reply_ms;	/* the clock time of the modem's last reply (0 = none yet) */
	unsigned int faults;			/* failed chip reads and guard trips since the start */
	int battery_mv, loop_ua;		/* the last guard sample */
	unsigned int calls_in, calls_out;	/* answered incoming / dialled outgoing calls since the start */
	unsigned int hook_reads, hook_gap_max_ms, hook_gaps;	/* the step timing record */
	struct phone_forward_state fwd;		/* the network forwarding read-back */
};

struct phone;

/*
 * Bring the line into service: protected mode, the line fed (forward active), the impedance and tone presets, the
 * tone detector, the voice path at our PCM positions, the bridge (hooks->bridge_start), the ring generator. The chip
 * must already have run stages 0..4. On failure everything done so far is undone (line open, detector restored) and
 * *out stays NULL. 0 or -errno.
 */
int phone_open(struct phone **out, struct chip *c, const struct board *b, const struct call_ops *call,
	const struct phone_cfg *cfg, const struct phone_hooks *hooks, FILE *log);

/*
 * One step (every HOOK_POLL_MS): the event register, the hook, the dial pause, the modem client, the ring
 * supervision, the progress tones and the guard sample, in that order (see the stage tool's watch loop history ). 0, or a negative errno after which the line must be closed (three failed chip reads in a row,
 * a guard trip): the caller then calls phone_close() and stops (the daemon exits and is restarted).
 */
int phone_step(struct phone *p, unsigned long long now);

/* 1 while the chip's wideband mode is applied (the HD voice setting in force) */
int phone_hd_active(const struct phone *p);

/* End the current call or call attempt (AT+CHUP through the modem queue). 0, -ENOENT when there is none. */
int phone_hangup(struct phone *p);

/* Ring the idle phone for 1..RING_TEST_MAX_SECONDS without a call. 0; -EBUSY when not idle or off-hook; -EINVAL. */
int phone_ring_test(struct phone *p, unsigned int seconds);

/*
 * New settings (a UCI reload). The hook limits, the dial pause and the caller id apply at the next moment the line
 * is idle with the handset down; a different channel is refused (-EINVAL: the caller closes and opens again).
 */
int phone_set_cfg(struct phone *p, const struct phone_cfg *cfg);

/*
 * The modem port came back (the daemon reopened it after a loss or found it late at boot). The core forgets the
 * lost state, queues the setup commands again and asks for the call list at the next step. A loss itself is seen by
 * the core (a request ends in -ENODEV): phone_status().modem_up goes to 0 and the daemon closes and looks again.
 */
void phone_modem_up(struct phone *p);

/*
 * The line test (the page's U36): three samples 100 ms apart of the hook, the battery and the loop current while the line is idle with the handset
 * down, then a verdict. It runs inside phone_step() (no waiting); start it, then call phone_line_test_poll() until it reports the result.
 *   off_hook      the handset was up in a sample;           no_battery  the battery is below the feed guard (LF_HOLD_GUARD_MV under the converter's value);
 *   interrupted   a call or a ring began during the test;   leak        reserved: the on-hook loop current above LINE_TEST_LEAK_UA. The threshold is
 *                 TO-VERIFY on the line, so until then it is only reported (leak_suspect) and the verdict stays ok. Healthy values: on-hook loop
 *                 current about 0.9 mA, the battery near the converter's value .
 */
enum phone_line_verdict { PHONE_LT_OK, PHONE_LT_OFF_HOOK, PHONE_LT_LEAK, PHONE_LT_NO_BATTERY, PHONE_LT_INTERRUPTED, PHONE_LT_ERROR };

struct phone_line_test_result {
	enum phone_line_verdict verdict;
	int hook_off;				/* the handset was up in a sample */
	int battery_mv, expected_mv;		/* the mean of the samples, and the converter's value */
	int loop_ua;				/* the mean loop current */
	int leak_suspect;			/* the loop current is above LINE_TEST_LEAK_UA (reported only) */
};

#define LINE_TEST_SAMPLES	3
#define LINE_TEST_SPACING_MS	100
#define LINE_TEST_LEAK_UA	2000	/* TO-VERIFY on the line */

/* Start the test. 0; -EBUSY when the line is not idle with the handset down, a ring is on, or a test is already running. */
int phone_line_test(struct phone *p);

/* 1 and the result when the test has finished (the result is handed over once); 0 while it runs or when none was started. */
int phone_line_test_poll(struct phone *p, struct phone_line_test_result *res);

/* the verdict's fixed word for the API: ok, off_hook, leak, no_battery, interrupted, error */
const char *phone_line_verdict_name(enum phone_line_verdict v);

void phone_status(const struct phone *p, struct phone_status *st);

/* The line's state as a fixed lower-case name for the API ("idle", "ringing", "dial_tone", ...). */
const char *phone_line_name(enum phone_line line);

/*
 * Take the line out of service: the ring stops, a call is hung up (best effort, one queued AT+CHUP pumped for at most
 * 500 ms on the caller's clock), the tone detector is restored, the oscillators stop, the line goes open, protected
 * mode is left; frees p. Returns err when it is non-zero, else the first teardown error.
 */
int phone_close(struct phone *p, int err);

#endif /* AW_PHONE_CORE_PHONE_H */
