// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The 16000/8000 pair the media socket needs: the average of a pair, the hold of one sample for two, the extremes. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "media_rate.h"

static void test_down(void)
{
	int16_t in[FE_SAMPLES_16K];
	int16_t out[FE_SAMPLES_8K];
	u32 k;

	for (k = 0; k < FE_SAMPLES_16K; k++)
		in[k] = (int16_t)(k * 37);
	media_rate_down16(in, out);
	for (k = 0; k < FE_SAMPLES_8K; k++)
		assert(out[k] == (int16_t)((in[2 * k] + in[2 * k + 1]) / 2));
	/* a pair around zero keeps its sign, and the ends stay at the ends */
	in[0] = 32767;
	in[1] = 32767;
	in[2] = -32768;
	in[3] = -32768;
	in[4] = 1;
	in[5] = 0;
	media_rate_down16(in, out);
	assert(out[0] == 32767);
	assert(out[1] == -32768);
	assert(out[2] == 0);		/* one half is cut down, as integer division does */
}

static void test_up(void)
{
	int16_t in[FE_SAMPLES_8K];
	int16_t out[FE_SAMPLES_16K];
	u32 k;

	for (k = 0; k < FE_SAMPLES_8K; k++)
		in[k] = (int16_t)(k - 80);
	media_rate_up16(in, out);
	for (k = 0; k < FE_SAMPLES_8K; k++) {
		assert(out[2 * k] == in[k]);
		assert(out[2 * k + 1] == in[k]);
	}

	/* down and up in a row keeps the coarse shape of a slow signal */
	memset(in, 0, sizeof(in));
	for (k = 0; k < FE_SAMPLES_8K; k++)
		in[k] = (int16_t)((k % 8) < 4 ? 1200 : -1200);
	media_rate_up16(in, out);
	media_rate_down16(out, in);
	for (k = 0; k < FE_SAMPLES_8K; k++)
		assert(in[k] == (int16_t)((k % 8) < 4 ? 1200 : -1200));
}

int main(void)
{
	test_down();
	test_up();
	printf("media_rate: all tests passed\n");
	return 0;
}
