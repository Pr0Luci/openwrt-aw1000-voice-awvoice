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
 * crossed line or the ringing this side plays), and the message starts only when the ring has played the greeting's last frame
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

/*
 * The greeting's place is one constant, not a setting: the package ships a default and the page manages that one file.
 * The two names beside it are the only places the paths appear - the staging below takes the folder and derives both,
 * so an upload can never be committed somewhere other than where the machine reads.
 *
 * The 0700 folder is the daemon's own (root only), the same rule the call log keeps; the package's default greeting is
 * installed under /usr/share/awvoice and copied here by the init script only when nothing is here yet, so an upgrade
 * never overwrites a greeting the user recorded.
 */
#define MACHINE_GREETING_DIR	"/etc/awvoice"
#define MACHINE_GREETING_NAME	"greeting.wav"
#define MACHINE_GREETING_TMP	".greeting.tmp"
#define MACHINE_GREETING_PATH	MACHINE_GREETING_DIR "/" MACHINE_GREETING_NAME

/*
 * One upload piece, in plain bytes (the page's greeting.js PUT_BYTES sends exactly this). A piece is small on purpose:
 * the decoded write lands between two steps of the line, and the whole file is capped, so an upload is a handful of
 * these and never one big reply the loop has to wait for.
 */
#define MACHINE_GREETING_PUT_MAX	16384

struct machine {
	int greeting_loaded;			/* the daemon holds a usable greeting: what the answer flag needs */
	int greeting_err;			/* 0 when one is loaded, else the -errno the last read gave (the page names it) */
	unsigned int greeting_ms;		/* the loaded greeting's length in milliseconds, 0 when there is none */
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

/* read the greeting at `path` into both rates; 0 when it is loaded, or -errno. A refusal frees what was loaded before
 * and leaves the machine unable to answer; either way the refusal's errno and the length are kept in the machine, so a
 * status reply is built from the read's own finding rather than a second probe of the file. `log` gets one line naming
 * the path and the reason, never content; NULL keeps it silent (a machine that is switched off is not a fault). */
int machine_greeting_load(struct machine *m, const char *path, FILE *log);

/* drop the greeting and leave the machine unable to answer; never touches the message (a settings change may come mid-call) */
void machine_greeting_drop(struct machine *m);

/*
 * The staged greeting upload: the page sends the file in pieces, they land in the folder's tmp name, and only a commit
 * that passes the same read the machine does puts them in the greeting's place. One upload at a time and no resume: a
 * broken piece ends the staging and the page starts over. The tmp is not the greeting, so staging may run during a
 * call the way the recorder's message writes do; commit is the only call that replaces the file, and the object lets it
 * through only while the line is quiet.
 *
 * The errors below are the ones a caller can be told about, and reply.h has one word each. -ELOOP is the greeting's own
 * name being a symlink, which is refused for writing because a root write through a link the user did not make could
 * land anywhere; reads still follow the link, since that file is what the machine plays. -EPROTO is a piece that does
 * not carry the offset the staging expects: a gap or a repeat, both of which mean the page has lost its place.
 * -EINVAL from a piece means no staging is open at all, which is the call being out of order rather than the file being
 * wrong, so the object answers INVALID_ARGUMENT instead of a word about the audio.
 */
struct machine_upload {
	FILE *file;				/* the open tmp, NULL when nothing is staged */
	u32 staged;				/* bytes in it, which is the offset the next piece must carry */
	int err;				/* the error that ended it, 0 while it is clean */
};

/* an upload that is not open: nothing staged, no error */
void machine_upload_init(struct machine_upload *up);

/* open the folder's tmp (0600), dropping a stale one from an earlier run or a dead upload; -ELOOP for a symlinked
 * greeting, -EBUSY when one is already open, else -errno from the folder or the file */
int machine_upload_begin(struct machine_upload *up, struct machine *m, const char *dir);

/* the next `len` bytes at `offset`; -EINVAL with nothing open, -EPROTO on a gap or a repeat, -EFBIG over the cap,
 * -EIO on a failed write - and none of them leaves a staging or a tmp behind */
int machine_upload_put(struct machine_upload *up, struct machine *m, const char *dir, u32 offset,
	const unsigned char *bytes, u32 len);

/* drop the staging and the tmp; succeeds when nothing is open, so the page can call it after any failure */
int machine_upload_abort(struct machine_upload *up, struct machine *m, const char *dir);

/*
 * Prove the staged bytes are a greeting the machine can play - optionally trimming the silence and adding the tone a
 * fresh recording still needs - and only then put them in the greeting's place and load them. `dir`'s greeting name is
 * the target, so a commit never writes where the machine does not read. Returns 0 with the length in `ms_out` and the
 * file in place; any refusal leaves the old greeting both on disk and loaded, the tmp gone, and the errno telling why.
 * `polish` is what the page passes for a recording: the trim and the tone are the recording's need, not the uploaded
 * file's, and an upload keeps exactly the audio its author made. A clip of nothing but silence is -ENODATA rather than
 * a greeting of silence, because a caller would hear a pause and no voice.
 */
int machine_upload_commit(struct machine_upload *up, struct machine *m, const char *dir, int polish,
	unsigned int *ms_out);

/* the folder's tmp, removed at the daemon's start: a previous run died mid-upload and left it behind */
void machine_upload_clean(const char *dir);

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
