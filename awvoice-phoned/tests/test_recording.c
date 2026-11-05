// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The recording core (pure): the WAV header field by field and the retention picker: make -f Makefile.host test */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "recording.h"

static u32 le16(const unsigned char *p)
{
	return p[0] | (u32)p[1] << 8;
}

static u32 le32(const unsigned char *p)
{
	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

/* every field of the canonical 44-byte header, compared against the layout; byte 44 must stay untouched */
static void check_header(u32 rate, u32 channels, u32 data_bytes)
{
	unsigned char buf[64];

	memset(buf, 0xAA, sizeof(buf));
	assert(recording_wav_header(buf, rate, channels, data_bytes) == 44);
	assert(!memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4));
	assert(!memcmp(buf + 12, "fmt ", 4) && !memcmp(buf + 36, "data", 4));
	assert(le32(buf + 4) == 36 + data_bytes);		/* the RIFF chunk covers the rest of the file */
	assert(le32(buf + 16) == 16);				/* the fmt chunk's size */
	assert(le16(buf + 20) == 1);				/* PCM */
	assert(le16(buf + 22) == channels);
	assert(le32(buf + 24) == rate);
	assert(le32(buf + 28) == rate * channels * 2);		/* byte rate */
	assert(le16(buf + 32) == channels * 2);			/* block align */
	assert(le16(buf + 34) == 16);				/* bits per sample */
	assert(le32(buf + 40) == data_bytes);
	assert(buf[44] == 0xAA);				/* exactly 44 bytes, nothing beyond */
}

/* RFC 4648's own vectors: the chunked fetch carries recordings as this encoding */
static void test_base64(void)
{
	char out[64];

	assert(recording_base64("", 0, out) == 0 && out[0] == 0);
	assert(recording_base64("f", 1, out) == 4 && !strcmp(out, "Zg=="));
	assert(recording_base64("fo", 2, out) == 4 && !strcmp(out, "Zm8="));
	assert(recording_base64("foo", 3, out) == 4 && !strcmp(out, "Zm9v"));
	assert(recording_base64("foob", 4, out) == 8 && !strcmp(out, "Zm9vYg=="));
	assert(recording_base64("fooba", 5, out) == 8 && !strcmp(out, "Zm9vYmE="));
	assert(recording_base64("foobar", 6, out) == 8 && !strcmp(out, "Zm9vYmFy"));
}

