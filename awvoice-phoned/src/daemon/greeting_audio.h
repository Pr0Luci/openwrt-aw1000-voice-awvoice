/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_GREETING_AUDIO_H
#define AW_PHONED_GREETING_AUDIO_H

/*
 * The two things a fresh greeting recording still needs before it can answer a call: the silent lead and tail that a
 * hand-started recording always carries, and the tone that tells the caller the machine is listening. The machine
 * plays no tone of its own, so the tone lives in the greeting file, the way a classic telephone answering machine
 * puts it at the end of its outgoing message.
 *
 * Both halves are plain arithmetic on 16-bit samples at one of the PCM bus's rates, with no allocation: the caller
 * holds the audio. Silence is measured against the clip's own peak rather than a fixed level, because the microphone's
 * gain is the modem's setting and a greeting recorded quietly is still a greeting.
 *
 * The tone is 1 kHz - the frequency every caller already knows means "leave a message" - at about -14 dBFS, with a
 * short ramp at each end: a sine switched on at full amplitude is a click the caller hears as a pop. The 16-entry
 * table below is exact at both rates of the bus: a cycle is 8 samples at 8000 Hz and 16 at 16000, and 16 is a
 * multiple of both, so no interpolation is involved and the tone is one kilohertz rather than close to it.
 *
 * The tone waits behind GREETING_GAP_MS of silence rather than starting on the greeting's last spoken sample: a
 * person who recorded the greeting is still saying the last word when it ends, and a tone that arrives in the middle
 * of that is heard as a jolt instead of a hand-over.
 */
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#include <linux/errno.h>
#else
#include <errno.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
#endif

#define GREETING_TONE_HZ	1000
#define GREETING_TONE_MS	700	/* long enough to be heard as a tone, short enough not to waste the caller */
#define GREETING_GAP_MS		500	/* silence between the last word and the tone: a tone that starts on the speech's
					   own last sample is heard as a jolt, and the listener has no time to stop talking */
#define GREETING_RAMP_MS	10	/* the fade at each end of the tone */
#define GREETING_WINDOW_MS	8	/* silence is judged over a window, so one click is not the start of speech */
#define GREETING_FLOOR		256	/* about -40 dBFS: nothing under this is speech whatever the gain was set to */
#define GREETING_QUIET_DIVISOR	8	/* a window 18 dB under the clip's own peak is silence in this clip */
#define GREETING_TONE_LEVEL	6553	/* about -14 dBFS: heard clearly over the line without drowning the voice */

/* the whole clip's loudest sample's magnitude; 0 for an empty clip */
static inline u32 greeting_peak(const int16_t *s, u32 n)
{
	u32 k, peak = 0;

	for (k = 0; k < n; k++) {
		u32 v = s[k] < 0 ? (u32)-(s[k]) : (u32)s[k];

		if (v > peak)
			peak = v;
	}
	return peak;
}

/* the loudest sample's magnitude in the window [from, from + len) of `s`, clipped to the clip */
static inline u32 greeting_window_peak(const int16_t *s, u32 n, u32 from, u32 len)
{
	u32 k, stop = from + len > n ? n : from + len, peak = 0;

	for (k = from; k < stop; k++) {
		u32 v = s[k] < 0 ? (u32)-(s[k]) : (u32)s[k];

		if (v > peak)
			peak = v;
	}
	return peak;
}

/*
 * Where the speech in `s` begins and ends, as the frame range [start, end). A window is silence when its loudest
 * sample stays under the clip's own threshold: the peak divided by GREETING_QUIET_DIVISOR, never less than
 * GREETING_FLOOR. Windows are stepped over the clip, so the cost is one pass.
 *
 * A clip with no speech at all - every window silence - gives an empty range, and the caller refuses it as empty
 * rather than store a greeting of nothing.
 */
