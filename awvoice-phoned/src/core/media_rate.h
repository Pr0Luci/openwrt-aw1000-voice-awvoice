// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#ifndef AW_PHONED_MEDIA_RATE_H
#define AW_PHONED_MEDIA_RATE_H

#include <stdint.h>

#include "front_end.h"

/* The media socket carries 16000 Hz call audio as 8000 Hz, one 20 ms frame at a time. Down, each pair of the call's
 * samples becomes one of the wire's, and their average is the two-tap filter that keeps the pair's band inside the
 * 4 kHz the wire has. Up, one wire sample is held for the two the ring plays: nothing above 4 kHz can be restored, and
 * the hold leaves the wire's own band as it was. */

static inline void media_rate_down16(const int16_t *in, int16_t *out)
{
	u32 k;

	for (k = 0; k < FE_SAMPLES_8K; k++)
		out[k] = (int16_t)(((int)in[2 * k] + (int)in[2 * k + 1]) / 2);
}

static inline void media_rate_up16(const int16_t *in, int16_t *out)
{
	u32 k;

	for (k = 0; k < FE_SAMPLES_8K; k++)
		out[2 * k] = out[2 * k + 1] = in[k];
}

#endif
