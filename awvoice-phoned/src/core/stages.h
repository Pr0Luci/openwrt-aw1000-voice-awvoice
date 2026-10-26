/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_SLIC_STAGES_H
#define AW_SLIC_STAGES_H

#include <stddef.h>
#include <stdio.h>

#include "board.h"
#include "hookwatch.h"
#include "chip.h"

/*
 * One stage tool's optional call answering (the modem side): the hold loop asks the modem for the call state about
 * once a second (without waiting for the reply) and sends ATA / CHUP / ATD at the standard moments (answer = the caller lifted the handset while a
 * voice call is incoming, hang-up = the handset went down during the call, dial = the caller pressed the digits of
 * a number and the send key). The outgoing side also tracks the network's own progress (dialing, alerting) and, on
 * a release before the answer, asks for the network's release cause.
 */
enum call_state { CALL_NONE, CALL_INCOMING, CALL_ACTIVE, CALL_GONE, CALL_DIALING, CALL_ALERTING, CALL_HELD /* only held calls are listed */ };
/* the modem requests (the same order as enum answer_kind) */
enum call_req { CALL_REQ_STATE, CALL_REQ_CMD, CALL_REQ_CAUSE, CALL_REQ_INFO };
/* One voice call of the call list (AT+CLCC, 3GPP TS 27.007): idx and the stat values 0 active, 1 held, 2 dialling, 3 alerting, 4 incoming, 5 waiting. */
#define CALL_LEGS_MAX	3
enum call_stat { LEG_ACTIVE, LEG_HELD, LEG_DIALING, LEG_ALERTING, LEG_INCOMING, LEG_WAITING };
struct call_leg {
	int idx;			/* the modem's index of the call (1-based) */
	int dir;			/* 0 mobile originated, 1 mobile terminated */
	int stat;			/* enum call_stat */
	int mpty;			/* 1 = part of a multiparty (conference) call */
	char number[33];		/* digits of the remote number, "" when withheld */
};

struct call_ops {
	void *ctx;
	/* The modem is reached without waiting: the hold loop reads the hook every few milliseconds and must never
	 * stand still for a modem. begin starts one request (CALL_REQ_CMD sends cmd: "ATA" / "AT+CHUP" / "ATD<number>;");
	 * 0 = started, -EBUSY = one is still open, -ENODEV = no port, another negative errno = refused. */
	int (*begin)(void *ctx, enum call_req kind, const char *cmd);
	/* pump moves the open request on and never blocks: 0 = nothing finished; 1 = it finished and *result is its
	 * outcome (CALL_REQ_STATE: one of enum call_state or -ETIMEDOUT; CALL_REQ_CMD: 0 when the modem answered OK, else
	 * a negative errno; CALL_REQ_CAUSE: the network's cause, TS 24.008 numbering, 0 = unknown, or -ETIMEDOUT; -ENODEV
	 * for any of them when the port was lost) */
	int (*pump)(void *ctx, int *result);
	const char *(*reply)(void *ctx);	/* the last request's reply, one short line, for the log of a refused command (never holds a number); optional */
	/* CALL_REQ_INFO sends cmd like CALL_REQ_CMD (a read-only question); its result is 0 when the modem answered OK, and body() is the whole reply text
	 * until the next request starts. Optional: without it the line asks no questions. */
	const char *(*body)(void *ctx);
	/* the voice calls of the last call-list answer, at most `max`; the count (0 = none). Optional: without it the line knows one call at most. */
	unsigned int (*legs)(void *ctx, struct call_leg *out, unsigned int max);
	int setup;				/* send the call setup the service needs at the start (AT+CLIP=1: the caller's number, AT+CRC=1: the extended ring report) */
	struct hookwatch_cfg hook_cfg;		/* the flash limits (flash_min_ms, flash_max_ms); zeros = the defaults */
	int ring;				/* ring the phone while a voice call is incoming and the handset is down */
	int cid;				/* send the caller's identity during the ring: enum cid_standard, 0 = off */
	int (*number)(void *ctx, char *out, size_t cap);	/* the incoming call's number (digits), 0 on success */
	int (*name)(void *ctx, const char *number, char *out, size_t cap);	/* the caller's name for a number, 0 when known */
	unsigned int ring_test_s;		/* ring once for this many seconds at the start, without a call (<= RING_TEST_MAX_SECONDS) */
	int service_log;			/* a running service's log: no dialled or caller's number or pressed digit, no ring supply readings, the guard's reading once a minute */
};

