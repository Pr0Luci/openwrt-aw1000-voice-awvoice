// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "greeting_audio.h"
#include "machine.h"

#define READ_CHUNK	1024		/* frames read at a time while the greeting is converted */

/* the two names beside one folder: the greeting the machine plays and the tmp an upload stages into */
static int greeting_paths(const char *dir, char *final_path, size_t final_cap, char *tmp_path, size_t tmp_cap)
{
	if (!dir || !*dir)
		return -EINVAL;
	if (snprintf(final_path, final_cap, "%s/%s", dir, MACHINE_GREETING_NAME) >= (int)final_cap ||
	    snprintf(tmp_path, tmp_cap, "%s/%s", dir, MACHINE_GREETING_TMP) >= (int)tmp_cap)
		return -ENAMETOOLONG;
	return 0;
}

static int le16(const unsigned char *p)
{
	return (int)(int16_t)((u16)p[0] | ((u16)p[1] << 8));
}

/* every loaded greeting goes through here: the machine is never half loaded. Afterwards the machine holds nothing and
 * the reason it can state is the plain one - there is no greeting - until a read finds a better answer */
void machine_greeting_drop(struct machine *m)
{
	free(m->greeting[0]);
	free(m->greeting[1]);
	m->greeting[0] = m->greeting[1] = NULL;
	m->frames[0] = m->frames[1] = 0;
	m->greeting_loaded = 0;
	m->greeting_ms = 0;
	m->greeting_err = -ENOENT;
	m->playing = 0;
	m->ring_frames = 0;
	m->pos = 0;
}

void machine_init(struct machine *m)
{
	memset(m, 0, sizeof(*m));
	m->slot = 0;
	m->rec_rate = 8000;
	m->greeting_err = -ENOENT;	/* nothing has been read yet: the honest word is "missing" */
}

void machine_free(struct machine *m)
{
	machine_greeting_drop(m);
	machine_message_abort(m);
}

/*
 * The greeting file as mono samples at its own rate: 0, or -errno. Each failure keeps its own errno because the page
 * says one sentence per case: a file that is not there, a file that cannot be opened, something that is not a regular
 * file, a WAV that is not one the machine plays, a file cut off short of what its own header promised, a file over the
 * cap, an
 * empty data section and a run out of memory. Only the format ones are -EINVAL: a header that does not describe 16-bit
 * PCM at one of the bus's rates is a different problem from a file that stops halfway, and the user is told something
 * different for each.
 *
 * The type is looked at before anything is opened and the open is O_NONBLOCK. A fifo with no writer would otherwise
 * hold the daemon's one thread inside the open for as long as nobody wrote to it, and the 5 ms clock cannot wait on a
 * file - so a thing that is not a regular file is refused by its stat, and a file made into one afterwards is opened
 * without blocking.
 */
