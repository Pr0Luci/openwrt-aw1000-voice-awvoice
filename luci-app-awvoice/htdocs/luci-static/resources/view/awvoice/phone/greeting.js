// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';

/*
 * greeting.js - what the page decides about the answering machine's one greeting file, kept in one place so the card,
 * the uploader and the recorder cannot drift into different rules: the file's fixed place, the shape the daemon plays,
 * the size of a piece, the words the daemon refuses with, and what the card may offer from a health reply.
 *
 * No DOM and no ubus here. The numbers are the daemon's own (machine.h, api.c, greeting_audio.h) and the tests
 * (tests/live.test.js) run these against them, so a change on one side that is not made on the other fails a test
 * rather than surprising a user.
 */

/* the daemon's own constant (machine.h MACHINE_GREETING_PATH): the page never picks a place for the greeting */
var PATH = '/etc/awvoice/greeting.wav';
var MAX_BYTES = 2 * 1024 * 1024;	/* MACHINE_MAX_BYTES: the byte over it is refused */
var PUT_BYTES = 16384;		/* MACHINE_GREETING_PUT_MAX: the plain bytes one piece may carry */
var GET_BYTES = 24576;		/* RECORDING_CHUNK: the raw bytes one reply carries back */
var HEADER = 44;		/* the canonical RIFF/WAVE header the daemon writes and reads */
var RATE = 8000;		/* what the page always makes: the narrowband rate every call can play */
var TONE_MS = 700;		/* greeting_audio.h: the tone the daemon adds to a recording, none of it the page's */

/* the words that mean there is nothing on the router to listen to: Play has no file to fetch */
var NO_FILE = { MISSING: 1, UNREADABLE: 1, IO: 1 };

/* what the card may offer, from the health reply alone: the daemon's own answer about the one file it plays */
function card(health) {
	var m = health && health.machine ? health.machine : null;

	if (!m || m.greetingLoaded == null)
		return { known: false, loaded: false, ms: 0, word: '', playable: false };
	return {
		known: true,
		loaded: m.greetingLoaded === true,
		ms: m.greetingMs || 0,
		word: m.greetingError || '',
		/* a file that is there can be listened to even when the machine cannot play it: hearing what stands on the
		 * router is how a wrong greeting is recognised. Only "there is nothing to read" turns Play off. */
		playable: m.greetingLoaded === true || !NO_FILE[m.greetingError]
	};
}

/*
 * The sentence for one refusal word, said where the user can act on it. An unknown word is answered too - with itself
 * named - because silence about a refusal is how a greeting ends up unexplained.
 */
function sentence(word) {
	if (!word)
		return '';
	switch (word) {
	case 'MISSING': return _('There is no greeting yet. Record one or upload a WAV.');
	case 'FORMAT': return _('That is not a 16-bit PCM WAV at 8 or 16 kHz.');
	case 'EMPTY': return _('The greeting holds no audio. Record again or choose another file.');
	case 'TRUNCATED': return _('The greeting file ends before its own header says it should. Send it again.');
	case 'TOO_BIG': return _('The greeting is larger than 2 MB. Use a shorter one.');
	case 'MEMORY': return _('The router had no room to read the greeting. Try a shorter one.');
	case 'SYMLINK': return _('The greeting is a link to another file. Replace the link with the file itself.');
	case 'BUSY': return _('A call is using the line. Try again when it is over.');
	case 'OFFSET': return _('The upload lost its place. Send the file again.');
	case 'IO': return _('The router could not write the greeting. Try again.');
	case 'UNREADABLE': return _('The greeting file cannot be read. Check that it is a WAV on the router.');
	default: return _('The phone service would not take the greeting (%s).').format(word);
	}
}

/*
 * Is this file worth sending at all? A file the daemon cannot play is refused by the daemon anyway, whatever the page
 * guessed first; this only saves a transfer for something that is not a WAV, or is bigger than a greeting can be.
 * The rate and the channel count of the author's file are the page's job to fix, not the user's, so a 44.1 kHz stereo
 * WAV is not refused here - it is converted.
 */
function rejectWav(bytes, size) {
	if (!bytes || bytes.length < 12)
		return 'FORMAT';
	if (size > MAX_BYTES)
		return 'TOO_BIG';
	if (bytes[0] !== 0x52 || bytes[1] !== 0x49 || bytes[2] !== 0x46 || bytes[3] !== 0x46)
		return 'FORMAT';			/* not "RIFF": not a WAV file */
	if (bytes[8] !== 0x57 || bytes[9] !== 0x41 || bytes[10] !== 0x56 || bytes[11] !== 0x45)
		return 'FORMAT';			/* not "WAVE" */
	if (size < HEADER)
		return 'TRUNCATED';			/* shorter than a whole header: a copy that stopped */
	return null;
}

/* the offsets and sizes a put loop walks, from 0 to the whole file, in pieces no bigger than `piece` */
function plan(total, piece) {
	var out = [], at = 0, size;

	piece = piece || PUT_BYTES;
	if (!(total > 0) || !(piece > 0))
		return out;
	while (at < total) {
		size = Math.min(piece, total - at);
		out.push({ offset: at, size: size });
		at += size;
	}
	return out;
}

/* the daemon's playback frames are big-endian S16 (web_proto.h); a WAV is little-endian: one swap between them. A
 * trailing half sample cannot be a sample of anything, so it is dropped rather than carried over as a byte of noise. */