/* the way back, for what the page uploads: the same vectors round trip, and nothing that is not the encoding is decoded */
static void test_base64_decode(void)
{
	unsigned char out[64];
	u32 len = 12345;

	assert(recording_base64_decode("", 0, out, sizeof(out), &len) == 0 && len == 0);
	assert(recording_base64_decode("Zg==", 4, out, sizeof(out), &len) == 0 && len == 1 && out[0] == 'f');
	assert(recording_base64_decode("Zm8=", 4, out, sizeof(out), &len) == 0 && len == 2 && !memcmp(out, "fo", 2));
	assert(recording_base64_decode("Zm9v", 4, out, sizeof(out), &len) == 0 && len == 3 && !memcmp(out, "foo", 3));
	assert(recording_base64_decode("Zm9vYg==", 8, out, sizeof(out), &len) == 0 && len == 4 && !memcmp(out, "foob", 4));
	assert(recording_base64_decode("Zm9vYmE=", 8, out, sizeof(out), &len) == 0 && len == 5 && !memcmp(out, "fooba", 5));
	assert(recording_base64_decode("Zm9vYmFy", 8, out, sizeof(out), &len) == 0 && len == 6 && !memcmp(out, "foobar", 6));

	/* everything the alphabet holds, both ways: a piece of audio is not made of 'f' and 'o' alone */
	{
		char enc[1032];
		unsigned char bytes[768];
		u32 k;

		for (k = 0; k < 256; k++) {
			bytes[k] = (unsigned char)k;
			bytes[256 + k] = (unsigned char)k;
			bytes[512 + k] = (unsigned char)k;
		}
		recording_base64(bytes, 768, enc);
		assert(recording_base64_decode(enc, 1024, out, sizeof(out), &len) == -ENOSPC);	/* a room too small is refused */
		assert(recording_base64_decode(enc, 768 * 4 / 3, bytes, sizeof(bytes), &len) == 0 && len == 768);
		for (k = 0; k < 768; k++)
			assert(bytes[k] == (unsigned char)(k % 256));	/* nothing was skipped or shifted */
	}

	/* refusals: a length that is not a whole quad, a character outside the alphabet, padding in the wrong place */
	assert(recording_base64_decode("Z", 1, out, sizeof(out), &len) == -EINVAL);
	assert(recording_base64_decode("Zg", 2, out, sizeof(out), &len) == -EINVAL);
	assert(recording_base64_decode("Zm9", 3, out, sizeof(out), &len) == -EINVAL);
	assert(recording_base64_decode("Zg=A", 4, out, sizeof(out), &len) == -EINVAL);	/* data after padding */
	assert(recording_base64_decode("=m9v", 4, out, sizeof(out), &len) == -EINVAL);	/* padding in the first place */
	/* "Zm==" holds the same byte as "Zg==" with bits where the encoding has no room for them: the canonical form is
	 * the one that zeroes them, and a second spelling of one file is not accepted */
	assert(recording_base64_decode("Zm==", 4, out, sizeof(out), &len) == -EINVAL);
	assert(recording_base64_decode("Zm9=", 4, out, sizeof(out), &len) == -EINVAL);	/* one pad where none fits: a partial byte */
	assert(recording_base64_decode("Zm9*", 4, out, sizeof(out), &len) == -EINVAL);	/* not in the alphabet */
	assert(recording_base64_decode("Zm9 ", 4, out, sizeof(out), &len) == -EINVAL);	/* no whitespace is skipped */
	assert(recording_base64_decode("Zm9\x80", 4, out, sizeof(out), &len) == -EINVAL);	/* nor a byte past the ASCII range */
	assert(recording_base64_decode(NULL, 4, out, sizeof(out), &len) == -EINVAL);
	/* the URL-safe alphabet is a different encoding: this one is RFC 4648 and the page sends this one */
	assert(recording_base64_decode("-_8=", 4, out, sizeof(out), &len) == -EINVAL);

	/* the two characters the standard alphabet adds beyond A..Za..z0..9, and a padded tail */
	{
		char enc[8];
		unsigned char raw[2] = { 0xfb, 0xff };	/* the six-bit groups that are 62 and 63, then four bits of pad */

		recording_base64(raw, 2, enc);
		assert(!memcmp(enc, "+/8=", 4));
		assert(recording_base64_decode("+/8=", 4, out, sizeof(out), &len) == 0 && len == 2);
		assert(!memcmp(out, raw, 2));
	}
}

/* a recording's header read back: the rate and the size a playback UI needs; anything else is refused */
/*
 * The byte count an upload piece carries: three plain bytes for every quad, less the padding that ends it. Padding
 * carries no byte, so a count of the characters is two too high whenever the piece is not a multiple of three - and
 * the piece size the page sends is not, so a padded count turns a full piece into one that is over its own cap.
 */
static void test_base64_plain_len(void)
{
	u32 n;

	assert(recording_base64_plain_len("", 0, &n) == 0 && n == 0);
	assert(recording_base64_plain_len("Zg==", 4, &n) == 0 && n == 1);
	assert(recording_base64_plain_len("Zm8=", 4, &n) == 0 && n == 2);
	assert(recording_base64_plain_len("Zm9v", 4, &n) == 0 && n == 3);
	assert(recording_base64_plain_len("Zm9vYg==", 8, &n) == 0 && n == 4);
	assert(recording_base64_plain_len("Zm9vYmE=", 8, &n) == 0 && n == 5);
	assert(recording_base64_plain_len("Zm9vYmFy", 8, &n) == 0 && n == 6);

	/* a length that is not a whole quad is not the encoding, and there is no string to count at all */
	assert(recording_base64_plain_len("Zg", 2, &n) == -EINVAL);
	assert(recording_base64_plain_len("Zm9", 3, &n) == -EINVAL);
	assert(recording_base64_plain_len(NULL, 4, &n) == -EINVAL);

	/* padding away from the end stays counted: this is an upper bound only, and the decoder is the one that refuses
	 * the string, so a caller who caps by it can never take more bytes than it meant to */
	assert(recording_base64_plain_len("Zg=A", 4, &n) == 0 && n == 3);
	assert(recording_base64_plain_len("=m9v", 4, &n) == 0 && n == 3);

	/* the whole round: for every length, the encoder's own output counts back to exactly what went in - one byte is
	 * one pad of two characters, two bytes a pad of one, three none, and the count must follow all three */
	{
		static unsigned char bytes[300];
		char enc[404];
		u32 k;

		for (k = 0; k < sizeof(bytes); k++)
			bytes[k] = (unsigned char)(k * 7 + 3);
		for (k = 1; k <= sizeof(bytes); k++) {
			u32 elen = (u32)recording_base64(bytes, k, enc);

			assert(recording_base64_plain_len(enc, elen, &n) == 0 && n == k);
		}
	}
}

