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
#include "greeting_audio.h"
#include "recording.h"

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
	assert(m.greeting_err == -ENOENT && !m.greeting_loaded);	/* nothing has been read yet: the honest word is "missing" */
	err = machine_greeting_load(&m, path, NULL);
	assert(err == 0 && m.greeting_loaded && !m.greeting_err);
	assert(m.frames[0] == 4 && m.frames[1] == 8);
	assert(m.greeting_ms == 0 && m.greeting_err == 0);		/* half a millisecond of four samples: the length the file is */
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
	assert(machine_greeting_load(&m, path, NULL) == -ENOENT && !m.greeting_loaded);
	assert(m.greeting_err == -ENOENT);	/* the read's own finding, kept for the status reply */
	/* a good one, then a bad reload: the old greeting goes with it (never half loaded) */
	snprintf(other, sizeof(other), "%s/good.wav", dir);
	wav_write(other, 8000, 1, eight, 4, 0);
	assert(machine_greeting_load(&m, other, NULL) == 0 && m.greeting_loaded && !m.greeting_err);

	log = open_memstream(&text, &text_len);
	snprintf(path, sizeof(path), "%s/empty.wav", dir);
	{
		FILE *f = fopen(path, "w");

		assert(f);
		fclose(f);
	}
	/* no bytes at all: shorter than a header is a file cut off, not a file of the wrong shape */
	assert(machine_greeting_load(&m, path, log) == -EBADMSG && !m.greeting_loaded && !m.greeting[0]);
	assert(machine_greeting_load(&m, other, log) == 0);
	/* a header that claims more data than the file holds: a copy cut short */
	snprintf(path, sizeof(path), "%s/cut.wav", dir);
	wav_write(path, 8000, 1, eight, 4, 4 * 2 + 10);
	assert(machine_greeting_load(&m, path, log) == -EBADMSG && !m.greeting_loaded);
	assert(machine_greeting_load(&m, other, log) == 0);
	/* a header of a file with no audio behind it: silence is not a greeting, and it is a different answer from
	 * "that is not a WAV the machine plays", so it has its own errno */
	snprintf(path, sizeof(path), "%s/nodata.wav", dir);
	wav_write(path, 8000, 1, eight, 0, 0);
	assert(machine_greeting_load(&m, path, log) == -ENODATA && !m.greeting_loaded);
	assert(machine_greeting_load(&m, other, log) == 0);
	/* not the rate and not the sample size the machine plays */
	snprintf(path, sizeof(path), "%s/rate.wav", dir);
	wav_write(path, 8000, 1, eight, 4, 0);
	wav_patch(path, 24, 44100);
	assert(machine_greeting_load(&m, path, log) == -EINVAL && !m.greeting_loaded);
	assert(machine_greeting_load(&m, other, log) == 0);
	wav_write(path, 8000, 1, eight, 4, 0);
	wav_patch(path, 34, 8);
	assert(machine_greeting_load(&m, path, log) == -EINVAL);
	/* a directory is not a file at all, and a fifo is not one either: both say "this cannot be read as audio",
	 * neither says "the audio is wrong", because nothing was looked at */
	assert(machine_greeting_load(&m, dir, log) == -EISDIR);
	snprintf(path, sizeof(path), "%s/pipe.wav", dir);
	assert(mkfifo(path, 0600) == 0);
	assert(machine_greeting_load(&m, path, log) == -EOPNOTSUPP);
	/* larger than a greeting could be: refused before it is read */
	snprintf(path, sizeof(path), "%s/huge.wav", dir);
	{
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

		assert(fd >= 0 && ftruncate(fd, MACHINE_MAX_BYTES + 1) == 0);
		close(fd);
	}
	assert(machine_greeting_load(&m, path, log) == -EFBIG);
	/* one line per refusal, naming the file, never its bytes */
	fflush(log);
	assert(text && strstr(text, "empty.wav") && strstr(text, "stays off"));
	assert(strstr(text, "Phone settings page"));		/* the way out is named where the log is read */
	assert(!strstr(text, "machine_greeting"));		/* the retired option is not advice any more */
	assert(!strstr(text, "garbage"));
	fclose(log);
	free(text);
	/* no greeting at all: there is nothing to play, and the caller would hear only silence */
	assert(machine_start(&m, 8000) == -ENOENT && !m.playing);
	machine_free(&m);
	assert(!m.greeting_loaded && !m.greeting_ms && m.greeting_err == -ENOENT);	/* the drop leaves the plain state: nothing there */
}

