// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "machine.h"

#define READ_CHUNK	1024		/* frames read at a time while the greeting is converted */

static int le16(const unsigned char *p)
{
	return (int)(int16_t)((u16)p[0] | ((u16)p[1] << 8));
}

/* every loaded greeting goes through here: the machine is never half loaded */
void machine_greeting_drop(struct machine *m)
{
	free(m->greeting[0]);
	free(m->greeting[1]);
	m->greeting[0] = m->greeting[1] = NULL;
	m->frames[0] = m->frames[1] = 0;
	m->enabled = 0;
	m->playing = 0;
	m->ring_frames = 0;
	m->pos = 0;
}

void machine_init(struct machine *m)
{
	memset(m, 0, sizeof(*m));
	m->slot = 0;
	m->rec_rate = 8000;
}

void machine_free(struct machine *m)
{
	machine_greeting_drop(m);
	machine_message_abort(m);
}

/* the greeting file as mono samples at its own rate: 0, or -errno (missing, unreadable, not our WAV, truncated) */
static int greeting_read(const char *path, int16_t **out, u32 *rate, u32 *nframes)
{
	unsigned char hdr[44];
	unsigned char buf[READ_CHUNK * 4];	/* 1024 frames of stereo */
	struct stat st;
	FILE *f;
	int16_t *mono = NULL;
	u32 channels, data, frame_bytes, done = 0;
	int err = 0;

	f = fopen(path, "r");
	if (!f)
		return -errno;
	if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode)) {
		err = -EINVAL;
		goto out;
	}
	if (st.st_size < 44) {
		err = -EINVAL;
		goto out;
	}
	if ((unsigned long long)st.st_size > MACHINE_MAX_BYTES) {
		err = -EFBIG;
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
		err = -EINVAL;
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
			err = -EIO;
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
	int err = greeting_read(path, &mono, &rate, &n);

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
		if (log)
			fprintf(log, "the greeting %s cannot be used (%s): the answering machine stays off; record a 16-bit PCM WAV at 8 or 16 kHz, or point awvoice.main.machine_greeting at one\n",
				path, strerror(-err));
		return err;
	}
	machine_greeting_drop(m);
	m->greeting[0] = lo;
	m->greeting[1] = hi;
	m->frames[0] = nlo;
	m->frames[1] = nhi;
	m->enabled = 1;
	return 0;
}

int machine_start(struct machine *m, u32 call_rate)
{
	const int slot = call_rate == 16000 ? 1 : 0;

	m->playing = 0;
	m->ring_frames = 0;
	m->pos = 0;
	if (!m->enabled || !m->greeting[slot] || !m->frames[slot])
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