static int greeting_read_at(const char *path, int16_t **out, u32 *rate, u32 *nframes)
{
	unsigned char hdr[44];
	unsigned char buf[READ_CHUNK * 4];	/* 1024 frames of stereo */
	struct stat st;
	FILE *f;
	int16_t *mono = NULL;
	int fd;
	u32 channels, data, frame_bytes, done = 0;
	int err = 0;

	if (stat(path, &st))
		return -errno;
	if (S_ISDIR(st.st_mode))
		return -EISDIR;
	if (!S_ISREG(st.st_mode))
		return -EOPNOTSUPP;	/* a fifo or a device is not a greeting, whatever it holds */
	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return -errno;
	f = fdopen(fd, "r");
	if (!f) {
		err = -errno;
		close(fd);
		return err;
	}
	if (fstat(fileno(f), &st)) {
		err = -errno;
		goto out;
	}
	if (!S_ISREG(st.st_mode)) {		/* it changed between the two looks: the open above already refused to block */
		err = S_ISDIR(st.st_mode) ? -EISDIR : -EOPNOTSUPP;
		goto out;
	}
	if ((unsigned long long)st.st_size > MACHINE_MAX_BYTES) {
		err = -EFBIG;
		goto out;
	}
	if (st.st_size < 44) {
		err = -EBADMSG;		/* not even a whole header: a file that was cut off */
		goto out;
	}
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || recording_wav_probe(hdr, rate, &channels, &data)) {
		err = -EINVAL;
		goto out;
	}
	/* the header's size is what a finished file holds: a shorter one is a copy that was cut off, not a greeting */
	if ((unsigned long long)data > (unsigned long long)st.st_size - 44) {
		err = -EBADMSG;
		goto out;
	}
	frame_bytes = channels * 2;
	*nframes = data / frame_bytes;		/* the probe ensured the multiple */
	if (!*nframes) {
		err = -ENODATA;		/* a valid header over no audio at all: silence is not a greeting */
		goto out;
	}
	mono = malloc((size_t)*nframes * sizeof(*mono));
	if (!mono) {
		err = -ENOMEM;
		goto out;
	}
	while (done < *nframes) {
		u32 n = *nframes - done > READ_CHUNK ? READ_CHUNK : *nframes - done;
		u32 k;

		if (fread(buf, frame_bytes, n, f) != n) {
			err = -EIO;		/* the file promised more than it holds and the read came up short */
			goto out;
		}
		for (k = 0; k < n; k++) {
			if (channels == 1)
				mono[done + k] = (int16_t)le16(buf + 2 * k);
			else
				mono[done + k] = (int16_t)((le16(buf + 4 * k) + le16(buf + 4 * k + 2)) / 2);
		}
		done += n;
	}
out:
	fclose(f);
	if (err) {
		free(mono);
		return err;
	}
	*out = mono;
	return 0;
}

/* 8000 -> 16000: linear interpolation, which is exactly what a factor of two needs; the last sample holds at the edge */
static int16_t *greeting_up(const int16_t *in, u32 n, u32 *out_n)
{
	int16_t *out = malloc((size_t)n * 2 * sizeof(*out));
	u32 k;

	if (!out)
		return NULL;
	for (k = 0; k < n; k++) {
		out[2 * k] = in[k];
		out[2 * k + 1] = (int16_t)(((int)in[k] + in[k + 1 < n ? k + 1 : n - 1]) / 2);
	}
	*out_n = n * 2;
	return out;
}

/* 16000 -> 8000: the [1 2 1]/4 low pass at every second sample; the edges hold */
static int16_t *greeting_down(const int16_t *in, u32 n, u32 *out_n)
{
	int16_t *out = malloc((size_t)(n / 2) * sizeof(*out));
	u32 k;

	if (!out)
		return NULL;
	for (k = 0; k < n / 2; k++) {
		int a = in[k ? 2 * k - 1 : 0];
		int b = in[2 * k];
		int c = in[2 * k + 1 < n ? 2 * k + 1 : n - 1];

		out[k] = (int16_t)((a + 2 * b + c) / 4);
	}
	*out_n = n / 2;
	return out;
}

int machine_greeting_load(struct machine *m, const char *path, FILE *log)
{
	int16_t *mono = NULL, *lo = NULL, *hi = NULL;
	u32 rate = 0, n = 0, nlo = 0, nhi = 0;
	int err = greeting_read_at(path, &mono, &rate, &n);

	if (!err) {
		if (rate == 8000) {
			lo = mono;
			nlo = n;
			hi = greeting_up(mono, n, &nhi);
			if (!hi)
				err = -ENOMEM;
		} else {
			hi = mono;
			nhi = n;
			lo = greeting_down(mono, n, &nlo);
			if (!lo)
				err = -ENOMEM;
		}
	}
	if (err) {
		if (lo != mono)
			free(lo);
		if (hi != mono)
			free(hi);
		free(mono);
		machine_greeting_drop(m);
		m->greeting_err = err;	/* the read's own finding: the health reply says this, not a second probe */
		if (log)
			fprintf(log, "the greeting %s cannot be used (%s): the answering machine stays off; record one or upload a 16-bit PCM WAV at 8 or 16 kHz on the Phone settings page\n",
				path, strerror(-err));
		return err;
	}
	machine_greeting_drop(m);
	m->greeting[0] = lo;
	m->greeting[1] = hi;
	m->frames[0] = nlo;
	m->frames[1] = nhi;
	m->greeting_err = 0;
	m->greeting_ms = (unsigned int)((long long)nlo * 1000 / 8000);	/* both slots hold the same moment in time */
	m->greeting_loaded = 1;
	return 0;
}

