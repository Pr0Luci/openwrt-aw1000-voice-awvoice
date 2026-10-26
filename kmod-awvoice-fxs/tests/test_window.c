// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The bridge's windowed level, on the host: make -f Makefile.host test */
#include <assert.h>
#include <stdio.h>

#include "../src/window.h"

int main(void)
{
	struct level_window w = { 0, 0 };
	u32 got = 123;
	u32 k, n;

	/* a window is complete exactly at its last frame, with the peak of that window only */
	for (k = 0; k < 799; k++)
		assert(!level_window_feed(&w, k % 7 == 0 ? 50 : 3, 800, &got));
	assert(got == 123);
	assert(level_window_feed(&w, 9, 800, &got) && got == 50);

	/* the next window starts empty: silence after loud audio reads as silence, not as the old peak */
	for (k = 0; k < 799; k++)
		assert(!level_window_feed(&w, 0, 800, &got));
	assert(level_window_feed(&w, 0, 800, &got) && got == 0);

	/* one loud frame anywhere in the window is seen, at the first and at the last position */
	assert(!level_window_feed(&w, 32768, 800, &got));
	for (k = 1; k < 799; k++)
		(void)level_window_feed(&w, 1, 800, &got);
	assert(level_window_feed(&w, 1, 800, &got) && got == 32768);
	for (k = 0; k < 799; k++)
		(void)level_window_feed(&w, 1, 800, &got);
	assert(level_window_feed(&w, 20000, 800, &got) && got == 20000);

	/* windows come at the length asked for, and only then; ten seconds of frames are exactly 100 windows */
	for (n = 0, k = 0; k < 80000; k++)
		n += level_window_feed(&w, 7, 800, &got);
	assert(n == 100 && got == 7);

	/* a window of one frame hands out every sample */
	for (k = 0; k < 5; k++)
		assert(level_window_feed(&w, k, 1, &got) && got == k);

	puts("window: all tests passed");
	return 0;
}
