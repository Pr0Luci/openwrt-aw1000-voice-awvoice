/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_MACHINE_H
#define AW_PHONED_MACHINE_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "recorder.h"

/*
 * The answering machine's portable half: the greeting and the message file. The decision to answer is the call
 * core's (call.c); the greeting is played toward the caller through the kernel's playback ring and the message is
 * recorded from the kernel's capture tap, both in main.c. This module holds no device and no clock, so the host
 * tests cover it whole (tests/test_machine.c).
 *
 * The greeting is a canonical RIFF/WAVE file of 16-bit PCM at 8000 or 16000 Hz, mono or stereo (the recorder's own
 * shape; recording_wav_probe reads it). It is loaded once and kept as mono at both of the PCM bus's rates, so the
 * greeting of a narrowband and of a wideband call is the same audio: 8 -> 16 kHz by linear interpolation, 16 -> 8 kHz
 * through a [1 2 1]/4 low pass before every second sample (a greeting is speech; this keeps the telephone band free
 * of the alias a plain decimation would fold into it). A file that is not one of these is refused as a whole: the
 * machine stays off rather than answer with a noise burst.
 *
 * While a machine call is in progress the greeting goes out first and silence follows (the caller must not hear the
 * crossed line or our own ringing), and the message starts only when the ring has played the greeting's last frame
 * (the daemon compares the ring's own frame counter). The words in the greeting's final MACHINE_FILL_MS are not kept:
 * the message begins with the caller's first frame after the greeting is through, not with its echo. Keep feeding: an
 * empty armed ring plays silence but counts an underrun for every frame, which would bury a real fault in the noise of
 * a call.
 *
 * TO-VERIFY on the line: a 16 kHz greeting played toward a wideband caller, and a wideband message stored at 16 kHz
 * (the host tests prove the resampler and the file, not the bridge's two halves). The first call after a daemon
 * restart is silent on this modem: whether a machine answer on that call records only silence is TO-VERIFY too.
 *
 * The message is one mono WAV at the call's rate, written through the recorder (tmp, fsync, rename, 0600). A message
 * that ends with no sample is discarded: an empty file would be a silent entry in the page. The file name is the
 * recordings' shape, <epoch>-<call id>.wav, so the call log and the page share one naming rule and no number appears.
 * One message belongs to one call: a tap reset under it (the bridge restarted, an HD switch changed the rate) keeps
 * what was written and closes the message, where the call recording would open a second file.
 */
#define MACHINE_RATES		2		/* the PCM bus's rates: 8000 and 16000 Hz */
/*
 * A larger file is not a greeting: 2 MiB is about two minutes at 8 kHz mono and half a minute at 16 kHz stereo. The
 * cap matters beyond storage: the whole file is read and resampled in one go, off the call, when the line is idle.
 */
#define MACHINE_MAX_BYTES	(2u * 1024 * 1024)
#define MACHINE_FILL_MS		120		/* how far ahead of the bridge the ring is kept */

struct machine {
	int enabled;				/* a greeting is loaded: the machine may answer */
	int16_t *greeting[MACHINE_RATES];		/* mono at 8000 (0) and 16000 (1) Hz, owned */
	u32 frames[MACHINE_RATES];		/* mono samples of each */
	/* the greeting in flight */
	int playing;				/* the greeting is not through yet (the rest of the call is silence) */
	int slot;				/* the greeting in use: 0 = 8000, 1 = 16000 Hz */
	u32 pos;				/* the next sample of it */
	u32 ring_frames;			/* its length in play frames at the call's rate: the message starts past it */
	/* the message being recorded (one at a time) */
	struct recorder rec;
	int recording;
	u32 rec_rate;				/* the call's rate, the message's rate */
	unsigned int call_id;			/* the call the message belongs to */
	unsigned long long rec_frames;		/* mono frames written */
	char name[64];				/* "<epoch>-<call id>.wav" once saved, "" = no message */
	int saved;				/* 1 = the file named in `name` is finished and non-empty */
};

/* an empty machine: no greeting, no message */
void machine_init(struct machine *m);

/* release the greeting and abort an open message (the process is ending) */
void machine_free(struct machine *m);

/* read the greeting at `path` into both rates; 0 when it is loaded (enabled), or -errno. A refusal frees what was
 * loaded before and disables the machine; `log` gets one line naming the path and the reason, never content. */
int machine_greeting_load(struct machine *m, const char *path, FILE *log);

/* drop the greeting and disable the machine; never touches the message (a settings change may come mid-call) */
void machine_greeting_drop(struct machine *m);

/* the greeting in flight at the call's rate (8000 or 16000); 0, or -ENOENT when no greeting is loaded (no playback) */
int machine_start(struct machine *m, u32 call_rate);

/* the greeting is over (the handset took the call, the call ended): the caller's side is the bridge's again */
void machine_stop(struct machine *m);

/* fill `frames` play frames of the caller's side with the greeting, then silence: one sample a frame at 8000 Hz and
 * two at 16000 (the wideband frame's two halves). Always fills all of `frames`. */
void machine_fill(struct machine *m, int16_t *out, u32 frames);

/* start the message `<dir>/<final_name>.wav` at the call's rate; 0 or -errno (an open message is -EBUSY) */
int machine_message_open(struct machine *m, unsigned int call_id, u32 rate, const char *dir, const char *final_name);

/* append mono samples; 0 or -errno, and a failed feed leaves no message (the file is discarded) */
int machine_message_feed(struct machine *m, const int16_t *samples, u32 count);

/* close the message: its data bytes when it is kept (and named), 0 when it was empty and discarded, or -errno */
long long machine_message_finish(struct machine *m);

/* discard the open message; safe when nothing is open (an ending call, a restart) */
void machine_message_abort(struct machine *m);

#endif /* AW_PHONED_MACHINE_H */
