// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * audio-level.js - the page's own send level. Plain arithmetic, no browser API: the same file is loaded as an
 * AudioWorklet module (before audio-worklet.js, which uses it) and run by the page's test suite.
 *
 * The browser's gain control is switched off for this stream (audio.js asks for autoGainControl false), because on a
 * live call it pushed a minus 20 dBFS test tone to minus 0.2 dBFS - twenty decibels of boost, straight into the hard
 * clamp at the end of the chain. A telephone path needs a level the sender owns instead: one slow loop that keeps a
 * talker's peaks near a nominal level, and one fast loop that never lets the signal reach full scale.
 *
 * Both loops work on the band-limited signal at the audio engine's own rate, before the 8 kHz decimation, so a peak
 * between two output samples is still caught. The fast loop looks ahead by LEVEL_LOOKAHEAD: the detector sees the
 * sample five milliseconds before the wire does, which is what keeps a plosive's first swing under the ceiling
 * instead of letting it through and clipping it. The bounds are the point: the loudness loop cannot turn up by more
 * than LEVEL_GAIN_MAX nor down by more than LEVEL_GAIN_MIN, and it stops pushing entirely below LEVEL_FLOOR so an
 * empty room is not lifted into hiss. The loudness loop works on the mean magnitude, so a steady tone's peak settles
 * a few decibels above LEVEL_TARGET and a voice's syllables more; the ceiling bounds the top either way. The nominal level
 * and the ceiling follow the usual telephone planning numbers (3GPP TS 26.131 on send sensitivity, ITU-T G.112 on the
 * clipping a send path may introduce).
 */

var LEVEL_TARGET = 0.1;		/* a talker's peak settles near minus 20 dBFS */
var LEVEL_CEILING = 0.707;	/* minus 3 dBFS: the limiter's knee, and what the wire must never pass */
var LEVEL_GAIN_MIN = 0.25;	/* minus 12 dB: the most this path turns down */
var LEVEL_GAIN_MAX = 8;		/* plus 18 dB: with the browser's gain control off, a laptop microphone can arrive this
						   far below the nominal level and still be a perfectly normal talker */
var LEVEL_FLOOR = 0.002;		/* minus 54 dBFS: below this the loudness loop stops pushing - the hiss of an empty
						   room is not lifted, and nothing is invented out of silence */
var LEVEL_ATTACK = 0.003;	/* the fast envelope: 3 ms, so a transient is caught while it is still ahead */
var LEVEL_RELEASE = 0.06;	/* 60 ms back up: the voice's own syllables are not chopped */
var LEVEL_LOOKAHEAD = 0.005;	/* 5 ms: the detector leads the wire */
var LEVEL_SLOW = 3;		/* the talker's loudness is re-estimated over about 3 s, not syllable by syllable */

function AwvoiceLevel(sampleRate) {
	this.kFast = Math.exp(-1 / (sampleRate * LEVEL_ATTACK));
	this.kUp = Math.exp(-1 / (sampleRate * LEVEL_RELEASE));
	this.kSlew = 1 - Math.exp(-1 / (sampleRate * LEVEL_SLOW));
	this.delay = new Float32Array(Math.ceil(sampleRate * LEVEL_LOOKAHEAD) || 1);
	this.at = 0;
	this.env = 0;
	this.slow = 0;
	this.gain = 1;
}

/* one band-limited sample in; the sample that went in LEVEL_LOOKAHEAD ago, levelled, comes out */
AwvoiceLevel.prototype.step = function (x) {
	var a = x < 0 ? -x : x, want, limit, gain, out;

	/* the fast envelope: up in 3 ms, back down over 60 ms */
	this.env = a > this.env ? this.kFast * this.env + (1 - this.kFast) * a
		: this.kUp * this.env + (1 - this.kUp) * a;

	/* the slow envelope: it runs up quickly to follow a talker leaning in, and relaxes four times slower */
	this.slow += a > this.slow ? (a - this.slow) * this.kSlew : (a - this.slow) * this.kSlew * 0.25;

	want = this.slow > LEVEL_FLOOR ? LEVEL_TARGET / this.slow : 1;
	if (want > LEVEL_GAIN_MAX)
		want = LEVEL_GAIN_MAX;
	else if (want < LEVEL_GAIN_MIN)
		want = LEVEL_GAIN_MIN;

	/* the ceiling outranks the loudness loop: the wire never passes it, however the normaliser feels about it */
	limit = LEVEL_CEILING / (this.env > 0.0001 ? this.env : 0.0001);
	gain = want < limit ? want : limit;

	/* onto the new gain at once (the attack is already in the envelope), off it slowly (no pumping) */
	this.gain = gain < this.gain ? gain : this.kUp * this.gain + (1 - this.kUp) * gain;

	out = this.delay[this.at];
	this.delay[this.at] = x;
	this.at = (this.at + 1) % this.delay.length;
	return out * this.gain;
};

/* the meter's held peak: a single sample must show, and fade over about a second rather than vanish */
function levelHold(held, peak, step) {
	var next = peak > held ? peak : held * step;

	return next > 1 ? 1 : (next < 0.0001 ? 0 : next);
}

globalThis.AwvoiceLevel = AwvoiceLevel;
globalThis.AWVOICE_LEVEL = {
	target: LEVEL_TARGET, ceiling: LEVEL_CEILING, gainMin: LEVEL_GAIN_MIN, gainMax: LEVEL_GAIN_MAX, floor: LEVEL_FLOOR,
	holdStep: 0.985, levelHold: levelHold
};
