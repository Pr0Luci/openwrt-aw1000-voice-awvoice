// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The tap's one drain and its three sinks, on the host: the recorder's interleave, the answering machine's message
 * and the front end's cut, fed from the same slots in one pass: make -f Makefile.host test */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "front_end.h"
#include "machine.h"

#define SLOTS	20		/* the slots of one read: fewer than a 20 ms frame, so the cut holds them */

static char dir[64];
static int frames;
static struct fe_frame frame[8];

static void on_capture(void *ctx, const struct fe_frame *fr)
{
	(void)ctx;
	assert(frames < (int)(sizeof(frame) / sizeof(frame[0])));
	frame[frames++] = *fr;
}

static void fresh_dir(void)
{
	snprintf(dir, sizeof(dir), "/tmp/aw-fanout-XXXXXX");
	assert(mkdtemp(dir));
}

/* the sink the cut delivers to; a NULL sink detaches the front end */
static void attach(struct fe *f)
{
	struct fe_sink s;

	memset(&s, 0, sizeof(s));
	s.capture = on_capture;
	fe_set_sink(f, &s);
}

/* every field of a slot carries its own value: a swapped channel or a lost second sample shows */
static void fill_slots(struct fe_slot *slots, u16 base)
{
	u32 k;

	for (k = 0; k < SLOTS; k++) {
		slots[k].modem = (u16)(base + k);
		slots[k].line = (u16)(base + 1000 + k);
		slots[k].modem2 = (u16)(base + 2000 + k);
		slots[k].line2 = (u16)(base + 3000 + k);
	}
}

/* the recording's file, sample by sample: channel 1 is the far end, channel 2 the handset, and at 16 kHz each
 * slot adds its second pair after them */
static u32 want_stereo(const struct fe_slot *slots, u32 rate, int wideband, u16 *out)
{
	u32 k, o = 0;

	for (k = 0; k < SLOTS; k++) {
		out[o++] = slots[k].modem;
		out[o++] = slots[k].line;
		if (wideband && rate == 16000) {
			out[o++] = slots[k].modem2;
			out[o++] = slots[k].line2;
		}
	}
	return o;
}

/* the message's samples: the caller's side, two a slot at 16 kHz */
static u32 want_mono(const struct fe_slot *slots, u32 rate, int wideband, int16_t *out)
{
	u32 k, o = 0;

	for (k = 0; k < SLOTS; k++) {
		out[o++] = (int16_t)slots[k].modem;
		if (wideband && rate == 16000)
			out[o++] = (int16_t)slots[k].modem2;
	}
	return o;
}

