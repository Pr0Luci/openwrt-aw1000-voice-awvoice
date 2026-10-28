/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AWFXS_PLAY_RING_H
#define AWFXS_PLAY_RING_H

/*
 * Single-producer / single-consumer frame ring of the playback path. The roles are the capture tap's the other way
 * round: the producer is the ioctl writer (the daemon's timer, holding the module mutex) and the consumer is the
 * bridge thread. The bridge must never wait for the writer, so the ring is lock-free and the indices move with
 * release/acquire ordering. A full ring refuses the arriving frame and counts it — the writer learns from `written`
 * and `dropped` — and an empty ring is the consumer's to notice: it sends silence, never an old frame again.
 * Plain arithmetic on purpose: tested on the host (tests/test_play.c). TO-VERIFY on the line: the underrun count
 * under a loaded bridge and the one-frame recovery after it (the host test proves the ring, not the audio clock).
 */
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/awvoice-fxs.h>	/* struct awfxs_play_frame: the ring's element is the playback ioctl's frame */
#define PLAY_STORE_RELEASE(p, v)	smp_store_release(p, v)
#define PLAY_LOAD_ACQUIRE(p)		smp_load_acquire(p)
#else
#include <stdint.h>
typedef uint16_t u16;
typedef uint32_t u32;
/* the same layout the UAPI header declares for the kernel's side */
struct awfxs_play_frame {
	u16 modem;		/* what the modem hears this frame */
	u16 line;		/* what the line chip hears this frame */
	u16 modem2;		/* wideband second sample; ignored when narrowband */
	u16 line2;
};
#define PLAY_STORE_RELEASE(p, v)	(*(p) = (v))
#define PLAY_LOAD_ACQUIRE(p)		(*(p))
#endif

struct awfxs_play_ring {
	struct awfxs_play_frame *slot;	/* capacity is mask + 1, a power of two */
	u32 mask;
	u32 head;		/* producer: next frame to write */
	u32 tail;		/* consumer: next frame to read */
	u32 drops;		/* frames refused when the ring was full */
};

static inline void play_ring_reset(struct awfxs_play_ring *r, struct awfxs_play_frame *slot, u32 capacity)
{
	r->slot = slot;
	r->mask = capacity - 1u;
	r->head = 0;
	r->tail = 0;
	r->drops = 0;
}

/* producer; true when stored, false when the ring was full and the frame was counted in drops */
static inline int play_ring_push(struct awfxs_play_ring *r, const struct awfxs_play_frame *f)
{
	u32 head = r->head;

	if ((u32)(head - PLAY_LOAD_ACQUIRE(&r->tail)) > r->mask) {
		r->drops++;
		return 0;
	}
	r->slot[head & r->mask] = *f;
	PLAY_STORE_RELEASE(&r->head, head + 1u);
	return 1;
}

/* consumer: copies at most max frames out, returns how many were copied (0 when the ring is empty) */
static inline u32 play_ring_pop(struct awfxs_play_ring *r, struct awfxs_play_frame *out, u32 max)
{
	u32 tail = r->tail, n = 0;

	while (n < max && tail != PLAY_LOAD_ACQUIRE(&r->head)) {
		out[n++] = r->slot[tail & r->mask];
		tail++;
	}
	PLAY_STORE_RELEASE(&r->tail, tail);
	return n;
}

/* frames sitting in the ring right now */
static inline u32 play_ring_stored(const struct awfxs_play_ring *r)
{
	return PLAY_LOAD_ACQUIRE(&r->head) - r->tail;
}

#endif /* AWFXS_PLAY_RING_H */
