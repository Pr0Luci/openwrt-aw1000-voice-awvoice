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
static void check_header(u32 rate, u32 data_bytes)
{
	unsigned char buf[64];

	memset(buf, 0xAA, sizeof(buf));
	assert(recording_wav_header(buf, rate, data_bytes) == 44);
	assert(!memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4));
	assert(!memcmp(buf + 12, "fmt ", 4) && !memcmp(buf + 36, "data", 4));
	assert(le32(buf + 4) == 36 + data_bytes);		/* the RIFF chunk covers the rest of the file */
	assert(le32(buf + 16) == 16);				/* the fmt chunk's size */
	assert(le16(buf + 20) == 1);				/* PCM */
	assert(le16(buf + 22) == 1);				/* mono */
	assert(le32(buf + 24) == rate);
	assert(le32(buf + 28) == rate * 2);			/* byte rate */
	assert(le16(buf + 32) == 2);				/* block align */
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

/* a recording's header read back: the rate and the size a playback UI needs; anything else is refused */
static void test_wav_probe(void)
{
	unsigned char hdr[44];
	u32 rate = 0, data = 0;

	memset(hdr, 0, sizeof(hdr));
	assert(recording_wav_header(hdr, 8000, 1234) == 44);
	assert(recording_wav_probe(hdr, &rate, &data) == 0 && rate == 8000 && data == 1234);
	assert(recording_wav_header(hdr, 16000, 987654u) == 44);
	assert(recording_wav_probe(hdr, &rate, &data) == 0 && rate == 16000 && data == 987654u);
	hdr[0] = 'X';
	assert(recording_wav_probe(hdr, &rate, &data) == -EINVAL);
	hdr[0] = 'R';
	hdr[34] = 8;					/* not 16-bit */
	assert(recording_wav_probe(hdr, &rate, &data) == -EINVAL);
	hdr[34] = 16;
	hdr[24] = 0x44; hdr[25] = 0xac; hdr[26] = 0; hdr[27] = 0;	/* 44100: not one of the bus's rates */
	assert(recording_wav_probe(hdr, &rate, &data) == -EINVAL);
}

int main(void)
{
	/* both rates the PCM bus runs at, empty at the start of a call, one second of 8 kHz, and sizes past 16 bits */
	check_header(8000, 0);
	check_header(16000, 0);
	check_header(8000, 16000);
	check_header(16000, 32000);
	check_header(8000, 12345678u);
	check_header(16000, 0xABCDEFu);
	test_base64();
	test_wav_probe();

	/* a rate the PCM bus never runs at is refused whole: 0 returned and not one byte written */
	{
		unsigned char buf[64];

		memset(buf, 0xAA, sizeof(buf));
		assert(recording_wav_header(buf, 44100, 1000) == 0);
		assert(recording_wav_header(buf, 0, 1000) == 0);
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