/* ---- the greeting the page uploads: pieces staged in the folder's tmp, proved, and only then in the place ---- */

static char up_dir[96], up_final[192], up_tmp[192];

static void upload_paths(void)
{
	snprintf(up_final, sizeof(up_final), "%s/" MACHINE_GREETING_NAME, up_dir);
	snprintf(up_tmp, sizeof(up_tmp), "%s/" MACHINE_GREETING_TMP, up_dir);
}

/* a whole WAV in one buffer, the way the page would send it */
static u32 wav_bytes(unsigned char *out, u32 rate, u32 channels, const int16_t *samples, u32 frames)
{
	u32 data = frames * channels * 2;

	assert(recording_wav_header(out, rate, channels, data) == 44);
	memcpy(out + 44, samples, data);
	return 44 + data;
}

static void le32_at(unsigned char *p, u32 value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
	p[2] = (unsigned char)(value >> 16);
	p[3] = (unsigned char)(value >> 24);
}

/* stage a whole file in small pieces and commit it: the path every upload takes, in one call */
static int staged(struct machine *m, struct machine_upload *up, const unsigned char *bytes, u32 len, int polish,
	unsigned int *ms)
{
	u32 at = 0;
	int err;

	assert(machine_upload_begin(up, m, up_dir) == 0);
	while (at < len) {
		u32 take = len - at > 97 ? 97 : len - at;

		err = machine_upload_put(up, m, up_dir, at, bytes + at, take);
		if (err)
			return err;
		at += take;
	}
	return machine_upload_commit(up, m, up_dir, polish, ms);
}

static void test_upload_happy(void)
{
	struct machine m;
	struct machine_upload up;
	static int16_t voice[64];
	unsigned char file[512];
	unsigned int ms = 99;
	struct stat st;
	u32 len;
	int k;

	fresh_dir();
	snprintf(up_dir, sizeof(up_dir), "%s/managed", dir);	/* a folder that is not there yet: begin makes it */
	upload_paths();
	machine_init(&m);
	machine_upload_init(&up);
	for (k = 0; k < 64; k++)
		voice[k] = (int16_t)(1000 + k * 100);

	len = wav_bytes(file, 8000, 1, voice, 64);
	assert(staged(&m, &up, file, len, 0, &ms) == 0);
	assert(stat(up_final, &st) == 0 && st.st_size == 44 + 128);
	assert((st.st_mode & 0777) == 0600);			/* the household's own voice, not world readable */
	assert(access(up_tmp, F_OK) != 0);				/* the tmp went with the rename */
	assert(stat(up_dir, &st) == 0 && (st.st_mode & 0700) == 0700);	/* the folder the daemon made is its own */
	assert(m.greeting_loaded && !m.greeting_err && m.frames[0] == 64);
	assert(ms == 8 && m.greeting_ms == 8);			/* 64 samples at 8 kHz: eight milliseconds, from the load */
	assert(!memcmp(m.greeting[0], voice, sizeof(voice)));

	/* a second upload replaces the file and the audio, it does not join on to it */
	for (k = 0; k < 64; k++)
		voice[k] = (int16_t)(-7);
	len = wav_bytes(file, 16000, 2, voice, 32);
	assert(staged(&m, &up, file, len, 0, &ms) == 0);
	assert(m.frames[1] == 32 && m.frames[0] == 16 && m.greeting_ms == 2);
	assert(m.greeting[1][0] == -7);
	assert(stat(up_final, &st) == 0 && st.st_size == 44 + 128);
	machine_free(&m);
}