/* a finished file's data, with the rate and the channel count its header claims */
static long wav_read(const char *path, unsigned char *buf, size_t cap, u32 *rate, u32 *channels)
{
	unsigned char hdr[44];
	u32 data = 0;
	FILE *f = fopen(path, "rb");
	long n;

	assert(f && fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
	assert(recording_wav_probe(hdr, rate, channels, &data) == 0);
	n = (long)fread(buf, 1, cap, f);
	assert(fclose(f) == 0);
	assert(n == (long)data);
	return n;
}

/* the file's little-endian samples are exactly `want` */
static void assert_samples(const unsigned char *data, const u16 *want, u32 count)
{
	u32 k;

	for (k = 0; k < count; k++) {
		assert(data[2 * k] == (want[k] & 0xff));
		assert(data[2 * k + 1] == (want[k] >> 8));
	}
}

/* the three sinks armed on one tap, as the daemon keeps them */
struct fan {
	struct fe *fe;
	struct fe_owners *own;
	struct recorder rec;
	struct machine mach;
	int rec_armed, msg_armed;
	u32 rec_rate;
	u32 msg_rate;
	u32 generation;
	int guards;			/* how many times the generation guard ran */
	int files;			/* recording files started: the first, then one per rate change */
};

static struct fan fan_open(void)
{
	struct fan f;

	memset(&f, 0, sizeof(f));
	machine_init(&f.mach);
	assert(fe_open(&f.fe, NULL, 0) == 0);
	f.own = fe_owners(f.fe);
	attach(f.fe);
	return f;
}

static void fan_close(struct fan *f)
{
	machine_free(&f->mach);
	fe_close(f->fe);
}

static void rec_start(struct fan *f, u32 rate)
{
	char name[32];

	snprintf(name, sizeof(name), "call-%d", ++f->files);
	assert(recorder_start(&f->rec, dir, name, rate, 2) == 0);
	f->rec_rate = rate;
	f->rec_armed = 1;
	fe_sink_set(f->own, FE_SINK_RECORDER, 1);
}

static void msg_start(struct fan *f, const char *name, u32 rate)
{
	assert(machine_message_open(&f->mach, 7, rate, dir, name) == 0);
	f->msg_rate = rate;
	f->msg_armed = 1;
	fe_sink_set(f->own, FE_SINK_MACHINE, 1);
}

static void fe_start(struct fan *f, u32 rate, int wideband)
{
	fe_sink_set(f->own, FE_SINK_FRONT_END, 1);
	assert(fe_capture_reset(f->fe, rate, wideband) == 0);
}

/* One pass over one tap read, as main.c runs it: the generation judged once, then every armed sink on the same
 * frames, the recorder, the machine and the front end in that order. */
static void fan_drain(struct fan *f, const struct fe_slot *frames_in, unsigned int got, u32 generation, int wideband)
{
	struct fe_slot slots[256];
	uint16_t samples[1024];
	int16_t mono[512];
	unsigned int k, n;
	char name[32];

	if (generation != f->generation) {
		f->generation = generation;
		f->guards++;
		if (f->rec_armed) {
			assert(recorder_finish(&f->rec) >= 0);
			f->rec_rate = wideband ? 16000 : 8000;
			snprintf(name, sizeof(name), "call-%d", ++f->files);
			assert(recorder_start(&f->rec, dir, name, f->rec_rate, 2) == 0);
		}
		if (f->msg_armed) {
			/* the message is one file: what was written is kept and the message stops here */
			f->msg_armed = 0;
			assert(machine_message_finish(&f->mach) >= 0);
		}
		assert(fe_capture_reset(f->fe, wideband ? 16000u : 8000u, wideband) == 0);
	}
	if (f->rec_armed) {
		for (k = 0, n = 0; k < got; k++) {
			samples[n++] = frames_in[k].modem;
			samples[n++] = frames_in[k].line;
			if (wideband && f->rec_rate == 16000) {
				samples[n++] = frames_in[k].modem2;
				samples[n++] = frames_in[k].line2;
			}
		}
		assert(recorder_feed(&f->rec, samples, n) == 0);
	}
	if (f->msg_armed && f->mach.recording) {
		for (k = 0, n = 0; k < got; k++) {
			mono[n++] = (int16_t)frames_in[k].modem;
			if (wideband && f->msg_rate == 16000)
				mono[n++] = (int16_t)frames_in[k].modem2;
		}
		assert(machine_message_feed(&f->mach, mono, n) == 0);
	}
	if (f->own->sink & FE_SINK_FRONT_END) {
		for (k = 0; k < got; k++) {
			slots[k].modem = frames_in[k].modem;
			slots[k].line = frames_in[k].line;
			slots[k].modem2 = frames_in[k].modem2;
			slots[k].line2 = frames_in[k].line2;
		}
		fe_tap(f->fe, slots, got);
	}
}

/* 20 slots through all three sinks: the files and the frame carry the same slots, at either rate */
static void test_agree(u32 rate, int wideband)
{
	struct fan f;
	struct fe_slot slots[SLOTS];
	u16 want[SLOTS * 4];
	int16_t mono[SLOTS * 2];
	unsigned char data[4096];
	u32 count, got_rate, got_ch, k;
	char path[300];

	fresh_dir();
	f = fan_open();
	frames = 0;
	fill_slots(slots, 100);
	rec_start(&f, rate);
	msg_start(&f, "msg-1", rate);
	fe_start(&f, rate, wideband);
	fan_drain(&f, slots, SLOTS, 0, wideband);
	assert(!f.guards && f.rec_armed && f.msg_armed && !frames);	/* the cut holds the slots of a short read */
	/* the reset sends the part held as one padded frame, the same way the cut delivers one */
	assert(fe_capture_reset(f.fe, rate, wideband) == 0);
	assert(frames == 1);
	assert(frame[0].rate == rate && !frame[0].sequence);
	assert(frame[0].count == (rate == 16000 ? FE_SAMPLES_16K : FE_SAMPLES_8K));
	for (k = 0; k < SLOTS; k++) {
		assert(frame[0].sample[rate == 16000 ? 2 * k : k] == (int16_t)slots[k].modem);
		if (rate == 16000)
			assert(frame[0].sample[2 * k + 1] == (int16_t)slots[k].modem2);
	}
	assert(!frame[0].sample[rate == 16000 ? 2 * SLOTS : SLOTS]);
	assert(!frame[0].sample[frame[0].count - 1]);
	/* the recording: the interleaved pair, and the wideband pair at 16 kHz */
	assert(recorder_finish(&f.rec) > 0);
	snprintf(path, sizeof(path), "%s/call-1.wav", dir);
	count = want_stereo(slots, rate, wideband, want);
	assert(wav_read(path, data, sizeof(data), &got_rate, &got_ch) == (long)count * 2);
	assert(got_rate == rate && got_ch == 2);
	assert_samples(data, want, count);
	/* the message: the caller's side, two samples a slot at 16 kHz */
	assert(machine_message_finish(&f.mach) > 0 && f.mach.saved);
	snprintf(path, sizeof(path), "%s/%s", dir, f.mach.name);
	count = want_mono(slots, rate, wideband, mono);
	assert(wav_read(path, data, sizeof(data), &got_rate, &got_ch) == (long)count * 2);
	assert(got_rate == rate && got_ch == 1);
	assert_samples(data, (const u16 *)mono, count);
	fan_close(&f);
}

/* one generation change, judged once: the recorder keeps its part and goes on in a second file at the new rate,
 * the message is kept and stops, the cut restarts at the new rate, and every slot lands in exactly one place */
static void test_generation(void)
{
	struct fan f;
	struct fe_slot first[SLOTS], second[SLOTS];
	u16 want[SLOTS * 4];
	int16_t mono[SLOTS * 2];
	unsigned char data[4096];
	u32 count, got_rate, got_ch;
	char path[300];

	fresh_dir();
	f = fan_open();
	frames = 0;
	fill_slots(first, 100);
	fill_slots(second, 900);
	rec_start(&f, 8000);
	msg_start(&f, "msg-1", 8000);
	fe_start(&f, 8000, 0);
	fan_drain(&f, first, SLOTS, 0, 0);
	assert(!f.guards && !frames);
	/* the bridge restarts: the rate goes to 16 kHz under the same sinks */
	fan_drain(&f, second, SLOTS, 1, 1);
	assert(f.guards == 1);
	assert(f.rec_armed && f.files == 2 && f.rec_rate == 16000);
	assert(!f.msg_armed && f.mach.saved);
	/* the cut sent the 8 kHz part it held, padded, and restarted at the new rate */
	assert(frames == 1 && frame[0].rate == 8000 && frame[0].count == FE_SAMPLES_8K && !frame[0].sequence);
	assert(frame[0].sample[0] == (int16_t)first[0].modem && frame[0].sample[SLOTS - 1] == (int16_t)first[SLOTS - 1].modem);
	assert(!frame[0].sample[SLOTS]);
	assert(fe_call_view(f.fe)->rate == 16000);
	assert(fe_capture_reset(f.fe, 16000, 1) == 0);
	assert(frames == 2 && frame[1].rate == 16000 && !frame[1].sequence && frame[1].count == FE_SAMPLES_16K);
	assert(frame[1].sample[0] == (int16_t)second[0].modem && frame[1].sample[1] == (int16_t)second[0].modem2);
	assert(!frame[1].sample[2 * SLOTS]);
	/* the first file is the slots before the change, the second the slots after it: none lost, none twice */
	assert(recorder_finish(&f.rec) > 0);
	snprintf(path, sizeof(path), "%s/call-1.wav", dir);
	count = want_stereo(first, 8000, 0, want);
	assert(wav_read(path, data, sizeof(data), &got_rate, &got_ch) == (long)count * 2);
	assert(got_rate == 8000 && got_ch == 2);
	assert_samples(data, want, count);
	snprintf(path, sizeof(path), "%s/call-2.wav", dir);
	count = want_stereo(second, 16000, 1, want);
	assert(wav_read(path, data, sizeof(data), &got_rate, &got_ch) == (long)count * 2);
	assert(got_rate == 16000 && got_ch == 2);
	assert_samples(data, want, count);
	/* the message holds the slots up to the change and nothing after it */
	snprintf(path, sizeof(path), "%s/msg-1.wav", dir);
	count = want_mono(first, 8000, 0, mono);
	assert(wav_read(path, data, sizeof(data), &got_rate, &got_ch) == (long)count * 2);
	assert(got_rate == 8000 && got_ch == 1);
	assert_samples(data, (const u16 *)mono, count);
	fan_close(&f);
}

/* the front end lets go: its frames stop, and the recorder and the message take every slot as before */
static void test_detach(void)
{
	struct fan f;
	struct fe_slot first[SLOTS], second[SLOTS];
	u16 want[2 * SLOTS * 4];
	int16_t mono[2 * SLOTS];
	unsigned char data[4096];
	u32 count, got_rate, got_ch;
	char path[300];

	fresh_dir();
	f = fan_open();
	frames = 0;
	fill_slots(first, 100);
	fill_slots(second, 900);
	rec_start(&f, 8000);
	msg_start(&f, "msg-1", 8000);
	fe_start(&f, 8000, 0);
	fan_drain(&f, first, SLOTS, 0, 0);
	assert(!frames);
	fe_set_sink(f.fe, NULL);		/* the transport goes: the cut drops the part it held */
	fe_sink_set(f.own, FE_SINK_FRONT_END, 0);
	assert(!strcmp(fe_ring_owner_name(f.own), "recorder"));
	fan_drain(&f, second, SLOTS, 0, 0);
	assert(!frames);
	/* both files carry both reads, whole */
	assert(recorder_finish(&f.rec) > 0);
	snprintf(path, sizeof(path), "%s/call-1.wav", dir);
	count = want_stereo(first, 8000, 0, want);
	count += want_stereo(second, 8000, 0, want + count);
	assert(wav_read(path, data, sizeof(data), &got_rate, &got_ch) == (long)count * 2);
	assert(got_rate == 8000 && got_ch == 2);
	assert_samples(data, want, count);
	assert(machine_message_finish(&f.mach) > 0);
	snprintf(path, sizeof(path), "%s/%s", dir, f.mach.name);
	count = want_mono(first, 8000, 0, mono);
	count += want_mono(second, 8000, 0, mono + count);
	assert(wav_read(path, data, sizeof(data), &got_rate, &got_ch) == (long)count * 2);
	assert(got_rate == 8000 && got_ch == 1);
	assert_samples(data, (const u16 *)mono, count);
	fan_close(&f);
}

/* the playback ring's one owner: the greeting, a front-end claim refused while it plays, the release, the
 * front-end's claim, and the word the health key gives at every step */
static void test_ring_ladder(void)
{
	struct fe *f = NULL;
	struct fe_owners *o;

	assert(fe_open(&f, NULL, 0) == 0);
	o = fe_owners(f);
	assert(!strcmp(fe_ring_owner_name(o), "idle"));
	fe_sink_set(o, FE_SINK_RECORDER, 1);
	fe_sink_set(o, FE_SINK_MACHINE, 1);
	assert(!strcmp(fe_ring_owner_name(o), "recorder"));
	assert(fe_play_claim(o, FE_PLAY_GREETING) == 0 && o->play == FE_PLAY_GREETING);
	assert(!strcmp(fe_ring_owner_name(o), "greeting"));
	assert(fe_play_claim(o, FE_PLAY_FRONT_END) == -EBUSY && o->play == FE_PLAY_GREETING);
	assert(!strcmp(fe_ring_owner_name(o), "greeting"));
	fe_play_release(o);
	assert(o->play == FE_PLAY_NONE && !strcmp(fe_ring_owner_name(o), "recorder"));
	assert(fe_play_claim(o, FE_PLAY_FRONT_END) == 0 && !strcmp(fe_ring_owner_name(o), "front-end"));
	/* the greeting wins over an owner that is not it */
	assert(fe_play_claim(o, FE_PLAY_GREETING) == 0 && !strcmp(fe_ring_owner_name(o), "greeting"));
	fe_play_release(o);
	fe_sink_set(o, FE_SINK_RECORDER | FE_SINK_MACHINE | FE_SINK_FRONT_END, 0);
	assert(!fe_tap_needed(o) && !strcmp(fe_ring_owner_name(o), "idle"));
	fe_close(f);
}

int main(void)
{
	test_agree(8000, 0);
	test_agree(16000, 1);
	test_generation();
	test_detach();
	test_ring_ladder();
	printf("fanout: all tests passed\n");
	return 0;
}
