/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_TESTS_FAKES_H
#define AW_PHONED_TESTS_FAKES_H

/*
 * The fake chip and the fake modem of the host tests: the stage tool's (tools/slic/test_slic.c) and the daemon's
 * (packages/awvoice-phoned/tests/test_daemon.c) tests drive the same models.
 */
#include <stdint.h>
#include <stddef.h>

#include "board.h"
#include "chip.h"
#include "slic_map.h"
#include "stages.h"

#define CHANNELS 2

#define WINDOW_POINTER	1358
#define WINDOW_DATA	1359

#define LCR_SCRIPT_MS 125		/* an entry of a hook script covers this much paused time (two per 250 ms sample) */

#define TEST_CH		1
#define TEST_HIGH_MV	60000
#define TEST_OITHRESH	0x2468

struct model {
	uint8_t reg[2][128];
	uint32_t ram[2][2048];
	unsigned int busy_polls;	/* how many status reads still show busy */
	int user[2], step[2], pending[2];	/* pending: data bytes were written since the last RAM access */
	int writes_ram;
	uint8_t id;
	uint8_t mstrstat;	/* the chip's ready state the reads report; the tests force another value for the dead-chip start */
	int ram_sticks;		/* 0: RAM refuses to hold values */
	int frames;
	unsigned int ram_write_addr[2][64], ram_write_count[2];
	/* battery voltage as the converter would report it: one value per read of RAM 3, the last one repeats */
	int32_t vbat_mv[400];
	unsigned int vbat_count, vbat_reads;
	int32_t iloop_ua[400];			/* the phone's loop current readings, one per read of RAM 10 */
	unsigned int iloop_count, iloop_reads;
	uint8_t lcr_bits[1024];			/* the chip's hook states, one per read of register 34 */
	unsigned int lcr_count, lcr_reads;
	unsigned long vtime_ms;			/* the time the tool has paused so far */
	unsigned long lcr_origin;
	int lcr_started;
	unsigned int lcr_ms;			/* paused time per hook-script entry; 0 = LCR_SCRIPT_MS */
	/* ordered record of the writes that are not part of the RAM window mechanics: 'm' RAM word, 'r' register */
	struct { char kind; unsigned int ch, addr; uint32_t value; unsigned long at; } log[512];
	unsigned int log_count;
	unsigned int cal_busy_polls;	/* reads of the calibration status that still show busy after a start; 0 = done at once */
	unsigned int cal_pending[2];
	int cal_stuck;			/* the busy flag never clears */
	unsigned int cal_reads;
	/* the chip's sequential RAM window: a write at the pointer register restarts both pointers, each access then moves on one word */
	uint32_t win[2][512];
	unsigned int win_w[2], win_r[2];
	unsigned int ram_or_addr;	/* read-back of this RAM word always has these extra bits set (bits the chip sets itself) */
	uint32_t ram_or_value;
	unsigned int ram_forced_addr;	/* read-back of this RAM word always shows ram_forced_value */
	uint32_t ram_forced_value;
	int lf_read_value;		/* the line feed register always reads back this state; -1 = it holds what was written */
	int dtmf_at;			/* the tone-detector event fires this many ms after the first hook read (the hold's start); 0 = none */
	int flag_at;			/* the chip's loop-change flag latches this many ms after the first hook read, until register 19 is read; 0 = none */
	int flag_set;
	int lcr_prev;			/* the hook state of the previous read: every change latches the loop-change flag */
	int flag_every;			/* the flag latches again this many ms after it fired; 0 = once */
	unsigned int frames_per_ms;	/* the chip's accesses take time: the clock advances by one ms per this many frames (0 = they take none) */
	int lcr_fail_ms, lcr_fail_end;	/* reads of the hook register fail from this many ms after the first one until the end time (0 = for good); 0 = never */
	unsigned int irq2_reads;
	/* scripted key presses: `at' ms after the first hook read the event fires and register 60 answers with the
	 * code until the next one */
	struct { unsigned int at; uint8_t code; } dtmf_events[16];
	unsigned int dtmf_event_count, dtmf_next;
};


extern const struct chip_ops model_ops;
extern const uint32_t TW[CW_COUNT];

void init_model(struct model *m);
size_t make_board_with(uint8_t *out, int with_general, uint32_t first_word, const uint32_t *converter);
size_t make_board(uint8_t *out, int with_general, uint32_t first_word);

/* a board file with optional converter, dc feed, impedance and tone sections (stages 5 and 6 need no general section) */
struct test_board {
	int with_converter, with_dcfeed, with_imped, with_tone, with_iir, with_ring;
	uint32_t dcfeed_len, imped_word0, tone_word0;
};

size_t make_linefeed_board(uint8_t *out, const struct test_board *t);
void linefeed_model(struct model *m, const int32_t *script, unsigned int count,
	const int32_t *iloop_script, unsigned int iloop_count, const uint8_t *lcr_script, unsigned int lcr_count);

/*
 * A scripted modem behind the non-blocking call ops: the tests give it the old kind of callbacks (the call state of
 * each query, the result of each command, the release cause) and a latency in pumps, the number of times the loop
 * must move a request on before it finishes -- a slow modem. The loop must keep reading the hook meanwhile.
 */
struct fake_modem {
	int (*poll)(void *user, int *st);
	int (*act)(void *user, const char *what);
	int (*cause)(void *user);
	const char *(*text)(void *user, const char *cmd);	/* the reply text of a question (CALL_REQ_INFO); NULL = an empty reply */
	char body[512];
	void *user;
	unsigned int latency;		/* pumps before a request finishes */
	int open;
	enum call_req kind;
	char cmd[64];
	unsigned int left;
	int lost;			/* the port is gone: every pump answers -ENODEV */
	unsigned int lose_after;	/* the port is lost when this many requests have been started; 0 = never */
	int state_timeout;		/* the call list gets no answer (-ETIMEDOUT) */
	unsigned int begins, pumps;
	struct call_leg legs[CALL_LEGS_MAX];	/* the voice calls the call list shows (the tests that use legs fill them with `poll`) */
	unsigned int nlegs;
};

int fm_begin(void *ctx, enum call_req kind, const char *cmd);
int fm_pump(void *ctx, int *result);
const char *fm_body(void *ctx);
unsigned int fm_legs(void *ctx, struct call_leg *out, unsigned int max);

#endif /* AW_PHONED_TESTS_FAKES_H */
