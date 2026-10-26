/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AWFXS_WINDOW_H
#define AWFXS_WINDOW_H

/*
 * The peak of the modem's samples over a fixed window of frames. One sample is fed per frame; when the window is full
 * its peak is handed out and the next window starts empty. Plain arithmetic on purpose: it is tested on the host
 * (tests/test_window.c) and runs inside the bridge thread, so it takes no lock and does not allocate.
 */
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t u32;
#endif

struct level_window {
	u32 peak;	/* the largest magnitude fed in the window that is running */
	u32 frames;	/* frames fed in it */
};

/* feed one frame's magnitude; true when this frame completed a window, *published is then that window's peak */
static inline int level_window_feed(struct level_window *w, u32 magnitude, u32 length, u32 *published)
{
	if (magnitude > w->peak)
		w->peak = magnitude;
	if (++w->frames < length)
		return 0;
	*published = w->peak;
	w->peak = 0;
	w->frames = 0;
	return 1;
}

#endif /* AWFXS_WINDOW_H */
