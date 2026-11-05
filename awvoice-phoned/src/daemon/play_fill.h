// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * play_fill.h - what one frame of the bridge's playback ring holds. Header-only so the rule is testable without the
 * hardware (tests/test_play_fill.c).
 *
 * The kernel answers the ring once per PCM frame - 125 microseconds, 8000 times a second - whichever band the line was
 * brought up at, and a frame carries the direction's samples for that slice of time: one sample at 8 kHz, two at
 * 16 kHz, the second sitting half a frame (128 bits) after the first. So the ring's own unit is the frame, 8 of them
 * per millisecond, and 4096 of them is 512 ms narrowband or 256 ms wideband (kmod awfxs.h says the same).
 *
 * Getting this wrong is not silent: one 8 kHz frame of 160 samples offered as 320 frames with the second sample of each
 * pair left at zero plays the same audio across 40 ms of line time with silence between every sample - the far end hears
 * a stretched, hollow sound instead of the voice.
 *
 * The frame is written as four uint16_t in the order modem, line, modem2, line2, which is struct awfxs_play_frame's
 * field order; main.c proves that order against the kernel's header where the struct is visible.
 */

#ifndef AWVOICE_PLAY_FILL_H
#define AWVOICE_PLAY_FILL_H

#include <stdint.h>

#define PLAY_FRAMES_PER_MS	8u	/* the ring is answered 8000 times a second at either band */
#define PLAY_FIELD_STRIDE	4u	/* modem, line, modem2, line2 */

/* the lead the ring should keep, in frames: ms of audio is ms * 8 frames, whatever the band */
static inline unsigned int play_lead_frames(unsigned int ms)
{
	return ms * PLAY_FRAMES_PER_MS;
}

/* how many ring frames `samples` of the wire's 8 kHz stream fill: two samples per frame when the line is wideband */
static inline unsigned int play_frames_of(unsigned int samples, int wideband)
{
	return wideband ? (samples + 1u) / 2u : samples;
}

/*
 * Place `n` samples of the front end's audio into `out`, at most `max_frames` frames, and return the frames written.
 * `wideband` says the layout the line came up at: the samples are then already the 16 kHz pair stream (the caller's
 * media_rate_up16 of an 8 kHz frame), and each frame takes two of them. An odd last sample waits for the next round
 * rather than being sent as half a frame. The handset's two slots are silence: a bridged call does not go to the
 * handset as well.
 */
static inline unsigned int play_fill_frames(uint16_t *out, unsigned int max_frames, const int16_t *samples,
					    unsigned int n, int wideband)
{
	unsigned int per = wideband ? 2u : 1u, f;

	for (f = 0; f < max_frames && (f + 1u) * per <= n; f++) {
		uint16_t *q = out + f * PLAY_FIELD_STRIDE;

		q[0] = (uint16_t)samples[f * per];	/* modem: this frame's first sample toward the caller */
		q[1] = 0;
		q[2] = wideband ? (uint16_t)samples[f * per + 1u] : 0;	/* modem2: its second sample */
		q[3] = 0;
	}
	return f;
}

#endif /* AWVOICE_PLAY_FILL_H */