static void test_wav_probe(void)
{
	unsigned char hdr[44];
	u32 rate = 0, channels = 0, data = 0;

	memset(hdr, 0, sizeof(hdr));
	assert(recording_wav_header(hdr, 8000, 1, 1234) == 44);
	assert(recording_wav_probe(hdr, &rate, &channels, &data) == 0 && rate == 8000 && channels == 1 && data == 1234);
	assert(recording_wav_header(hdr, 16000, 2, 987652u) == 44);	/* stereo data comes in 4-byte frames */
	assert(recording_wav_probe(hdr, &rate, &channels, &data) == 0 && rate == 16000 && channels == 2 && data == 987652u);
	hdr[0] = 'X';
	assert(recording_wav_probe(hdr, &rate, &channels, &data) == -EINVAL);
	hdr[0] = 'R';
	hdr[34] = 8;					/* not 16-bit */
	assert(recording_wav_probe(hdr, &rate, &channels, &data) == -EINVAL);
	hdr[34] = 16;
	hdr[22] = 3;					/* not one or two channels */
	assert(recording_wav_probe(hdr, &rate, &channels, &data) == -EINVAL);
	hdr[22] = 2;
	hdr[32] = 2;					/* block align must match the channels */
	assert(recording_wav_probe(hdr, &rate, &channels, &data) == -EINVAL);
	hdr[24] = 0x44; hdr[25] = 0xac; hdr[26] = 0; hdr[27] = 0;	/* 44100: not one of the bus's rates */
	assert(recording_wav_probe(hdr, &rate, &channels, &data) == -EINVAL);
}

int main(void)
{
	/* both rates the PCM bus runs at, empty at the start of a call, one second of 8 kHz, and sizes past 16 bits */
	check_header(8000, 1, 0);
	check_header(16000, 1, 0);
	check_header(8000, 2, 16000);
	check_header(16000, 2, 32000);
	check_header(8000, 1, 12345678u);
	check_header(16000, 2, 0xABCDEFu);
	{
		unsigned char buf3[64];

		memset(buf3, 0xAA, sizeof(buf3));
		assert(recording_wav_header(buf3, 8000, 3, 0) == 0);	/* three channels is not a thing */
	}
	test_base64();
	test_base64_decode();
	test_base64_plain_len();
	test_wav_probe();

	/* a rate the PCM bus never runs at is refused whole: 0 returned and not one byte written */
	{
		unsigned char buf[64];

		memset(buf, 0xAA, sizeof(buf));
		assert(recording_wav_header(buf, 44100, 1, 1000) == 0);
		assert(recording_wav_header(buf, 0, 1, 1000) == 0);
		{
			u32 k;

			for (k = 0; k < sizeof(buf); k++)
				assert(buf[k] == 0xAA);
		}
	}

	/* an empty directory has nothing to delete */
	assert(recording_prune_count((const u32 *)0, 0, 1000) == 0);

	/* under and exactly at the budget: nothing goes */
	{
		const u32 sizes[] = { 100, 200, 300 };

		assert(recording_prune_count(sizes, 3, 1000) == 0);
		assert(recording_prune_count(sizes, 3, 600) == 0);
	}

	/* over by one entry: the oldest goes and the rest fit exactly */
	{
		const u32 sizes[] = { 100, 200, 300 };

		assert(recording_prune_count(sizes, 3, 599) == 1);
	}

	/* the front only: {2,3,1} fits 4 by dropping the leading 2; deleting from the back would name two entries */
	{
		const u32 sizes[] = { 2, 3, 1 };

		assert(recording_prune_count(sizes, 3, 4) == 1);
	}

	/* several deletions in a row, still never the last entry */
	{
		const u32 sizes[] = { 10, 20, 30, 40 };

		assert(recording_prune_count(sizes, 4, 60) == 3);
		assert(recording_prune_count(sizes, 4, 39) == 3);
	}

	/* one recording that alone is over the budget stays: nothing is deleted and the caller refuses new ones */
	{
		const u32 one[] = { 500 };
		const u32 two[] = { 100, 100 };

		assert(recording_prune_count(one, 1, 500) == 0);
		assert(recording_prune_count(one, 1, 499) == 0);
		assert(recording_prune_count(two, 2, 50) == 1);		/* the last one stays even though it alone is over */
	}

	/* a zero budget still leaves the last entry */
	{
		const u32 sizes[] = { 10, 20 };

		assert(recording_prune_count(sizes, 2, 0) == 1);
	}

	puts("recording: all tests passed");
	return 0;
}
