/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONE_CORE_LINE_H
#define AW_PHONE_CORE_LINE_H

#include <stdint.h>
#include <stdio.h>

#include "board.h"
#include "chip.h"

/* The line's own operations, shared by the call logic (call.c) and the stage tool's stages 5..7 (internal to the core). */

int32_t line_battery_mv(uint32_t raw);
/* Read the battery voltage (now) and the converter's expected high voltage (both in millivolts). */
int read_battery(struct chip *c, unsigned int ch, int32_t *now, int32_t *expected);
int read_iloop(struct chip *c, unsigned int ch, int32_t *ua);

int osc_start_once(struct chip *c, unsigned int ch, FILE *log);
int osc_stop_once(struct chip *c, unsigned int ch, FILE *log);

/* the log line with its monotonic stamp, so a session's timing is read from the log alone */
void stamped(FILE *log, const char *fmt, ...);
unsigned long long clock_ms(struct chip *c);
/* the time from `since` to `now`; a time taken a moment after the step began is not before it (never a negative time) */
unsigned long long elapsed_ms(unsigned long long now, unsigned long long since);

int fed_line(struct chip *c, unsigned int ch, uint32_t off_bit, FILE *log);
int release_line(struct chip *c, unsigned int ch, int err, FILE *log);
int voice_up(struct chip *c, const struct board *board, unsigned int ch, FILE *log);
int tone_up(struct chip *c, unsigned int ch, const struct board_section *imped, const struct board_section *tones, FILE *log);
int tone_down(struct chip *c, unsigned int ch, int err, FILE *log);
int irq2_enable(struct chip *c, unsigned int ch, uint8_t bits, FILE *log);
int irq2_disable(struct chip *c, unsigned int ch, uint8_t bits, FILE *log);
int dtmf_enable(struct chip *c, unsigned int ch, FILE *log);
int dtmf_disable(struct chip *c, unsigned int ch, FILE *log);

#endif /* AW_PHONE_CORE_LINE_H */