static void test_upload_protocol(void)
{
	struct machine m;
	struct machine_upload up;
	unsigned char file[512];
	static int16_t voice[64];

	fresh_dir();
	snprintf(up_dir, sizeof(up_dir), "%s/managed2", dir);
	upload_paths();
	machine_init(&m);
	machine_upload_init(&up);
	assert(wav_bytes(file, 8000, 1, voice, 64) == 44 + 128);	/* the header and the audio the pieces carry */

	/* a piece with nothing staged: the calls are out of order, and the file is not at fault */
	assert(machine_upload_put(&up, &m, up_dir, 0, file, 44) == -EINVAL);
	assert(access(up_tmp, F_OK) != 0);
	assert(machine_upload_abort(&up, &m, up_dir) == 0);	/* nothing staged is the answer the page wanted anyway */
	assert(machine_upload_abort(&up, &m, up_dir) == 0);

	/* one staging at a time: the second begin is refused and leaves the first alone */
	assert(machine_upload_begin(&up, &m, up_dir) == 0);
	assert(machine_upload_begin(&up, &m, up_dir) == -EBUSY);
	assert(machine_upload_put(&up, &m, up_dir, 0, file, 44) == 0);
	assert(up.staged == 44);
	assert(machine_upload_abort(&up, &m, up_dir) == 0);
	assert(access(up_tmp, F_OK) != 0);			/* an abandoned upload leaves no tmp for the next one */

	/* a gap and a repeat both mean the page has lost its place: the staging and the tmp go */
	assert(machine_upload_begin(&up, &m, up_dir) == 0);
	assert(machine_upload_put(&up, &m, up_dir, 0, file, 44) == 0);
	assert(machine_upload_put(&up, &m, up_dir, 50, file + 50, 10) == -EPROTO);
	assert(!up.file && up.staged == 0 && access(up_tmp, F_OK) != 0);
	assert(machine_upload_begin(&up, &m, up_dir) == 0);
	assert(machine_upload_put(&up, &m, up_dir, 0, file, 44) == 0);
	assert(machine_upload_put(&up, &m, up_dir, 0, file, 44) == -EPROTO);
	assert(!up.file && access(up_tmp, F_OK) != 0);

	/* an upload of nothing is refused as empty rather than stored as a greeting of silence */
	assert(machine_upload_begin(&up, &m, up_dir) == 0);
	assert(machine_upload_commit(&up, &m, up_dir, 0, NULL) == -ENODATA);
	assert(access(up_tmp, F_OK) != 0);

	/* the cap: the byte over it is refused, and nothing of the staging is left */
	{
		static unsigned char big[65536];
		u32 at = 0;

		assert(machine_upload_begin(&up, &m, up_dir) == 0);
		while (at < MACHINE_MAX_BYTES) {
			assert(machine_upload_put(&up, &m, up_dir, at, big, sizeof(big)) == 0);
			at += sizeof(big);
		}
		assert(machine_upload_put(&up, &m, up_dir, at, big, 1) == -EFBIG);
		assert(!up.file && up.staged == 0 && access(up_tmp, F_OK) != 0);
		assert(!m.greeting_loaded);			/* a refusal never touches the greeting that is loaded */
	}

	/* the piece the page sends whenever a file is bigger than one piece: the cap in plain bytes, in more characters
	 * than that - 16384 is not a multiple of three, so its last quad is padded and carries one byte. A cap measured
	 * in characters instead of bytes sees two bytes that are not there, finds the daemon's own piece size over the
	 * limit it set, and every upload of more than one piece dies at its first write. */
	{
		static unsigned char piece[MACHINE_GREETING_PUT_MAX];
		char enc[MACHINE_GREETING_PUT_MAX * 4 / 3 + 8];
		u32 want, elen = MACHINE_GREETING_PUT_MAX / 3 * 4 + 4;
		int k;

		for (k = 0; k < (int)sizeof(piece); k++)
			piece[k] = (unsigned char)k;
		assert(recording_base64(piece, sizeof(piece), enc) == elen);
		assert(recording_base64_plain_len(enc, elen, &want) == 0 && want == MACHINE_GREETING_PUT_MAX);
		assert(machine_upload_begin(&up, &m, up_dir) == 0);
		assert(machine_upload_put(&up, &m, up_dir, 0, piece, want) == 0);
		assert(up.staged == MACHINE_GREETING_PUT_MAX);
		assert(machine_upload_abort(&up, &m, up_dir) == 0);
	}

	/* a tmp a previous run left behind goes at the start and at the next begin */
	{
		FILE *f = fopen(up_tmp, "w");

		assert(f);
		fprintf(f, "half an upload");
		fclose(f);
		machine_upload_clean(up_dir);
		assert(access(up_tmp, F_OK) != 0);
		assert(machine_upload_begin(&up, &m, up_dir) == 0);
		f = fopen(up_tmp, "w");			/* and one left while a begin follows: replaced, not appended */
		assert(f);
		fclose(f);
		assert(machine_upload_begin(&up, &m, up_dir) == -EBUSY);	/* the open staging still holds the folder */
		assert(machine_upload_abort(&up, &m, up_dir) == 0);
		assert(machine_upload_begin(&up, &m, up_dir) == 0);
		assert(access(up_tmp, F_OK) == 0);
		assert(machine_upload_abort(&up, &m, up_dir) == 0);
	}
	machine_free(&m);
}

