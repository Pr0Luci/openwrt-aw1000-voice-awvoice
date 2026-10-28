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

/* read a recording's header back: the rate, the channel count and the data size behind it. -EINVAL when the bytes
 * are not one of ours (mono or stereo 16-bit PCM at one of the bus's rates is what we ever write). */
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