int machine_start(struct machine *m, u32 call_rate)
{
	const int slot = call_rate == 16000 ? 1 : 0;

	m->playing = 0;
	m->ring_frames = 0;
	m->pos = 0;
	if (!m->greeting_loaded || !m->greeting[slot] || !m->frames[slot])
		return -ENOENT;
	m->slot = slot;
	m->playing = 1;
	m->ring_frames = m->frames[slot] / (slot ? 2u : 1u);	/* the wideband play frame carries two samples */
	return 0;
}

void machine_stop(struct machine *m)
{
	m->playing = 0;
	m->ring_frames = 0;
	m->pos = 0;
}

void machine_fill(struct machine *m, int16_t *out, u32 frames)
{
	const unsigned int width = m->slot ? 2 : 1;
	u32 f, w;

	for (f = 0; f < frames; f++) {
		for (w = 0; w < width; w++) {
			int16_t v = 0;

			if (m->playing) {
				v = m->greeting[m->slot][m->pos++];
				if (m->pos >= m->frames[m->slot])
					m->playing = 0;	/* the rest of the call is silence on the caller's side */
			}
			*out++ = v;
		}
	}
}

int machine_message_open(struct machine *m, unsigned int call_id, u32 rate, const char *dir, const char *final_name)
{
	int err;

	if (m->recording)
		return -EBUSY;
	err = recorder_start(&m->rec, dir, final_name, rate, 1);
	if (err)
		return err;
	m->recording = 1;
	m->saved = 0;
	m->rec_rate = rate;
	m->call_id = call_id;
	m->rec_frames = 0;
	snprintf(m->name, sizeof(m->name), "%s.wav", final_name);
	return 0;
}

int machine_message_feed(struct machine *m, const int16_t *samples, u32 count)
{
	int err;

	if (!m->recording)
		return 0;
	err = recorder_feed(&m->rec, (const u16 *)samples, count);
	if (err) {
		/* recorder_feed discarded the tmp; the name goes with it, so the call log never names a file that is not there */
		m->recording = 0;
		m->name[0] = 0;
		m->rec_frames = 0;
		return err;
	}
	m->rec_frames += count;
	return 0;
}

long long machine_message_finish(struct machine *m)
{
	long long bytes;

	if (!m->recording)
		return 0;
	if (!m->rec_frames) {
		/* the caller said nothing before the end: a file of header only is no message */
		machine_message_abort(m);
		return 0;
	}
	m->recording = 0;
	bytes = recorder_finish(&m->rec);
	if (bytes < 0) {
		m->name[0] = 0;
		m->saved = 0;
		m->rec_frames = 0;
		return bytes;
	}
	m->saved = 1;
	m->rec_frames = 0;
	return bytes;
}

void machine_message_abort(struct machine *m)
{
	if (m->recording)
		recorder_abort(&m->rec);
	m->recording = 0;
	m->saved = 0;
	m->rec_frames = 0;
	m->name[0] = 0;
}

/* ---- the staged greeting: pieces in the folder's tmp, the greeting replaced only by a commit that proves them ---- */

void machine_upload_init(struct machine_upload *up)
{
	memset(up, 0, sizeof(*up));
}

/* the greeting's own name must not be a link: a write through one lands wherever the link points. Absent is fine -
 * the commit is what creates it - and the daemon reads through a link that is already there, since that file is what
 * the machine plays. */
static int target_is_link(const char *path)
{
	struct stat st;

	if (lstat(path, &st))
		return errno == ENOENT ? 0 : -errno;
	return S_ISLNK(st.st_mode) ? -ELOOP : 0;
}

