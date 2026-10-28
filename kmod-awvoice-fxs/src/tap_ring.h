/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AWFXS_TAP_RING_H
#define AWFXS_TAP_RING_H

/*
 * Single-producer / single-consumer frame ring of the capture tap. The producer is the bridge thread (one frame per
 * PCM frame, at FIFO-low priority beside the audio path: it must never wait on a lock, so the ring is lock-free and
 * the indices move with release/acquire ordering). The consumer is the ioctl reader; the module mutex keeps readers
 * serialised. Full rings drop the arriving frame and count it — a reader that cannot keep up loses the newest audio,
 * never the ordering of what it already has. Plain arithmetic on purpose: tested on the host (tests/test_tap.c).
 */
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/awvoice-fxs.h>	/* struct awfxs_tap_frame: the ring's element is the tap ioctl's frame */
#define TAP_STORE_RELEASE(p, v)	smp_store_release(p, v)
#define TAP_LOAD_ACQUIRE(p)	smp_load_acquire(p)
#else
#include <stdint.h>
typedef uint16_t u16;
typedef uint32_t u32;
/* the same layout the UAPI header declares for the kernel's side */
struct awfxs_tap_frame {
	u16 modem;		/* what the modem sent this frame */
	u16 line;		/* what the line chip sent; 0 in LOOP mode */
	u16 modem2;		/* wideband second sample; 0 when narrowband */
	u16 line2;
};
#define TAP_STORE_RELEASE(p, v)	(*(p) = (v))
#define TAP_LOAD_ACQUIRE(p)	(*(p))
#endif

struct awfxs_tap_ring {
	struct awfxs_tap_frame *slot;	/* capacity is mask + 1, a power of two */
	u32 mask;
	u32 head;		/* producer: next frame to write */
	u32 tail;		/* consumer: next frame to read */
	u32 drops;		/* frames discarded when the ring was full */
};

static inline void tap_ring_reset(struct awfxs_tap_ring *r, struct awfxs_tap_frame *slot, u32 capacity)
{
	r->slot = slot;
	r->mask = capacity - 1u;
	r->head = 0;
	r->tail = 0;
	r->drops = 0;
}

/* producer; true when stored, false when the ring was full and the frame was counted in drops */
static inline int tap_ring_push(struct awfxs_tap_ring *r, const struct awfxs_tap_frame *f)
{
	u32 head = r->head;

	if ((u32)(head - TAP_LOAD_ACQUIRE(&r->tail)) > r->mask) {
		r->drops++;
		return 0;
	}
	r->slot[head & r->mask] = *f;
	TAP_STORE_RELEASE(&r->head, head + 1u);
	return 1;
}

/* consumer: copies at most max frames out, returns how many were copied */
static inline u32 tap_ring_pop(struct awfxs_tap_ring *r, struct awfxs_tap_frame *out, u32 max)
{
	u32 tail = r->tail, n = 0;

	while (n < max && tail != TAP_LOAD_ACQUIRE(&r->head)) {
		out[n++] = r->slot[tail & r->mask];
		tail++;
	}
	TAP_STORE_RELEASE(&r->tail, tail);
	return n;
}

/* frames sitting in the ring right now */
static inline u32 tap_ring_stored(const struct awfxs_tap_ring *r)
{
	return TAP_LOAD_ACQUIRE(&r->head) - r->tail;
}

#endif /* AWFXS_TAP_RING_H */
