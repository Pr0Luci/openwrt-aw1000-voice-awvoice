/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_RECORDER_H
#define AW_PHONED_RECORDER_H

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>

#include "recording.h"

/*
 * The file lifecycle of one call recording: a WAV file (16-bit PCM, one or two channels) is written whole to `<dir>/<name>.wav.tmp`
 * and renamed to `<dir>/<name>.wav` only at the finish, so a power cut never leaves a half recording under the final
 * name. The 44-byte header is written with a zero size at the start and rewritten with the real one at the finish,
 * so the samples can be appended as they are captured. The recorder is channel-agnostic: the caller feeds exactly
 * the interleaved samples that belong in the file (the daemon feeds the far end and the handset, two channels).
 * Call content is private, so the tmp
 * file is 0600, like the call log. A finish on a recorder that is not open is -EINVAL and changes nothing; an abort
 * on one is a no-op.
 */
struct recorder {
	FILE *file;			/* the tmp file while it is open, NULL when not */
	char tmp_path[PATH_MAX];
	char final_path[PATH_MAX];
	u32 rate;
	u32 channels;			/* 1 = mono, 2 = the far end and the handset interleaved */
	u32 data_bytes;			/* bytes appended after the 44-byte header */
	bool open;
};

/* start `<dir>/<final_name>.wav` through its `.tmp`; the directory is created if missing; 0 or -errno, nothing left behind on error */
int recorder_start(struct recorder *rc, const char *dir, const char *final_name, u32 rate, u32 channels);

/* append `count` 16-bit samples to the data section; 0 or -errno, and a failed feed closes and discards the tmp */
int recorder_feed(struct recorder *rc, const u16 *samples, u32 count);

/* rewrite the header with the final size, close and rename to the final path: the data bytes, or -errno */
long long recorder_finish(struct recorder *rc);

/* discard the recording; safe to call when nothing is open */
void recorder_abort(struct recorder *rc);

#endif /* AW_PHONED_RECORDER_H */
