/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_SLIC_RING_H
#define AW_SLIC_RING_H

#include <stdio.h>

#include "board.h"
#include "chip.h"

/*
 * Ringing. ring_load writes the ring generator from the board file's preset and our cadence (every word is read
 * back); the caller holds protected mode and the line is fed (forward active). ring_set moves the line between
 * forward active and ringing and reads the state back. Both return 0 or a negative errno.
 */
int ring_load(struct chip *c, const struct board *b, unsigned int ch, FILE *log);
int ring_set(struct chip *c, unsigned int ch, int on, FILE *log);

#endif /* AW_SLIC_RING_H */