function swap16(bytes) {
	var n = bytes.length - (bytes.length % 2), out = new Uint8Array(n), k;

	for (k = 0; k < n; k += 2) {
		out[k] = bytes[k + 1];
		out[k + 1] = bytes[k];
	}
	return out;
}

function put16(view, at, value) {
	view.setUint16(at, value & 0xffff, true);
}

function put32(view, at, value) {
	view.setUint32(at, value >>> 0, true);
}

/*
 * The canonical 44-byte header of a 16-bit PCM WAV - the bytes the daemon's own writer produces (recording.h) - so a
 * file the page makes is readable by the machine with nothing to convert. A rate that is not one of the bus's two, or
 * a channel count that is not one or two, is refused here rather than written: the daemon would refuse it after the
 * transfer, and the user would wait for the answer.
 */
function wavHeader(view, samples, rate, channels) {
	if ((rate !== 8000 && rate !== 16000) || (channels !== 1 && channels !== 2) || !(samples >= 0))
		return false;
	view.setUint8(0, 0x52); view.setUint8(1, 0x49); view.setUint8(2, 0x46); view.setUint8(3, 0x46);	/* RIFF */
	put32(view, 4, 36 + samples * 2);
	view.setUint8(8, 0x57); view.setUint8(9, 0x41); view.setUint8(10, 0x56); view.setUint8(11, 0x45);	/* WAVE */
	view.setUint8(12, 0x66); view.setUint8(13, 0x6d); view.setUint8(14, 0x74); view.setUint8(15, 0x20);	/* fmt  */
	put32(view, 16, 16);
	put16(view, 20, 1);					/* PCM */
	put16(view, 22, channels);
	put32(view, 24, rate);
	put32(view, 28, rate * channels * 2);
	put16(view, 32, channels * 2);
	put16(view, 34, 16);					/* bits a sample */
	view.setUint8(36, 0x64); view.setUint8(37, 0x61); view.setUint8(38, 0x74); view.setUint8(39, 0x61);	/* data */
	put32(view, 40, samples * 2);
	return true;
}

/* Int16 samples (mono, at `rate`) as one whole canonical WAV, ready for the pieces */
function wav(samples, rate) {
	var bytes = new Uint8Array(HEADER + samples.length * 2), view = new DataView(bytes.buffer), k;

	if (!wavHeader(view, samples.length, rate, 1))
		return null;
	for (k = 0; k < samples.length; k++)
		put16(view, HEADER + k * 2, samples[k] < 0 ? samples[k] + 65536 : samples[k]);
	return bytes;
}

/* Float32 in [-1, 1] as Int16, clamped at both ends: the shape the WAV and the bus both carry */
function to16(floats) {
	var out = new Int16Array(floats.length), k, v;

	for (k = 0; k < floats.length; k++) {
		v = Math.round(Math.max(-1, Math.min(0.999969, floats[k])) * 32768);
		out[k] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
	}
	return out;
}

var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

/* standard base64 (RFC 4648) with the padding: what the daemon's reader accepts, piece for piece */
function b64(bytes) {
	var out = '', k, a, b, c;

	for (k = 0; k + 2 < bytes.length; k += 3) {
		a = bytes[k]; b = bytes[k + 1]; c = bytes[k + 2];
		out += B64.charAt(a >> 2) + B64.charAt(((a & 3) << 4) | (b >> 4)) +
			B64.charAt(((b & 15) << 2) | (c >> 6)) + B64.charAt(c & 63);
	}
	if (bytes.length - k === 1) {
		a = bytes[k];
		out += B64.charAt(a >> 2) + B64.charAt((a & 3) << 4) + '==';
	} else if (bytes.length - k === 2) {
		a = bytes[k]; b = bytes[k + 1];
		out += B64.charAt(a >> 2) + B64.charAt(((a & 3) << 4) | (b >> 4)) + B64.charAt((b & 15) << 2) + '=';
	}
	return out;
}

/* base64 back to bytes, for what the replies carry; a character outside the alphabet is no bytes at all */
function unb64(text) {
	var out = new Uint8Array(Math.floor(text.length * 3 / 4)), n = 0, v = 0, bits = 0, k, i, c;

	for (k = 0; k < text.length; k++) {
		c = text.charAt(k);
		if (c === '=')
			break;
		i = B64.indexOf(c);
		if (i < 0)
			return null;
		v = (v << 6) | i;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[n++] = (v >> bits) & 0xff;
		}
	}
	return out.subarray(0, n);
}

/* the raw bytes one base64 reply carries: the contract's own way to name the next offset */
function rawLen(text) {
	var tail = String(text || '').replace(/=+$/, '');

	return Math.floor(tail.length * 6 / 8);
}

return baseclass.extend({
	PATH: PATH,
	MAX_BYTES: MAX_BYTES,
	PUT_BYTES: PUT_BYTES,
	GET_BYTES: GET_BYTES,
	HEADER: HEADER,
	RATE: RATE,
	TONE_MS: TONE_MS,
	card: card,
	sentence: sentence,
	rejectWav: rejectWav,
	plan: plan,
	swap16: swap16,
	wav: wav,
	to16: to16,
	b64: b64,
	unb64: unb64,
	rawLen: rawLen
});
