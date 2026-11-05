// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* tests/test_play_fill.c - the playback ring's frame shape: host-only, plain asserts. */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#include "play_fill.h"

/* 1. narrowband: one sample per frame, the second slot of the pair unused */
static void test_narrowband_one_sample_per_frame(void)
{
	int16_t in[4] = { 100, -200, 300, 4 };
	uint16_t out[4 * 4];
	unsigned int f;

	f = play_fill_frames(out, 4, in, 4, 0);
	assert(f == 4);
	assert(out[0] == 100 && out[1] == 0 && out[2] == 0 && out[3] == 0);
	assert(out[4] == (uint16_t)(-200) && out[6] == 0);	/* the two's-complement pattern the kernel pastes */
	assert(out[8] == 300 && out[12] == 4);
	assert(play_frames_of(4, 0) == 4);
}

/* 2. wideband: two samples per frame, in the order the kernel pastes them (modem at the slot, modem2 half a frame on) */
static void test_wideband_two_samples_per_frame(void)
{
	int16_t in[6] = { 1, 2, 3, 4, 5, 6 };
	uint16_t out[6 * 4];
	unsigned int f;

	f = play_fill_frames(out, 6, in, 6, 1);
	assert(f == 3);						/* six samples are three frames, not six */
	assert(out[0] == 1 && out[2] == 2);
	assert(out[4] == 3 && out[6] == 4);
	assert(out[8] == 5 && out[10] == 6);
	assert(out[1] == 0 && out[3] == 0 && out[5] == 0);		/* the handset's slots: a bridged call does not feed it */
}

/* 3. the 20 ms contract: a wire frame is 160 samples at 8 kHz, and the same audio at 16 kHz is 320 samples in 160
 *    frames - 20 ms of ring time. Offering 320 frames here is what stretched the page's voice across 40 ms. */
static void test_wire_frame_covers_its_own_20_ms(void)
{
	int16_t mono[160], wide[320];
	uint16_t out[320 * 4];
	unsigned int k, f;

	for (k = 0; k < 160; k++) {
		mono[k] = (int16_t)(k * 100 - 8000);
		wide[2 * k] = wide[2 * k + 1] = mono[k];	/* what media_rate_up16 hands over */
	}
	f = play_fill_frames(out, 320, wide, 320, 1);
	assert(f == 160);
	assert(f / PLAY_FRAMES_PER_MS == 20);				/* 20 ms of line time, as the frame was */
	for (k = 0; k < 160; k++) {
		assert(out[k * 4] == (uint16_t)mono[k]);
		assert(out[k * 4 + 2] == (uint16_t)mono[k]);		/* the held sample, not silence */
	}
	f = play_fill_frames(out, 320, mono, 160, 0);
	assert(f == 160);
	assert(f / PLAY_FRAMES_PER_MS == 20);				/* the same 20 ms narrowband */
}

/* 4. an odd sample waits: half a frame is never sent */
static void test_odd_sample_waits(void)
{
	int16_t in[5] = { 7, 8, 9, 10, 11 };
	uint16_t out[5 * 4];
	unsigned int k;

	for (k = 0; k < 5 * 4; k++)
		out[k] = 0x5a5a;					/* a pattern nothing here writes: the untouched slots keep it */
	assert(play_fill_frames(out, 5, in, 5, 1) == 2);		/* four samples placed, the fifth waits */
	assert(out[0] == 7 && out[2] == 8);
	assert(out[4] == 9 && out[6] == 10);
	assert(out[8] == 0x5a5a && out[10] == 0x5a5a);			/* the third frame was not half-filled */
	assert(out[1] == 0 && out[3] == 0 && out[5] == 0 && out[7] == 0);
}

/* 5. the writer's cap is respected, and the lead is counted in frames whatever the band */
static void test_caps_and_lead(void)
{
	int16_t in[100];
	uint16_t out[100 * 4];
	unsigned int k;

	for (k = 0; k < 100; k++)
		in[k] = (int16_t)k;
	assert(play_fill_frames(out, 10, in, 100, 0) == 10);
	assert(play_fill_frames(out, 10, in, 100, 1) == 10);		/* 20 samples of the 100 */
	assert(play_lead_frames(120) == 960);				/* the same 120 ms at 8 kHz and at 16 kHz */
	assert(play_frames_of(320, 1) == 160 && play_frames_of(160, 0) == 160);
}

int main(void)
{
	test_narrowband_one_sample_per_frame();
	test_wideband_two_samples_per_frame();
	test_wire_frame_covers_its_own_20_ms();
	test_odd_sample_waits();
	test_caps_and_lead();
	printf("play_fill: all tests passed\n");
	return 0;
}
