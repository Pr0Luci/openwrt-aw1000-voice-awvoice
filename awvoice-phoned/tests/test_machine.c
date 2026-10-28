// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The answering machine's portable half: the greeting's WAV read and its two rates, the message file: make -f Makefile.host test */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "machine.h"

static char dir[64];

static void fresh_dir(void)
{
	snprintf(dir, sizeof(dir), "/tmp/aw-machine-XXXXXX");
	assert(mkdtemp(dir));
}

static void wav_write(const char *path, u32 rate, u32 channels, const int16_t *samples, u32 frames, u32 claimed_data)
{
	unsigned char hdr[44];
	u32 data = frames * channels * 2;
	FILE *f;

	assert(recording_wav_header(hdr, rate, channels, claimed_data ? claimed_data : data) == 44);
	f = fopen(path, "w");
	assert(f);
	assert(fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
	assert(fwrite(samples, sizeof(*samples), frames * channels, f) == frames * channels);
	fclose(f);
}

static void wav_patch(const char *path, size_t at, u32 value)
{
	unsigned char hdr[44];
	FILE *f = fopen(path, "r+");

	assert(f && fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
	recording_put_le32(hdr + at, value);
	assert(fseek(f, 0, SEEK_SET) == 0 && fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
	fclose(f);
}

static const int16_t eight[] = { 100, 200, 400, 800 };

static void test_greeting_8k(void)
{
	struct machine m;
	char path[128];
	int err;

	fresh_dir();
	snprintf(path, sizeof(path), "%s/g.wav", dir);
	wav_write(path, 8000, 1, eight, 4, 0);
	machine_init(&m);
	err = machine_greeting_load(&m, path, NULL);
	assert(err == 0 && m.enabled);
	assert(m.frames[0] == 4 && m.frames[1] == 8);
	assert(m.greeting[0][0] == 100 && m.greeting[0][3] == 800);
	/* 8 -> 16 kHz by linear interpolation: held, averaged, last sample repeated at the edge */
	assert(m.greeting[1][0] == 100 && m.greeting[1][1] == 150 && m.greeting[1][2] == 200 && m.greeting[1][3] == 300);
	assert(m.greeting[1][4] == 400 && m.greeting[1][5] == 600 && m.greeting[1][6] == 800 && m.greeting[1][7] == 800);

	/* the narrowband call: one sample a play frame */
	assert(machine_start(&m, 8000) == 0 && m.playing && m.ring_frames == 4);
	{
		int16_t out[6];

		machine_fill(&m, out, 3);
		assert(out[0] == 100 && out[1] == 200 && out[2] == 400);
		assert(m.playing);
		machine_fill(&m, out, 3);
		assert(out[0] == 800 && out[1] == 0 && out[2] == 0);	/* the rest of the call is silence */
		assert(!m.playing && m.ring_frames == 4);
	}
	/* the wideband call: two samples a play frame, so half the frames */
	assert(machine_start(&m, 16000) == 0 && m.ring_frames == 4);
	{
		int16_t out[8];

		machine_fill(&m, out, 4);
		assert(out[0] == 100 && out[1] == 150 && out[2] == 200 && out[3] == 300);
		assert(out[4] == 400 && out[5] == 600 && out[6] == 800 && out[7] == 800);
		assert(!m.playing);
		machine_stop(&m);
		assert(!m.playing && m.ring_frames == 0);
	}
	machine_free(&m);
}

static void test_greeting_16k_stereo(void)
{
	struct machine m;
	char path[128];
	const int16_t pairs[] = { 100, 300, 400, 600, 1000, 2000, 3000, 5000 };

	snprintf(path, sizeof(path), "%s/g16.wav", dir);
	wav_write(path, 16000, 2, pairs, 4, 0);
	machine_init(&m);
	assert(machine_greeting_load(&m, path, NULL) == 0 && m.frames[1] == 4 && m.frames[0] == 2);
	/* stereo -> mono first: the mean of the two channels */
	assert(m.greeting[1][0] == 200 && m.greeting[1][1] == 500 && m.greeting[1][2] == 1500 && m.greeting[1][3] == 4000);
	/* then 16 -> 8 kHz through [1 2 1]/4: (200 + 2*200 + 500) / 4 and (500 + 2*1500 + 4000) / 4 */
	assert(m.greeting[0][0] == 275 && m.greeting[0][1] == 1875);
	assert(machine_start(&m, 8000) == 0 && m.ring_frames == 2);
	machine_free(&m);
}

static void test_refusals(void)
{
	struct machine m;
	char path[128], other[128];
	char *text = NULL;
	size_t text_len = 0;
	FILE *log;

	fresh_dir();
	machine_init(&m);
	/* a missing greeting: named, the machine stays off, nothing loaded */
	snprintf(path, sizeof(path), "%s/none.wav", dir);
	assert(machine_greeting_load(&m, path, NULL) == -ENOENT && !m.enabled);
	/* a good one, then a bad reload: the old greeting goes with it (never half loaded) */
	snprintf(other, sizeof(other), "%s/good.wav", dir);
	wav_write(other, 8000, 1, eight, 4, 0);
	assert(machine_greeting_load(&m, other, NULL) == 0 && m.enabled);

	log = open_memstream(&text, &text_len);
	snprintf(path, sizeof(path), "%s/empty.wav", dir);
	{
		FILE *f = fopen(path, "w");

		assert(f);
		fclose(f);
	}
	assert(machine_greeting_load(&m, path, log) == -EINVAL && !m.enabled && !m.greeting[0]);
	assert(machine_greeting_load(&m, other, log) == 0);
	/* a header that claims more data than the file holds: a copy cut short */
	snprintf(path, sizeof(path), "%s/cut.wav", dir);
	wav_write(path, 8000, 1, eight, 4, 4 * 2 + 10);
	assert(machine_greeting_load(&m, path, log) == -EBADMSG && !m.enabled);
	assert(machine_greeting_load(&m, other, log) == 0);
	/* not our rate and not our sample size */
	wav_write(path, 8000, 1, eight, 4, 0);
	wav_patch(path, 24, 44100);
	assert(machine_greeting_load(&m, path, log) == -EINVAL && !m.enabled);
	assert(machine_greeting_load(&m, other, log) == 0);
	wav_write(path, 8000, 1, eight, 4, 0);
	wav_patch(path, 34, 8);
	assert(machine_greeting_load(&m, path, log) == -EINVAL);
	/* a directory is not a file */
	assert(machine_greeting_load(&m, dir, log) == -EINVAL);
	/* larger than a greeting could be: refused before it is read */
	{
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

		assert(fd >= 0 && ftruncate(fd, MACHINE_MAX_BYTES + 1) == 0);
		close(fd);
	}
	assert(machine_greeting_load(&m, path, log) == -EFBIG);
	/* one line per refusal, naming the file, never its bytes */
	fflush(log);
	assert(text && strstr(text, "empty.wav") && strstr(text, "stays off"));
	assert(!strstr(text, "garbage"));
	fclose(log);
	free(text);
	/* no greeting at all: there is nothing to play, and the caller would hear only silence */
	assert(machine_start(&m, 8000) == -ENOENT && !m.playing);
	machine_free(&m);
}

static void test_message(void)
{
	struct machine m;
	char path[160];
	FILE *f;
	struct stat st;
	unsigned char hdr[44];
	u32 rate = 0, channels = 0, data = 0;
	const int16_t voice[] = { 10, -20, 30, -40, 50 };

	fresh_dir();
	machine_init(&m);
	assert(machine_message_open(&m, 7, 11025, dir, "1-7") == -EINVAL);	/* never: the recorder refuses the rate */
	assert(!m.recording);
	assert(machine_message_open(&m, 7, 8000, dir, "1700000000-7") == 0 && m.recording && !m.saved);
	assert(!strcmp(m.name, "1700000000-7.wav"));
	assert(machine_message_open(&m, 8, 8000, dir, "1700000001-8") == -EBUSY);	/* one at a time */
	assert(machine_message_feed(&m, voice, 5) == 0 && m.rec_frames == 5);
	assert(machine_message_finish(&m) == 10 && m.saved && !m.recording);
	snprintf(path, sizeof(path), "%s/%s", dir, m.name);
	assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_size == 44 + 10);
	f = fopen(path, "r");
	assert(f && fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && recording_wav_probe(hdr, &rate, &channels, &data) == 0);
	{
		int16_t back[5];

		assert(rate == 8000 && channels == 1 && data == 10);
		assert(fread(back, sizeof(*back), 5, f) == 5 && !memcmp(back, voice, sizeof(voice)));
	}
	fclose(f);
	/* a message that ends with no sample is discarded, not left as a header-only entry */
	assert(machine_message_open(&m, 9, 16000, dir, "1700000002-9") == 0);
	assert(machine_message_finish(&m) == 0 && !m.saved && !m.name[0] && !m.recording);
	snprintf(path, sizeof(path), "%s/1700000002-9.wav", dir);
	assert(access(path, F_OK) != 0);
	/* an aborted message leaves nothing behind under any name */
	assert(machine_message_open(&m, 10, 8000, dir, "1700000003-10") == 0);
	assert(machine_message_feed(&m, voice, 2) == 0);
	machine_message_abort(&m);
	assert(!m.recording && !m.name[0]);
	snprintf(path, sizeof(path), "%s/1700000003-10.wav", dir);
	assert(access(path, F_OK) != 0);
	snprintf(path, sizeof(path), "%s/1700000003-10.wav.tmp", dir);
	assert(access(path, F_OK) != 0);
	/* an ending process with a message open: the tmp goes too */
	assert(machine_message_open(&m, 11, 8000, dir, "1700000004-11") == 0);
	machine_message_feed(&m, voice, 3);
	machine_free(&m);
	snprintf(path, sizeof(path), "%s/1700000004-11.wav.tmp", dir);
	assert(access(path, F_OK) != 0);
}

/* a closing message that the caller hangs up on mid-feed: the failed write is reported and nothing is left */
static void test_feed_after_abort(void)
{
	struct machine m;
	char name[64] = "5-9";

	machine_init(&m);
	assert(machine_message_feed(&m, eight, 4) == 0);	/* nothing open: a no-op, not an error */
	assert(machine_message_open(&m, 1, 8000, dir, name) == 0);
	machine_message_abort(&m);
	assert(machine_message_feed(&m, eight, 4) == 0 && machine_message_finish(&m) == 0);
	machine_free(&m);
}

int main(void)
{
	fresh_dir();
	test_greeting_8k();
	test_greeting_16k_stereo();
	test_refusals();
	test_message();
	test_feed_after_abort();
	printf("machine: all tests passed\n");
	return 0;
}
