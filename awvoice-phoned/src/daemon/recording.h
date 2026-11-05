/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_RECORDING_H
#define AW_PHONED_RECORDING_H

/*
 * The portable core of call recording: the canonical 44-byte RIFF/WAVE header of a 16-bit mono PCM file, and the
 * retention picker for the recordings directory. The header's multi-byte fields are written byte by byte so the
 * layout is exact on any host; only the two rates the PCM bus runs at are accepted and anything else is refused
 * before a byte is written. Retention deletes from the front (the oldest) only and never the last remaining entry:
 * if even one recording alone is over budget it stays and the caller must refuse new recordings instead. Plain
 * arithmetic on purpose: it is tested on the host (tests/test_recording.c).
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

static inline void recording_put_le16(unsigned char *p, u32 v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static inline void recording_put_le32(unsigned char *p, u32 v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

/* the 44-byte RIFF/WAVE header for 16-bit PCM at `rate` (1 = mono, 2 = the far end and the handset);
 * only 8000 and 16000 are rates and only 1 or 2 are channels, else 0 and nothing written */
static inline u32 recording_wav_header(void *out, u32 rate, u32 channels, u32 data_bytes)
{
	unsigned char *p = out;

	if ((rate != 8000 && rate != 16000) || (channels != 1 && channels != 2))
		return 0;
	p[0] = 'R';
	p[1] = 'I';
	p[2] = 'F';
	p[3] = 'F';
	recording_put_le32(p + 4, 36 + data_bytes);
	p[8] = 'W';
	p[9] = 'A';
	p[10] = 'V';
	p[11] = 'E';
	p[12] = 'f';
	p[13] = 'm';
	p[14] = 't';
	p[15] = ' ';
	recording_put_le32(p + 16, 16);		/* the fmt chunk's size */
	recording_put_le16(p + 20, 1);		/* PCM */
	recording_put_le16(p + 22, channels);
	recording_put_le32(p + 24, rate);
	recording_put_le32(p + 28, rate * channels * 2);	/* byte rate: 16-bit samples, two bytes per frame per channel */
	recording_put_le16(p + 32, channels * 2);	/* block align */
	recording_put_le16(p + 34, 16);		/* bits per sample */
	p[36] = 'd';
	p[37] = 'a';
	p[38] = 't';
	p[39] = 'a';
	recording_put_le32(p + 40, data_bytes);
	return 44;
}

/* how many leading (oldest) entries of `n` to delete so the rest fits the byte budget; the last remaining entry stays */
static inline u32 recording_prune_count(const u32 *bytes, u32 n, u32 budget)
{
	u32 total = 0, deleted = 0, k;

	for (k = 0; k < n; k++)
		total += bytes[k];
	if (!n)
		return 0;
	while (deleted < n - 1 && total > budget)
		total -= bytes[deleted++];
	return deleted;
}

/* the recordings go to the page over ubus, whose replies are small: the bytes travel base64-encoded in chunks
 * (RFC 4648, the standard alphabet with the padding). `out` needs 4 bytes for every 3 of `len`. */
static inline u32 recording_base64(const void *in, u32 len, char *out)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const unsigned char *p = in;
	u32 o = 0, k;

	for (k = 0; k + 3 <= len; k += 3) {
		u32 v = ((u32)p[k] << 16) | ((u32)p[k + 1] << 8) | p[k + 2];

		out[o++] = alphabet[(v >> 18) & 63];
		out[o++] = alphabet[(v >> 12) & 63];
		out[o++] = alphabet[(v >> 6) & 63];
		out[o++] = alphabet[v & 63];
	}
	if (len - k == 1) {
		u32 v = (u32)p[k] << 16;

		out[o++] = alphabet[(v >> 18) & 63];
		out[o++] = alphabet[(v >> 12) & 63];
		out[o++] = '=';
		out[o++] = '=';
	} else if (len - k == 2) {
		u32 v = ((u32)p[k] << 16) | ((u32)p[k + 1] << 8);

		out[o++] = alphabet[(v >> 18) & 63];
		out[o++] = alphabet[(v >> 12) & 63];
		out[o++] = alphabet[(v >> 6) & 63];
		out[o++] = '=';
	}
	out[o] = 0;
	return o;
}

/*
 * How many plain bytes a base64 string of `len` characters carries: three for every quad, less the padding that ends
 * it. A caller who caps an upload piece caps bytes, not characters, and a padded last quad is one or two bytes rather
 * than three - count the characters and a piece exactly at the cap looks two bytes over it and is refused for padding
 * that carries nothing. Padding away from the end stays in the count, which is then only an upper bound: the decoder
 * below is the one that refuses such a string. 0 or -EINVAL (no string, a length that is not a whole number of quads).
 */