static inline void greeting_trim(const int16_t *s, u32 n, u32 rate, u32 *start, u32 *end)
{
	u32 win = rate * GREETING_WINDOW_MS / 1000;
	u32 peak = greeting_peak(s, n);
	u32 thresh = peak / GREETING_QUIET_DIVISOR;
	u32 from, first = 0, last = 0, found = 0;

	*start = *end = 0;
	if (!n || !rate)
		return;
	if (!win)
		win = 1;
	if (thresh < GREETING_FLOOR)
		thresh = GREETING_FLOOR;
	for (from = 0; from < n; from += win) {
		u32 stop = from + win > n ? n : from + win;

		if (greeting_window_peak(s, n, from, win) < thresh)
			continue;
		if (!found)
			first = from;
		last = stop;
		found = 1;
	}
	if (!found)
		return;
	*start = first;
	*end = last;
}

/* the tone's length in samples at `rate` */
static inline u32 greeting_tone_len(u32 rate)
{
	return rate * GREETING_TONE_MS / 1000;
}

/* the silence the tone waits behind */
static inline u32 greeting_gap_len(u32 rate)
{
	return rate * GREETING_GAP_MS / 1000;
}

/* everything the polish adds after the trimmed voice: a caller that cannot know the trim sizes its room by this */
static inline u32 greeting_polish_tail_len(u32 rate)
{
	return greeting_gap_len(rate) + greeting_tone_len(rate);
}

/* sin(2 pi k / 16) for k = 0..15, scaled by 1000; a cycle of 1 kHz is 8 samples at 8000 Hz and 16 at 16000 */
static const int greeting_sine16[16] = {
	0, 383, 707, 924, 1000, 924, 707, 383, 0, -383, -707, -924, -1000, -924, -707, -383
};

/*
 * The 1 kHz tone, ramped in and out over GREETING_RAMP_MS. `n` must be greeting_tone_len(rate); the phase advances by
 * exactly 1000/rate of a cycle a sample, which is a whole number of table steps at either rate.
 */
static inline int greeting_tone_write(int16_t *out, u32 n, u32 rate)
{
	unsigned long long step;		/* the phase's advance, 65536 a cycle */
	unsigned long long phase = 0;
	u32 ramp, k;

	if (!out || !n || !rate || rate < GREETING_TONE_HZ * 2)
		return -EINVAL;			/* a tone needs the rate to carry it: Nyquist, not a matter of taste */
	step = (unsigned long long)GREETING_TONE_HZ * 65536 / rate;	/* computed after the rate is known to be one */
	ramp = rate * GREETING_RAMP_MS / 1000;
	if (ramp < 1)
		ramp = 1;
	if (ramp > n / 2)
		ramp = n / 2;
	for (k = 0; k < n; k++) {
		long amp = GREETING_TONE_LEVEL;
		unsigned int index;

		if (k < ramp)
			amp = amp * (long)k / (long)ramp;
		else if (k + ramp > n)
			amp = amp * (long)(n - k) / (long)ramp;
		index = (unsigned int)((phase >> 12) & 15);
		out[k] = (int16_t)(greeting_sine16[index] * amp / 1000);
		phase += step;
	}
	return 0;
}

/*
 * Trim the silence off `in` and put the result in `out`: the voice, a short silence, then the tone. `out_cap` needs the
 * trimmed length plus greeting_polish_tail_len(rate); a caller that cannot know the trim passes the worst case
 * (n + greeting_polish_tail_len(rate)). Returns the samples written, -ENODATA when the clip holds no speech, or
 * -EINVAL when `out_cap` cannot hold it.
 */
static inline long greeting_audio_polish(const int16_t *in, u32 n, u32 rate, int16_t *out, u32 out_cap)
{
	u32 start = 0, end = 0, kept, gap = greeting_gap_len(rate), tail = greeting_polish_tail_len(rate);

	if (!in || !out || !rate || !n)
		return -ENODATA;
	greeting_trim(in, n, rate, &start, &end);
	if (end <= start)
		return -ENODATA;
	kept = end - start;
	if (kept > out_cap || tail > out_cap - kept)
		return -EINVAL;
	memcpy(out, in + start, kept * sizeof(*out));
	memset(out + kept, 0, gap * sizeof(*out));
	greeting_tone_write(out + kept + gap, greeting_tone_len(rate), rate);
	return (long)(kept + tail);
}

#endif /* AW_PHONED_GREETING_AUDIO_H */