/* the tmp goes and the staging ends with it: no half upload is ever left in the folder for the next one to trip over */
static void upload_discard(struct machine_upload *up, const char *tmp_path)
{
	if (up->file) {
		fclose(up->file);
		up->file = NULL;
	}
	up->staged = 0;
	unlink(tmp_path);
}

/* the tmp is opened without blocking and at 0600: a thing that is not a regular file at that name is refused by the
 * open (ENXIO for a fifo with no reader, EISDIR for the folder) instead of holding the daemon's one thread there, and
 * the household's own voice never becomes world readable because someone asked for a file */
static FILE *stage_open(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK, 0600);
	FILE *f;

	if (fd < 0)
		return NULL;
	f = fdopen(fd, "w");
	if (!f) {
		int err = errno;

		close(fd);
		errno = err;
		return NULL;
	}
	fchmod(fd, 0600);		/* the mode the open asked for, whatever the process's umask said */
	return f;
}

int machine_upload_begin(struct machine_upload *up, struct machine *m, const char *dir)
{
	char final_path[PATH_MAX], tmp_path[PATH_MAX];
	FILE *f;
	int err;

	(void)m;			/* a call is not in the way: the tmp is not the greeting the machine plays */
	err = greeting_paths(dir, final_path, sizeof(final_path), tmp_path, sizeof(tmp_path));
	if (err)
		return err;
	if (up->file)
		return -EBUSY;			/* one upload at a time; the page aborts the open one before starting again */
	err = target_is_link(final_path);
	if (err)
		return err;
	if (mkdir(dir, 0700) && errno != EEXIST)
		return -errno;			/* the folder is the daemon's own, like the call log's: root only */
	unlink(tmp_path);			/* a stale tmp from a run that died, or from an upload that was dropped */
	f = stage_open(tmp_path);
	if (!f)
		return -errno;
	up->file = f;
	up->staged = 0;
	up->err = 0;
	return 0;
}

int machine_upload_put(struct machine_upload *up, struct machine *m, const char *dir, u32 offset,
	const unsigned char *bytes, u32 len)
{
	char final_path[PATH_MAX], tmp_path[PATH_MAX];
	int err;

	(void)m;
	err = greeting_paths(dir, final_path, sizeof(final_path), tmp_path, sizeof(tmp_path));
	if (err)
		return err;
	if (!up->file)
		return -EINVAL;			/* a piece with no upload open: the calls are out of order, the file is not at fault */
	if (offset != up->staged) {
		/* a gap or a repeat: the page has lost its place, and stitching from two different files is not a greeting */
		upload_discard(up, tmp_path);
		return -EPROTO;
	}
	if ((unsigned long long)up->staged + len > MACHINE_MAX_BYTES) {
		upload_discard(up, tmp_path);
		return -EFBIG;
	}
	if (len && fwrite(bytes, 1, len, up->file) != len) {
		err = errno ? -errno : -EIO;
		upload_discard(up, tmp_path);
		return err;
	}
	up->staged += len;
	return 0;
}

int machine_upload_abort(struct machine_upload *up, struct machine *m, const char *dir)
{
	char final_path[PATH_MAX], tmp_path[PATH_MAX];
	int err = greeting_paths(dir, final_path, sizeof(final_path), tmp_path, sizeof(tmp_path));

	(void)m;
	if (err)
		return err;
	upload_discard(up, tmp_path);		/* nothing staged is a success: the page is asking for the same result either way */
	return 0;
}

/* the staged tmp rewritten as one canonical mono file: the trimmed audio and the tone, header and all. Returns the
 * samples written, or -errno; a failure leaves the tmp gone, so the commit's refusal cannot leave a half file. */