static inline int recording_base64_plain_len(const char *in, u32 len, u32 *out)
{
	u32 pad = 0;

	*out = 0;
	if (!in || len % 4)
		return -EINVAL;
	if (!len)
		return 0;
	if (in[len - 1] == '=') {
		pad = 1;
		if (in[len - 2] == '=')
			pad = 2;
	}
	*out = len / 4 * 3 - pad;
	return 0;
}

/*
 * The way back, for what the page uploads: standard base64 (RFC 4648) decoded into `out`, whose capacity is `out_cap`.
 * Strict on purpose - a length that is not a whole number of quads, a character outside the alphabet, or padding that
 * is not the file's last two bytes at most is -EINVAL and nothing is written. A piece that decoded "as far as it could"
 * would be audio with a hole in it, and a greeting of a hole is worse than a refused upload.
 *
 * Returns 0 with the decoded length in `out_len`, or -EINVAL (a bad character, an unquaddable length, padding in the
 * wrong place) or -ENOSPC (the piece does not fit the caller's room).
 */
static inline int recording_base64_decode(const char *in, u32 len, unsigned char *out, u32 out_cap, u32 *out_len)
{
	static const signed char values[] = {
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,	/* '+' and '/' */
		52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,	/* A..Z then 0..9 */
		-1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
		15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
		-1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
		41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1
	};
	unsigned long long v;
	u32 o = 0, k, keep;

	*out_len = 0;
	if (!in || !out || len % 4)
		return -EINVAL;
	if (!len)
		return 0;
	for (k = 0; k < len; k += 4) {
		int pad = 0, j;

		for (j = 0; j < 4; j++) {
			unsigned char c = (unsigned char)in[k + j];

			if (c != '=' && (c > 127 || values[c] < 0))
				return -EINVAL;		/* a character that is not in the alphabet at all */
			if (c == '=') {
				/* padding only in the last quad, only in its last two places, and never followed by data */
				if (k + 4 != len || j < 2)
					return -EINVAL;
				pad++;
				continue;
			}
			if (pad)
				return -EINVAL;		/* data after padding: the length cannot be trusted */
		}
		keep = 3 - pad;
		if (o + keep > out_cap)
			return -ENOSPC;
		v = 0;
		for (j = 0; j < 4; j++) {
			unsigned char c = (unsigned char)in[k + j];
			int val = c == '=' ? 0 : values[c];

			/* the bits a padded group does not carry must be zero: an encoding that puts bits there decodes to
			 * the same bytes as another and is not the canonical form - two spellings of one file is a thing a
			 * checksum-free writer should not accept */
			if (pad == 1 && j == 2 && (val & 0x03))
				return -EINVAL;
			if (pad == 2 && j == 1 && (val & 0x0f))
				return -EINVAL;
			v = (v << 6) | (unsigned long long)val;
		}
		if (keep > 0)
			out[o++] = (unsigned char)(v >> 16);
		if (keep > 1)
			out[o++] = (unsigned char)(v >> 8);
		if (keep > 2)
			out[o++] = (unsigned char)v;
	}
	*out_len = o;
	return 0;
}

/* read a recording's header back: the rate, the channel count and the data size behind it. -EINVAL when the bytes
 * are not one this writer makes (mono or stereo 16-bit PCM at one of the bus's rates is all it ever writes). */
static inline int recording_wav_probe(const void *hdr44, u32 *rate, u32 *channels, u32 *data_bytes)
{
	const unsigned char *p = hdr44;
	u32 ch;

	if (memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4) || memcmp(p + 12, "fmt ", 4) || memcmp(p + 36, "data", 4))
		return -EINVAL;
	if (p[20] != 1 || p[21] != 0 || p[34] != 16 || p[35] != 0)
		return -EINVAL;			/* not 16-bit PCM */
	ch = (u32)p[22] | ((u32)p[23] << 8);
	if (ch != 1 && ch != 2)
		return -EINVAL;
	*rate = (u32)p[24] | ((u32)p[25] << 8) | ((u32)p[26] << 16) | ((u32)p[27] << 24);
	*channels = ch;
	*data_bytes = (u32)p[40] | ((u32)p[41] << 8) | ((u32)p[42] << 16) | ((u32)p[43] << 24);
	if ((*rate != 8000 && *rate != 16000) || p[32] != ch * 2 || p[33] != 0)
		return -EINVAL;
	return *data_bytes % (ch * 2) == 0 ? 0 : -EINVAL;
}

#endif /* AW_PHONED_RECORDING_H */