/*
 * Initialisation is done in stages, each one safe to stop after (a chip reset undoes all of them) and each one
 * confirmed by reading back what it wrote.
 *   0  identify both channels and prove the register write path with a harmless register
 *   1  load the board's general configuration into the chip's RAM (no converter is started, no line voltage)
 *   2  calibration round A (converter off), then start one channel's battery converter and wait until the battery
 *      voltage tracks (the line stays open, so no voltage reaches tip and ring); a failed start powers the converter down
 *   3  calibration round B on the running converter
 *   4  load the board's dc feed preset into the chip's RAM, both channels, line still open (no line voltage)
 *   5  take the named channel's line from open to forward active (tip and ring are fed); the line is held for the hold
 *      time (samples every 250 ms: battery, loop current and the line state; a battery fall with no loop current puts
 *      the line back open; hold time 0 = until the tool is stopped), then it goes back to open
 *   6  dial tone: the same fed line as stage 5, with the board's impedance preset and the verified 425 Hz oscillator
 *      values loaded, the oscillators started (continuous) for the hold time, then they stop and the line opens
 *   7  dial tone with DTMF detection on top: the stage 6 chain plus the tone detector (its event watched during the
 *      hold; every detected digit is logged), the detector state restored afterwards
 *   8  the voice path: the stage 7 chain plus the chip's codec at our frame bit positions and the chip's own dialing controls
 *      off; with a bridge-start callback (the real run supplies it, the tests pass none) the kernel bridge then moves
 *      the chip's audio to and from the modem during the hold; with call ops the hold also answers an incoming
 *      voice call when the caller lifts the handset and hangs up when it is put back. The outgoing side: the digits
 *      the caller presses after the dial tone are collected, the send key or the end of the pauses sends them as
 *      ATD, the ringing tone plays while the remote is rung, the call is abandoned with CHUP when the handset goes
 *      down before the answer, a release before the answer and a remote end while the caller is still on the line
 *      play the busy or the congestion tone by the network's cause, and the caller's own hang-up brings no tone;
 *      while the caller dials, an incoming call is not answered by itself (no call waiting)
 */

int stage0_identify(struct chip *c, FILE *log);
int stage1_general(struct chip *c, const struct board *b, FILE *log);
int stage2_converter(struct chip *c, const struct board *b, unsigned int ch, FILE *log);
int stage3_calibrate(struct chip *c, const struct board *b, unsigned int ch, FILE *log);
int stage4_dcfeed(struct chip *c, const struct board *b, FILE *log);
int stage5_linefeed(struct chip *c, const struct board *b, unsigned int ch, unsigned int seconds, FILE *log);
int stage6_dialtone(struct chip *c, const struct board *b, unsigned int ch, unsigned int seconds, FILE *log);
int stage7_dtmf(struct chip *c, const struct board *b, unsigned int ch, unsigned int seconds, FILE *log);
int stage8_voice(struct chip *c, const struct board *b,
	int (*bridge_start)(unsigned int modem_rx, unsigned int modem_tx, unsigned int line_rx, unsigned int line_tx),
	const struct call_ops *call, unsigned int ch, unsigned int seconds, FILE *log);

/* the hook sensor's bench: stages 0..4's feed and then a short-step watch of the hook register with monotonic
 * stamps, logging every change with the width of the state before it (a physical toggle's width and what the
 * register itself reports); the summary counts the breaks and the makes with their widths */
int hook_trace_run(struct chip *c, const struct board *b, unsigned int ch, unsigned int seconds, FILE *log);

/* Report a failed step on a channel in the log and pass the error on (never 0). */
int fail_stage(FILE *log, const char *what, unsigned int ch, int err);

#endif /* AW_SLIC_STAGES_H */