static void test_upload_refusals(void)
{
	struct machine m;
	struct machine_upload up;
	unsigned char file[1024];
	static int16_t voice[64];
	char link[192], elsewhere[192];
	struct stat keep;
	u32 len;

	fresh_dir();
	snprintf(up_dir, sizeof(up_dir), "%s/managed3", dir);
	upload_paths();
	machine_init(&m);
	machine_upload_init(&up);

	/* a greeting that is loaded first, so a refused commit has something to leave in place */
	len = wav_bytes(file, 8000, 1, voice, 64);
	assert(staged(&m, &up, file, len, 0, NULL) == 0);
	assert(m.greeting_loaded && m.frames[0] == 64);

	/* not a WAV the machine plays: the rate field is not one the bus runs at */
	len = wav_bytes(file, 8000, 1, voice, 64);
	le32_at(file + 24, 44100);
	assert(staged(&m, &up, file, len, 0, NULL) == -EINVAL);
	assert(m.greeting_loaded && m.frames[0] == 64);		/* the old audio is still what the machine plays */
	assert(stat(up_final, &keep) == 0 && keep.st_size == 44 + 128);	/* and the old file is still the one on disk */

	/* a file whose header promises more than the pieces carried */
	len = wav_bytes(file, 8000, 1, voice, 64);
	le32_at(file + 40, 64 * 2 + 400);
	assert(staged(&m, &up, file, len, 0, NULL) == -EBADMSG);
	assert(m.greeting_loaded && m.frames[0] == 64);
	/* a header with no audio behind it */
	len = wav_bytes(file, 8000, 1, voice, 0);
	assert(staged(&m, &up, file, len, 0, NULL) == -ENODATA);
	assert(m.greeting_loaded && m.frames[0] == 64);

	/* a commit waits for the line: the greeting under the fill is not freed, and the staging stays for the retry */
	assert(machine_upload_begin(&up, &m, up_dir) == 0);
	len = wav_bytes(file, 8000, 1, voice, 64);
	assert(machine_upload_put(&up, &m, up_dir, 0, file, len) == 0);
	m.playing = 1;
	assert(machine_upload_commit(&up, &m, up_dir, 0, NULL) == -EBUSY);
	m.playing = 0;
	assert(up.file && up.staged == len);				/* nothing was thrown away: the page retries */
	assert(machine_upload_commit(&up, &m, up_dir, 0, NULL) == 0);
	assert(m.greeting_loaded && m.frames[0] == 64 && access(up_tmp, F_OK) != 0);

	/* the greeting's name as a link is refused for writing, before a piece is even staged */
	snprintf(elsewhere, sizeof(elsewhere), "%s/elsewhere.wav", dir);
	{
		FILE *f = fopen(elsewhere, "w");

		assert(f);
		fprintf(f, "not the greeting");
		fclose(f);
	}
	assert(remove(up_final) == 0);
	snprintf(link, sizeof(link), "%s", up_final);
	assert(symlink(elsewhere, link) == 0);
	assert(machine_upload_begin(&up, &m, up_dir) == -ELOOP);
	assert(!up.file && access(up_tmp, F_OK) != 0);
	/* and one that appears while the pieces are coming in is caught before the rename */
	assert(unlink(link) == 0);
	assert(machine_upload_begin(&up, &m, up_dir) == 0);
	len = wav_bytes(file, 8000, 1, voice, 64);
	assert(machine_upload_put(&up, &m, up_dir, 0, file, len) == 0);
	(void)unlink(up_final);			/* nothing is in the greeting's place now: a link appears there mid-upload */
	assert(symlink(elsewhere, up_final) == 0);
	assert(machine_upload_commit(&up, &m, up_dir, 0, NULL) == -ELOOP);
	assert(access(up_tmp, F_OK) != 0);				/* the refused upload leaves no tmp */
	assert(access(elsewhere, F_OK) == 0);				/* and nothing was written through the link */
	{
		char held[32];
		size_t got;
		FILE *f = fopen(elsewhere, "r");

		assert(f);
		got = fread(held, 1, sizeof(held) - 1, f);
		held[got] = 0;
		fclose(f);
		assert(!strcmp(held, "not the greeting"));		/* the linked file is exactly what it was */
	}
	machine_free(&m);
}

