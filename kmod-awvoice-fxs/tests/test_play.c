// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * Host tests of the playback ring: the same play_ring.h the ioctl writer fills and the bridge thread drains one
 * frame per PCM frame. Run: make -f Makefile.host test
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/play_ring.h"

#define CAPACITY 64

static struct awfxs_play_frame frame_of(u32 n)
{
	struct awfxs_play_frame f = { .modem = (u16)(n * 3 + 1), .line = (u16)(n * 5 + 2),
		.modem2 = (u16)(n * 7 + 3), .line2 = (u16)(n * 11 + 4) };

	return f;
}

static void assert_frame(const struct awfxs_play_frame *f, u32 n)
{
	const struct awfxs_play_frame want = frame_of(n);

	assert(!memcmp(f, &want, sizeof(*f)));
}

/* the writer fills, the bridge takes one frame at a time: order intact, the count follows */
static void test_fifo_order(void)
{
	struct awfxs_play_frame slot[CAPACITY], out[1];
	struct awfxs_play_ring r;
	u32 n;

	play_ring_reset(&r, slot, CAPACITY);
	assert(play_ring_stored(&r) == 0);
	assert(play_ring_pop(&r, out, 1) == 0);		/* empty: the bridge sends silence, it gets no frame */
	for (n = 0; n < CAPACITY; n++) {
		struct awfxs_play_frame f = frame_of(n);

		assert(play_ring_push(&r, &f) == 1);
		assert(play_ring_stored(&r) == n + 1);
	}
	for (n = 0; n < CAPACITY; n++) {
		assert(play_ring_pop(&r, out, 1) == 1);
		assert_frame(&out[0], n);
	}
	assert(play_ring_stored(&r) == 0);
	assert(r.drops == 0);
	printf("play: frames leave in the order the writer stored them, one per frame\n");
}

/* push one more than the capacity: the queued frames survive, exactly one frame is counted refused */
static void test_overflow_drops_newest(void)
{
	struct awfxs_play_frame slot[CAPACITY], out[CAPACITY];
	struct awfxs_play_ring r;
	u32 n;

	play_ring_reset(&r, slot, CAPACITY);
	for (n = 0; n < CAPACITY + 1; n++)
		if (n == CAPACITY) {
			assert(play_ring_push(&r, &(struct awfxs_play_frame){ 0 }) == 0);
			assert(r.drops == 1);
		} else {
			struct awfxs_play_frame f = frame_of(n);

			assert(play_ring_push(&r, &f) == 1);
			assert(r.drops == 0);
		}
	assert(play_ring_pop(&r, out, CAPACITY) == CAPACITY);
	for (n = 0; n < CAPACITY; n++)
		assert_frame(&out[n], n);		/* drop-newest: frames 0..CAPACITY-1 remain in order */
	assert(play_ring_stored(&r) == 0);
	printf("play: overflow refuses the arriving frame and keeps what was queued continuous\n");
}

/* a ring kept nearly full while the bridge drains one frame per write: every frame in order across many wraparounds */
static void test_wraparound_integrity(void)
{
	struct awfxs_play_frame slot[CAPACITY], out[1];
	struct awfxs_play_ring r;
	u32 total = CAPACITY * 5 + 13, written = 0, taken = 0, n;

	play_ring_reset(&r, slot, CAPACITY);
	for (n = 0; n < CAPACITY - 8; n++) {
		struct awfxs_play_frame f = frame_of(n);

		assert(play_ring_push(&r, &f) == 1);
		written++;
	}
	while (written < total) {
		struct awfxs_play_frame f = frame_of(written);

		assert(play_ring_push(&r, &f) == 1);
		written++;
		assert(play_ring_pop(&r, out, 1) == 1);
		assert_frame(&out[0], taken++);
	}
	while (play_ring_pop(&r, out, 1) == 1)
		assert_frame(&out[0], taken++);
	assert(taken == total);
	assert(r.drops == 0);
	assert(play_ring_stored(&r) == 0);
	printf("play: %u frames survive a nearly full ring across wraparounds, all four samples intact\n", total);
}

/* reset zeroes everything and the ring is reusable */
static void test_reset(void)
{
	struct awfxs_play_frame slot[CAPACITY], out[1];
	struct awfxs_play_ring r;
	struct awfxs_play_frame f = frame_of(99);

	play_ring_reset(&r, slot, CAPACITY);
	assert(play_ring_push(&r, &f) == 1);
	assert(play_ring_stored(&r) == 1);
	play_ring_reset(&r, slot, CAPACITY);
	assert(r.head == 0 && r.tail == 0 && r.drops == 0);
	assert(play_ring_pop(&r, out, 1) == 0);
	printf("play: reset clears frames, indices and the refused count\n");
}

int main(void)
{
	test_fifo_order();
	test_overflow_drops_newest();
	test_wraparound_integrity();
	test_reset();
	printf("play: all tests passed\n");
	return 0;
}
