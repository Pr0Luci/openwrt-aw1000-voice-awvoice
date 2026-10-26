/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_SLIC_SCRIPT_H
#define AW_SLIC_SCRIPT_H

#include <stdio.h>

#include "board.h"
#include "chip.h"

/*
 * A script is a board-file section made of 8-byte records (little endian): operation, channel, address, value.
 * It carries a sequence of chip accesses that is vendor-provided data and therefore lives in the user's board file,
 * not in this repository; the interpreter below knows nothing about what a script is for.
 *
 *   'r' write register          'R' read register and require `value`
 *   'm' write RAM word          'M' read RAM word and require `value`
 *   'u' protected mode on (value 1) or off (value 0)
 *
 * The whole script is checked before the chip is touched. It stops at the first access that fails or reads back a different value,
 * and protected mode is switched off again on both channels whatever happened. The log names the record number, never its contents.
 */
#define SCRIPT_RECORD_SIZE	8

int script_run(struct chip *c, const struct board_section *script, FILE *log);

/* Run the board-file section `id` as a script. */
int stage_script(struct chip *c, const struct board *b, const char *id, FILE *log);

#endif /* AW_SLIC_SCRIPT_H */
