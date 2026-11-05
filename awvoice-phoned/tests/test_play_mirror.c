// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The written audio waiting for its slot in the recording: order, the wrap, a short pull, an overflow, the reset. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "play_mirror.h"

#define FRAME_SAMPLES	160u	/* one 20 ms frame at 8000 Hz: what the play feed writes at a time */

static void test_order(void)
{
	struct play_mirror m;
	u16 in[FRAME_SAMPLES], out[FRAME_SAMPLES];
	u32 k;

	memset(&m, 0, sizeof(m));
	for (k = 0; k < FRAME_SAMPLES; k++)
		in[k] = (u16)(k + 1);
	play_mirror_push(&m, in, FRAME_SAMPLES);
	assert(m.used == FRAME_SAMPLES);
	assert(m.drops == 0);

	/* the tap drains half a frame at a time and the samples come out as they went in */
	assert(play_mirror_pull(&m, out, FRAME_SAMPLES / 2) == FRAME_SAMPLES / 2);
	for (k = 0; k < FRAME_SAMPLES / 2; k++)
		assert(out[k] == (u16)(k + 1));
	assert(play_mirror_pull(&m, out, FRAME_SAMPLES / 2) == FRAME_SAMPLES / 2);
	for (k = 0; k < FRAME_SAMPLES / 2; k++)
		assert(out[k] == (u16)(FRAME_SAMPLES / 2 + k + 1));
	assert(m.used == 0);

	/* an empty mirror hands out nothing at all */
	assert(play_mirror_pull(&m, out, 4) == 0);
}

static void test_wrap(void)
{
	struct play_mirror m;
	u16 in[FRAME_SAMPLES], out[FRAME_SAMPLES];
	unsigned int k;

	memset(&m, 0, sizeof(m));
	/* nine seconds of frames, each drained whole: the indices cross the end of the array again and again */
	for (k = 0; k < 450; k++) {
		unsigned int j;

		for (j = 0; j < FRAME_SAMPLES; j++)
			in[j] = (u16)(k * FRAME_SAMPLES + j);
		play_mirror_push(&m, in, FRAME_SAMPLES);
		assert(play_mirror_pull(&m, out, FRAME_SAMPLES) == FRAME_SAMPLES);
		for (j = 0; j < FRAME_SAMPLES; j++)
			assert(out[j] == (u16)(k * FRAME_SAMPLES + j));	/* the count wraps as a u16 the same way on both sides */
	}
	assert(m.used == 0 && m.drops == 0);
}

static void test_short_pull(void)
{
	struct play_mirror m;
	u16 in[8], out[4];
	u32 k;

	memset(&m, 0, sizeof(m));
	for (k = 0; k < 8; k++)
		in[k] = (u16)(100 + k);
	play_mirror_push(&m, in, 6);
	assert(play_mirror_pull(&m, out, 4) == 4);
	for (k = 0; k < 4; k++)
		assert(out[k] == (u16)(100 + k));
	out[2] = 7002;
	out[3] = 7003;	/* the tap's own slot stands in until the mirror fills up */
	/* only two are left: the entries the mirror could not fill keep the values they came with */
	assert(play_mirror_pull(&m, out, 4) == 2);
	assert(out[0] == 104 && out[1] == 105);
	assert(out[2] == 7002 && out[3] == 7003);
	assert(m.used == 0);
}

static void test_overflow(void)
{
	struct play_mirror m;
	u16 in[FRAME_SAMPLES], out[2];
	unsigned int pushed = 0;

	memset(&m, 0, sizeof(m));
	/* a drain that stopped: the mirror fills, and then the oldest samples go while the newest stay */
	while (pushed < PLAY_MIRROR_SAMPLES + FRAME_SAMPLES) {
		unsigned int k;

		for (k = 0; k < FRAME_SAMPLES; k++)
			in[k] = (u16)(pushed + k + 1);
		play_mirror_push(&m, in, FRAME_SAMPLES);
		pushed += FRAME_SAMPLES;
	}
	assert(m.used == PLAY_MIRROR_SAMPLES);
	assert(m.drops == pushed - PLAY_MIRROR_SAMPLES);
	assert(play_mirror_pull(&m, out, 2) == 2);
	assert(out[0] == (u16)(pushed - PLAY_MIRROR_SAMPLES + 1));
	assert(out[1] == (u16)(pushed - PLAY_MIRROR_SAMPLES + 2));

	/* more in one go than the mirror holds: its newest part is what stays */
	{
		u16 big[PLAY_MIRROR_SAMPLES + 16];
		unsigned int k;

		play_mirror_reset(&m);
		for (k = 0; k < sizeof(big) / sizeof(big[0]); k++)
			big[k] = (u16)k;
		play_mirror_push(&m, big, sizeof(big) / sizeof(big[0]));
		assert(m.used == PLAY_MIRROR_SAMPLES);
		assert(play_mirror_pull(&m, out, 2) == 2);
		assert(out[0] == (sizeof(big) / sizeof(big[0])) - PLAY_MIRROR_SAMPLES);
		assert(out[1] == (sizeof(big) / sizeof(big[0])) - PLAY_MIRROR_SAMPLES + 1);
	}
}

static void test_reset(void)
{
	struct play_mirror m;
	u16 in[4] = { 11, 12, 13, 14 }, out[4] = { 91, 92, 93, 94 };
	u32 k;

	memset(&m, 0, sizeof(m));
	play_mirror_push(&m, in, 4);
	play_mirror_reset(&m);
	assert(m.used == 0);
	assert(play_mirror_pull(&m, out, 4) == 0);
	for (k = 0; k < 4; k++)
		assert(out[k] == (u16)(91 + k));	/* a pull of an empty mirror writes no sample at all */
	/* the drops are the daemon's count over its life: a new call does not hide an old overflow */
	m.drops = 3;
	play_mirror_reset(&m);
	assert(m.drops == 3);
}

int main(void)
{
	test_order();
	test_wrap();
	test_short_pull();
	test_overflow();
	test_reset();
	printf("play_mirror: all tests passed\n");
	return 0;
}
