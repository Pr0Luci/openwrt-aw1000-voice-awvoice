// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#ifndef AW_PHONED_PLAY_MIRROR_H
#define AW_PHONED_PLAY_MIRROR_H

#include <stdint.h>

/*
 * The audio the daemon writes toward the modem, kept so the recording can place it where the handset's own
 * channel would have been. The two sides are never in the same place at the same time: the write goes into
 * the playback ring, which the bridge drains one frame per 125 us slot, and the capture tap hands out the
 * slots the bridge has just taken. So a written sample reaches the file only after it has waited exactly the
 * ring's lead here: one entry per time sample, filled by the play feed and emptied by the tap's drain, slot
 * by slot, at the bridge's rate.
 *
 * The producer and the consumer are the daemon's one thread, so no ordering is needed beyond the counts.
 * A full mirror drops the oldest samples and counts them: an overflow says the drain stopped, and silence
 * in the middle of a recording is never the answer to that.
 *
 * Host-tested (tests/test_play_mirror.c).
 */

typedef uint16_t u16;
typedef uint32_t u32;

#define PLAY_MIRROR_SAMPLES	8192u	/* one second of narrowband audio, half a second of wideband: the ring's lead with room to spare */

struct play_mirror {
	u16 slot[PLAY_MIRROR_SAMPLES];
	u32 head;	/* where the next written sample goes */
	u32 tail;	/* where the next drained sample comes from */
	u32 used;	/* samples waiting */
	u32 drops;	/* samples refused when the mirror was full */
};

static inline void play_mirror_reset(struct play_mirror *m)
{
	m->head = m->tail = m->used = 0;	/* drops is not reset: it counts over the daemon's life */
}

static inline void play_mirror_push(struct play_mirror *m, const u16 *s, u32 n)
{
	u32 k;

	if (n > PLAY_MIRROR_SAMPLES) {
		m->drops += n - PLAY_MIRROR_SAMPLES;	/* longer than the mirror: only its newest part can be kept */
		s += n - PLAY_MIRROR_SAMPLES;
		n = PLAY_MIRROR_SAMPLES;
	}
	while (m->used + n > PLAY_MIRROR_SAMPLES) {
		m->tail = (m->tail + 1u) % PLAY_MIRROR_SAMPLES;
		m->used--;
		m->drops++;
	}
	for (k = 0; k < n; k++) {
		m->slot[m->head] = s[k];
		m->head = (m->head + 1u) % PLAY_MIRROR_SAMPLES;
	}
	m->used += n;
}

/* the oldest n samples, or as many as are waiting: the count that came back says whether the caller's side
 * of the file has a hole in it */
static inline u32 play_mirror_pull(struct play_mirror *m, u16 *s, u32 n)
{
	u32 k, got = n < m->used ? n : m->used;

	for (k = 0; k < got; k++) {
		s[k] = m->slot[m->tail];
		m->tail = (m->tail + 1u) % PLAY_MIRROR_SAMPLES;
		m->used--;
	}
	return got;
}

#endif
