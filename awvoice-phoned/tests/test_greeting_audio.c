// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The greeting recording's two fixes - the silence off both ends and the tone at the end: make -f Makefile.host test */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "greeting_audio.h"

static int16_t in[8000], out[16000];

static void fill_silence(u32 n)
{
	u32 k;

	for (k = 0; k < n; k++)
		in[k] = 0;
}

static void fill_speech(u32 from, u32 to, int level)
{
	u32 k;

	for (k = from; k < to; k++)
		in[k] = (int16_t)((k & 1) ? -level : level);
}

static void test_peak(void)
{
	assert(greeting_peak(in, 0) == 0);
	in[0] = 100;
	in[1] = -300;
	in[2] = 200;
	assert(greeting_peak(in, 3) == 300);
	in[1] = -32768;			/* the one magnitude that is not a positive sample: it must not wrap */
	assert(greeting_peak(in, 3) == 32768);
	in[1] = 0;
	assert(greeting_window_peak(in, 3, 0, 64) == 200);	/* a window longer than the clip reads to its end */
	in[0] = 0; in[2] = 0;
	assert(greeting_peak(in, 3) == 0);
}

static void test_trim(void)
{
	u32 start = 99, end = 99;

	/* a clip of nothing but silence has no speech to keep: an empty range, which the caller refuses as empty */
	fill_silence(4000);
	in[200] = 40;				/* under the floor: a breath of noise is not a voice */
	greeting_trim(in, 4000, 8000, &start, &end);
	assert(start == 0 && end == 0);

	/* the silent lead and tail go, the voice stays where it was */
	fill_silence(4000);
	fill_speech(800, 2400, 8000);
	greeting_trim(in, 4000, 8000, &start, &end);
	assert(start == 768 && end == 2432);	/* the nearest window edge outside the speech, 8 ms at either rate */
	assert(!memcmp(in + start, in + 768, 10));	/* nothing was rewritten: the range is a range into the caller's clip */

	/* speech that fills the clip keeps all of it */
	fill_speech(0, 4000, 6000);
	greeting_trim(in, 4000, 8000, &start, &end);
	assert(start == 0 && end == 4000);

	/* the threshold is the clip's own peak: a quiet recording is still a recording */
	fill_silence(4000);
	fill_speech(1000, 2000, 900);		/* peak 900, so the threshold is the 256 floor, not a ninth of it */
	greeting_trim(in, 4000, 8000, &start, &end);
	assert(start == 960 && end == 2048);
	/* and a loud clip judges its own tail's noise as silence: 18 dB under its peak */
	fill_silence(4000);
	fill_speech(100, 600, 30000);
	for (u32 k = 2000; k < 2100; k++)
		in[k] = 2000;			/* a hum well under the peak: not speech in this clip */
	greeting_trim(in, 4000, 8000, &start, &end);
	assert(end < 2000);

	/* one click is a window of speech, and the range is that window: a click is not a greeting but it is not silence */
	fill_silence(4000);
	in[2000] = 20000;
	greeting_trim(in, 4000, 8000, &start, &end);
	assert(start == 1984 && end == 2048);

	/* a rate of none or a clip of none is the empty range, not a division by zero */
	greeting_trim(in, 0, 8000, &start, &end);
	assert(start == 0 && end == 0);
	fill_speech(0, 100, 8000);
	greeting_trim(in, 100, 0, &start, &end);
	assert(start == 0 && end == 0);
}