static int upload_write_polished(const char *tmp_path, const int16_t *mono, u32 n, u32 rate)
{
	unsigned char header[44];
	int16_t *out;
	u32 tail = greeting_polish_tail_len(rate);
	FILE *f;
	long kept;
	int err = 0;

	out = malloc((size_t)(n + tail) * sizeof(*out));
	if (!out)
		return -ENOMEM;
	kept = greeting_audio_polish(mono, n, rate, out, n + tail);
	if (kept < 0) {
		free(out);
		return (int)kept;
	}
	if (recording_wav_header(header, rate, 1, (u32)kept * 2) != 44) {
		free(out);
		return -EINVAL;
	}
	f = stage_open(tmp_path);		/* the staged bytes are replaced whole: an append would mix two audios */
	if (!f) {
		free(out);
		return -errno;
	}
	if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
	    fwrite(out, sizeof(*out), (size_t)kept, f) != (size_t)kept)
		err = errno ? -errno : -EIO;
	if (fflush(f) || ferror(f) || fsync(fileno(f)))
		err = err ? err : -EIO;
	if (fclose(f) && !err)
		err = -EIO;
	free(out);
	if (err)
		unlink(tmp_path);
	return err;
}

int machine_upload_commit(struct machine_upload *up, struct machine *m, const char *dir, int polish,
	unsigned int *ms_out)
{
	char final_path[PATH_MAX], tmp_path[PATH_MAX];
	struct machine scratch;
	int16_t *mono = NULL;
	u32 rate = 0, n = 0;
	int err;

	if (ms_out)
		*ms_out = 0;
	err = greeting_paths(dir, final_path, sizeof(final_path), tmp_path, sizeof(tmp_path));
	if (err)
		return err;
	if (m->playing || m->recording)
		return -EBUSY;		/* the old greeting is under the fill or the message is open: the swap would free it in flight */
	if (!up->file)
		return -EINVAL;			/* nothing was staged: there is no greeting to prove */
	if (fflush(up->file) || ferror(up->file) || fsync(fileno(up->file))) {
		err = errno ? -errno : -EIO;
		upload_discard(up, tmp_path);
		return err;
	}
	fclose(up->file);
	up->file = NULL;
	if (!up->staged) {
		up->staged = 0;
		unlink(tmp_path);
		return -ENODATA;			/* an upload of nothing is not a greeting: say so rather than store silence */
	}
	err = target_is_link(final_path);		/* a link can have appeared while the pieces came in */
	if (err) {
		up->staged = 0;
		unlink(tmp_path);
		return err;
	}
	if (polish) {
		/* the recording is read at its own rate, trimmed and given the tone, and written back as the one shape the
		 * machine plays without a second thought */
		err = greeting_read_at(tmp_path, &mono, &rate, &n);
		if (!err)
			err = upload_write_polished(tmp_path, mono, n, rate);
		free(mono);
		if (err) {
			up->staged = 0;
			unlink(tmp_path);
			return err;
		}
	}
	/* prove it as the machine would: the same read, into a scratch machine, before the old greeting is touched */
	machine_init(&scratch);
	err = machine_greeting_load(&scratch, tmp_path, NULL);
	if (err) {
		machine_free(&scratch);
		unlink(tmp_path);
		up->staged = 0;
		return err;
	}
	if (rename(tmp_path, final_path)) {
		err = -errno;
		machine_free(&scratch);
		unlink(tmp_path);
		up->staged = 0;
		return err;			/* the old file stays and so does the old greeting: nothing half happened */
	}
	up->staged = 0;
	/* the scratch machine holds the audio the file on disk now describes: hand it to the live machine as one swap */
	machine_greeting_drop(m);
	m->greeting[0] = scratch.greeting[0];
	m->greeting[1] = scratch.greeting[1];
	m->frames[0] = scratch.frames[0];
	m->frames[1] = scratch.frames[1];
	m->greeting_loaded = scratch.greeting_loaded;
	m->greeting_ms = scratch.greeting_ms;
	m->greeting_err = 0;
	if (ms_out)
		*ms_out = m->greeting_ms;
	return 0;
}

void machine_upload_clean(const char *dir)
{
	char final_path[PATH_MAX], tmp_path[PATH_MAX];

	if (!greeting_paths(dir, final_path, sizeof(final_path), tmp_path, sizeof(tmp_path)))
		unlink(tmp_path);		/* a previous run died between two pieces: nothing of it is a greeting */
}
