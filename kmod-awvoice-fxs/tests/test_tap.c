// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * Host tests of the capture tap's ring: the same tap_ring.h the bridge thread writes and the ioctl reader drains.
 * Run: make -f Makefile.host test
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/tap_ring.h"

#define CAPACITY 64

static struct awfxs_tap_frame frame_of(u32 n)
{
	struct awfxs_tap_frame f = { .modem = (u16)(n * 3 + 1), .line = (u16)(n * 5 + 2),
		.modem2 = (u16)(n * 7 + 3), .line2 = (u16)(n * 11 + 4) };

	return f;
}

static void assert_frame(const struct awfxs_tap_frame *f, u32 n)
{
	const struct awfxs_tap_frame want = frame_of(n);

	assert(!memcmp(f, &want, sizeof(*f)));
}

/* push one more than the capacity: the oldest survives, exactly one frame is counted dropped, order intact */
static void test_overflow_drops_newest(void)
{
	struct awfxs_tap_frame slot[CAPACITY], out[CAPACITY];
	struct awfxs_tap_ring r;
	u32 n;

	tap_ring_reset(&r, slot, CAPACITY);
	for (n = 0; n < CAPACITY + 1; n++)
		if (n == CAPACITY) {
			assert(tap_ring_push(&r, &(struct awfxs_tap_frame){ 0 }) == 0);
			assert(r.drops == 1);
		} else {
			struct awfxs_tap_frame f = frame_of(n);

			assert(tap_ring_push(&r, &f) == 1);
			assert(r.drops == 0);
		}
	assert(tap_ring_pop(&r, out, CAPACITY) == CAPACITY);
	for (n = 0; n < CAPACITY; n++)
		assert_frame(&out[n], n);		/* drop-newest: frames 0..CAPACITY-1 remain in order */
	assert(tap_ring_stored(&r) == 0);
	printf("tap: overflow drops the arriving frame and keeps the ring continuous\n");
}

/* a slow reader that always drains keeps every frame across many wraparounds, wideband stride included */
static void test_wraparound_integrity(void)
{
	struct awfxs_tap_frame slot[CAPACITY], out[8];
	struct awfxs_tap_ring r;
	u32 total = CAPACITY * 5 + 13, n, got;

	tap_ring_reset(&r, slot, CAPACITY);
	for (n = 0; n < total; n++) {
		struct awfxs_tap_frame f = frame_of(n);

		assert(tap_ring_push(&r, &f) == 1);
		while ((got = tap_ring_pop(&r, out, 8)) > 0)
			for (u32 i = 0; i < got; i++)
				assert_frame(&out[i], n + 1 - got + i);
	}
	assert(r.drops == 0);
	assert(tap_ring_stored(&r) == 0);
	(void)out;
	printf("tap: %u frames survive a slow-reader drain across wraparounds, all four samples intact\n", total);
}

/* drops do not desynchronise the ring: after a full drain the next push reads back whole */
static void test_recovery_after_drops(void)
{
	struct awfxs_tap_frame slot[CAPACITY], out[CAPACITY];
	struct awfxs_tap_ring r;
	u32 n;

	tap_ring_reset(&r, slot, CAPACITY);
	for (n = 0; n < CAPACITY + 7; n++) {
		struct awfxs_tap_frame f = frame_of(n);

		(void)tap_ring_push(&r, &f);
	}
	assert(r.drops == 7);
	assert(tap_ring_pop(&r, out, CAPACITY) == CAPACITY);
	for (n = 0; n < CAPACITY; n++)
		assert_frame(&out[n], n);		/* the dropped seven were the newest; 0..CAPACITY-1 remain whole */
	assert(tap_ring_pop(&r, out, CAPACITY) == 0);
	printf("tap: after drops the ring resumes whole frames from the first survivor\n");
}

/* reset zeroes everything and the ring is reusable */
static void test_reset(void)
{
	struct awfxs_tap_frame slot[CAPACITY], out[4];
	struct awfxs_tap_ring r;
	struct awfxs_tap_frame f = frame_of(99);

	tap_ring_reset(&r, slot, CAPACITY);
	assert(tap_ring_push(&r, &f) == 1);
	tap_ring_reset(&r, slot, CAPACITY);
	assert(r.head == 0 && r.tail == 0 && r.drops == 0);
	assert(tap_ring_pop(&r, out, 4) == 0);
	printf("tap: reset clears frames, indices and the drop counter\n");
}

int main(void)
{
	test_overflow_drops_newest();
	test_wraparound_integrity();
	test_recovery_after_drops();
	test_reset();
	printf("tap: all tests passed\n");
	return 0;
}
