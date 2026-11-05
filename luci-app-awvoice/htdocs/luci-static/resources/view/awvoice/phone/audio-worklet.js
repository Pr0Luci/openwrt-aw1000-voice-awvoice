// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * audio-worklet.js - the two processors of the page's browser audio (audio.js). Plain worklet code: no LuCI, no page.
 * It needs audio-level.js added first, which puts AwvoiceLevel on the worklet's global scope.
 *
 * awvoice-capture: the microphone at the audio context's rate, low-passed at 3.4 kHz (RBJ's biquad, Q = 1/sqrt2),
 * levelled to the send level this port owns (audio-level.js) and decimated to 8000 Hz; 160 samples (20 ms) are posted
 * at a time with the frame's peak for the level meter. At an 8 kHz context the filter and the decimation are skipped:
 * the engine is already the stream's rate, and only the level stage works. The S16 conversion keeps its clamp, but now as the last guard only - nothing should
 * reach it, because the limiter above has already held the frame under minus 3 dBFS.
 *
 * awvoice-player: 8 kHz frames into a small jitter queue (three frames is the start), interpolated up to the context's
 * rate; an empty queue is silence and the next three frames start it again. Nothing above 4 kHz exists in the stream,
 * so the interpolation only fills the higher rate's samples.
 *
 * The stream's samples are S16BE on the wire (the protocol's order, see web_proto.h in the daemon): a typed array is
 * the host's order, so the capture swaps its frame's bytes on the way out and the player swaps the frame it is given
 * on the way in. Without that swap the same samples are heard as noise, the way a byte-swapped voice always sounds.
 */

function lowpass(fs, fc) {
	var w0 = 2 * Math.PI * fc / fs, cos = Math.cos(w0), alpha = Math.sin(w0) / (2 * Math.SQRT1_2);
	var a0 = 1 + alpha;

	return {
		b0: ((1 - cos) / 2) / a0, b1: (1 - cos) / a0, b2: ((1 - cos) / 2) / a0,
		a1: (-2 * cos) / a0, a2: (1 - alpha) / a0
	};
}

class AwvoiceCapture extends AudioWorkletProcessor {
	constructor() {
		super();
		this.ratio = sampleRate / 8000;
		this.coef = sampleRate > 8000 ? lowpass(sampleRate, 3400) : null;
		this.level = new AwvoiceLevel(sampleRate);	/* the send level, computed here: see audio-level.js */
		this.x1 = 0; this.x2 = 0; this.y1 = 0; this.y2 = 0;
		this.ring = new Float32Array(64);
		this.idx = 0;			/* input samples seen */
		this.next = 0;			/* the input position of the next 8 kHz sample */
		this.out = new Int16Array(160);
		this.n = 0;
		this.peak = 0;
	}

	filtered(s) {
		if (!this.coef)
			return s;
		var y = this.coef.b0 * s + this.coef.b1 * this.x1 + this.coef.b2 * this.x2 - this.coef.a1 * this.y1 - this.coef.a2 * this.y2;

		this.x2 = this.x1; this.x1 = s;
		this.y2 = this.y1; this.y1 = y;
		return y;
	}

	process(inputs) {
		var ch = inputs[0] && inputs[0][0];

		if (!ch)
			return true;
		for (var i = 0; i < ch.length; i++) {
			var v = this.level.step(this.filtered(ch[i]));	/* band-limit, then level: the peaks in the stop
															   band must not push the limiter down */

			this.ring[this.idx & 63] = v;
			if (this.idx >= this.next) {
				var sample = this.ring[Math.round(this.next) & 63];
				var s16 = Math.max(-32768, Math.min(32767, Math.round(sample * 32768)));

				this.out[this.n++] = s16;
				this.peak = Math.max(this.peak, Math.abs(s16));
				this.next += this.ratio;
				if (this.n === 160) {
					var copy = this.out.buffer;
					var bytes = new Uint8Array(copy);
					var k, t;

					for (k = 0; k < bytes.length; k += 2) {	/* host order to the stream's S16BE */
						t = bytes[k];
						bytes[k] = bytes[k + 1];
						bytes[k + 1] = t;
					}
					this.port.postMessage({ pcm: copy, level: this.peak / 32768 }, [ copy ]);
					this.out = new Int16Array(160);
					this.n = 0;
					this.peak = 0;
				}
			}
			this.idx++;
		}
		return true;
	}
}

class AwvoicePlayer extends AudioWorkletProcessor {
	constructor() {
		super();
		this.ratio = 8000 / sampleRate;
		this.ring = new Float32Array(2048);	/* a little over 0.25 s of 8 kHz speech */
		this.head = 0; this.tail = 0;
		this.pos = 0;
		this.playing = false;
		this.port.onmessage = (e) => {
			var pcm, bytes, t, k;

			try {
				bytes = new Uint8Array(e.data);
			} catch (err) {
				return;
			}
			if (bytes.length % 2)
				return;
			for (k = 0; k < bytes.length; k += 2) {	/* the stream's S16BE to host order */
				t = bytes[k];
				bytes[k] = bytes[k + 1];
				bytes[k + 1] = t;
			}
			pcm = new Int16Array(bytes.buffer);
			for (var n = 0; n < pcm.length; n++) {
				if (this.tail - this.head >= this.ring.length)
					this.head++;		/* the far end outran the speaker: the oldest sample goes */
				this.ring[this.tail & (this.ring.length - 1)] = pcm[n] / 32768;
				this.tail++;
			}
			if (!this.playing && this.tail - this.head >= 480)	/* three frames: the queue's start */
				this.playing = true;
		};
	}

	sample(pos) {
		var a = this.ring[this.head & (this.ring.length - 1)];
		var b = this.head + 1 < this.tail ? this.ring[(this.head + 1) & (this.ring.length - 1)] : a;

		return a + (b - a) * pos;
	}

	process(inputs, outputs) {
		var out = outputs[0] && outputs[0][0];

		if (!out)
			return true;
		for (var i = 0; i < out.length; i++) {
			if (!this.playing || this.head >= this.tail) {
				out[i] = 0;
				this.playing = false;
				continue;
			}
			out[i] = this.sample(this.pos);
			this.pos += this.ratio;
			while (this.pos >= 1 && this.head < this.tail) {
				this.head++;
				this.pos -= 1;
			}
		}
		return true;
	}
}

registerProcessor('awvoice-capture', AwvoiceCapture);
registerProcessor('awvoice-player', AwvoicePlayer);