/* a fresh recording still has the silence of being started by hand and no tone; the commit fixes both */
static void test_upload_polish(void)
{
	struct machine m;
	struct machine_upload up;
	static unsigned char file[65536];
	static int16_t in[3200], tone[8192];
	unsigned int ms = 0;
	u32 lead = 800, speech = 1600, start = 0, end = 0, want, len;
	int k;

	fresh_dir();
	snprintf(up_dir, sizeof(up_dir), "%s/managed4", dir);
	upload_paths();
	machine_init(&m);
	machine_upload_init(&up);

	for (k = 0; k < 3200; k++)
		in[k] = 0;
	for (k = 0; k < (int)speech; k++)
		in[lead + k] = (int16_t)(4000 + (k % 7) * 900);
	len = wav_bytes(file, 8000, 1, in, 3200);

	/* an upload as it stands keeps every sample the author sent, silence and all */
	assert(staged(&m, &up, file, len, 0, &ms) == 0);
	assert(m.frames[0] == 3200);

	/* the same audio as a recording: the silent lead and tail go, a breath of quiet and the tone are added, and the
	 * file the machine holds begins where the voice begins and ends with the tone */
	len = wav_bytes(file, 8000, 1, in, 3200);
	assert(staged(&m, &up, file, len, 1, &ms) == 0);
	greeting_trim(in, 3200, 8000, &start, &end);
	assert(start > 0 && end < 3200);				/* both ends were trimmed */
	assert(greeting_gap_len(8000) == 4000);				/* the quiet between them is a real length, not a word */
	want = end - start + greeting_polish_tail_len(8000);
	assert(m.frames[0] == want);
	assert(ms == (unsigned int)((long long)want * 1000 / 8000));
	assert(!memcmp(m.greeting[0], in + start, (end - start) * sizeof(*in)));	/* the voice, untouched */
	for (k = end - start; k < end - start + greeting_gap_len(8000); k++)
		assert(m.greeting[0][k] == 0);				/* the word ends, and only then does the tone start */
	greeting_tone_write(tone, greeting_tone_len(8000), 8000);
	assert(!memcmp(m.greeting[0] + (end - start) + greeting_gap_len(8000), tone, greeting_tone_len(8000) * sizeof(*tone)));
	assert(m.greeting[0][want - 1] == tone[greeting_tone_len(8000) - 1]);
	assert(machine_greeting_load(&m, up_final, NULL) == 0 && m.frames[0] == want);	/* the file on disk is the same audio */

	/* a recording of nothing but silence is refused as empty: a greeting of silence would only waste the caller's time */
	for (k = 0; k < 3200; k++)
		in[k] = 0;
	len = wav_bytes(file, 8000, 1, in, 3200);
	assert(staged(&m, &up, file, len, 1, &ms) == -ENODATA);
	assert(m.greeting_loaded && m.frames[0] == want);		/* the previous greeting is still the one that plays */
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
	test_upload_happy();
	test_upload_protocol();
	test_upload_refusals();
	test_upload_polish();
	test_message();
	test_feed_after_abort();
	printf("machine: all tests passed\n");
	return 0;
}