static void test_tone(void)
{
	u32 k;

	assert(greeting_tone_len(8000) == 5600);	/* 700 ms at either rate of the bus */
	assert(greeting_tone_len(16000) == 11200);
	assert(greeting_gap_len(8000) == 4000);		/* the breath before it: 500 ms */
	assert(greeting_gap_len(16000) == 8000);
	assert(greeting_polish_tail_len(8000) == 9600);	/* what the caller has to leave room for, gap and tone */
	assert(greeting_polish_tail_len(16000) == 19200);
	assert(greeting_tone_write(out, 0, 8000) == -EINVAL);
	assert(greeting_tone_write(NULL, 100, 8000) == -EINVAL);
	assert(greeting_tone_write(out, 100, 0) == -EINVAL);
	assert(greeting_tone_write(out, 100, 8000) == 0);

	/* 1 kHz exactly: the waveform repeats at its period once the ramp is over */
	assert(greeting_tone_write(out, greeting_tone_len(8000), 8000) == 0);
	for (k = 100; k + 8 < greeting_tone_len(8000) - 100; k++)
		assert(out[k] == out[k + 8]);
	assert(out[0] == 0);						/* ramped in: the tone does not start as a click */
	{
		int mid = 0, in8 = 0, out8 = 0;

		for (k = 0; k < 5600; k++) {
			int v = out[k] < 0 ? -out[k] : out[k];

			if (k > 200 && k < 5300 && v > mid)
				mid = v;
			if (k < 8 && v > in8)
				in8 = v;
			if (k >= 5600 - 8 && v > out8)
				out8 = v;
		}
		assert(mid == GREETING_TONE_LEVEL);			/* the level it climbs to is the level the line carries */
		assert(in8 < GREETING_TONE_LEVEL / 8);		/* the first eight samples are the bottom of the fade in */
		assert(out8 < GREETING_TONE_LEVEL / 8);		/* and the last eight the bottom of the fade out: no click */
	}
	assert(out[1] > 0 && out[2] > out[1]);			/* the ramp climbs */
	for (k = 0; k < greeting_tone_len(8000); k++)
		assert(out[k] <= GREETING_TONE_LEVEL && out[k] >= -GREETING_TONE_LEVEL);
	{
		int peak = 0;

		for (k = 0; k < greeting_tone_len(8000); k++)
			if (out[k] > peak)
				peak = out[k];
		assert(peak == GREETING_TONE_LEVEL);		/* the level a caller hears as a tone, not a shout */
	}

	/* the wideband greeting gets the same tone, twice the samples of it */
	assert(greeting_tone_write(out, greeting_tone_len(16000), 16000) == 0);
	for (k = 200; k + 16 < greeting_tone_len(16000) - 200; k++)
		assert(out[k] == out[k + 16]);
	/* a rate that cannot carry a kilohertz is refused, not aliased into something else */
	assert(greeting_tone_write(out, 100, 1500) == -EINVAL);
}

static void test_polish(void)
{
	long got;
	u32 k, start = 0, end = 0, kept;

	assert(greeting_audio_polish(NULL, 100, 8000, out, 1000) == -ENODATA);
	assert(greeting_audio_polish(in, 100, 8000, NULL, 1000) == -ENODATA);
	assert(greeting_audio_polish(in, 0, 8000, out, 1000) == -ENODATA);
	assert(greeting_audio_polish(in, 100, 0, out, 1000) == -ENODATA);

	/* nothing but silence: there is no greeting in it */
	fill_silence(3000);
	assert(greeting_audio_polish(in, 3000, 8000, out, 16000) == -ENODATA);

	/* the voice with its silence cut off, a breath of quiet, and the tone behind it */
	fill_silence(8000);
	fill_speech(800, 2400, 8000);
	greeting_trim(in, 4000, 8000, &start, &end);
	kept = end - start;
	got = greeting_audio_polish(in, 4000, 8000, out, kept + greeting_polish_tail_len(8000));
	assert(got == (long)(kept + greeting_polish_tail_len(8000)));
	assert(!memcmp(out, in + start, kept * sizeof(*out)));		/* the voice, sample for sample */
	for (k = kept; k < kept + greeting_gap_len(8000); k++)
		assert(out[k] == 0);						/* the tone does not start on the word's own last sample */
	{
		int16_t tone[8192];

		greeting_tone_write(tone, greeting_tone_len(8000), 8000);
		assert(!memcmp(out + kept + greeting_gap_len(8000), tone, greeting_tone_len(8000) * sizeof(*tone)));
	}

	/* a room that cannot hold the gap and the tone is refused: no half file is ever handed to the caller */
	assert(greeting_audio_polish(in, 4000, 8000, out, kept) == -EINVAL);
	assert(greeting_audio_polish(in, 4000, 8000, out, kept + greeting_polish_tail_len(8000) - 1) == -EINVAL);
	/* the worst-case room the caller can pass without knowing the trim */
	got = greeting_audio_polish(in, 4000, 8000, out, 4000 + greeting_polish_tail_len(8000));
	assert(got == (long)(kept + greeting_polish_tail_len(8000)));
}

int main(void)
{
	test_peak();
	test_trim();
	test_tone();
	test_polish();
	printf("greeting audio: all tests passed\n");
	return 0;
}
