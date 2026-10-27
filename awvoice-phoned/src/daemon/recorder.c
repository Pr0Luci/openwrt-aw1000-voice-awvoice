// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "recorder.h"

int recorder_start(struct recorder *rc, const char *dir, const char *final_name, u32 rate, u32 channels)
{
	unsigned char header[44];
	FILE *f;

	rc->file = NULL;
	rc->open = false;
	if (!dir || !*dir || !final_name || !*final_name)
		return -EINVAL;
	if (recording_wav_header(header, rate, channels, 0) != 44)	/* a bad rate or channel count is refused before anything is created */
		return -EINVAL;
	if (snprintf(rc->final_path, sizeof(rc->final_path), "%s/%s.wav", dir, final_name) >= (int)sizeof(rc->final_path) ||
	    snprintf(rc->tmp_path, sizeof(rc->tmp_path), "%s.tmp", rc->final_path) >= (int)sizeof(rc->tmp_path))
		return -ENAMETOOLONG;
	(void)mkdir(dir, 0755);		/* an existing directory is fine; a real failure shows at the open */
	f = fopen(rc->tmp_path, "w");
	if (!f)
		return -errno;
	fchmod(fileno(f), 0600);	/* call content is private, like the call log */
	if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
		fclose(f);
		unlink(rc->tmp_path);
		return -EIO;
	}
	rc->file = f;
	rc->rate = rate;
	rc->channels = channels;
	rc->data_bytes = 0;
	rc->open = true;
	return 0;
}

int recorder_feed(struct recorder *rc, const u16 *samples, u32 count)
{
	if (!rc->open)
		return -EINVAL;
	if (!count)
		return 0;
	if (fwrite(samples, sizeof(*samples), count, rc->file) != count) {
		recorder_abort(rc);	/* a failed write leaves a broken file: close and discard the tmp */
		return -EIO;
	}
	rc->data_bytes += count * 2u;
	return 0;
}

long long recorder_finish(struct recorder *rc)
{
	unsigned char buf[44];
	FILE *f;
	int err = 0;

	if (!rc->open)		/* a second finish, or one after a failed feed, is -EINVAL and changes nothing */
		return -EINVAL;
	f = rc->file;
	rc->file = NULL;
	rc->open = false;
	if (recording_wav_header(buf, rc->rate, rc->channels, rc->data_bytes) != 44 || fseek(f, 0, SEEK_SET) ||
	    fwrite(buf, 1, sizeof(buf), f) != sizeof(buf))
		err = -EIO;
	if (fflush(f) || ferror(f) || fsync(fileno(f)))
		err = -EIO;
	if (fclose(f) && !err)
		err = -EIO;
	if (!err && rename(rc->tmp_path, rc->final_path))
		err = -errno;
	if (err)
		unlink(rc->tmp_path);	/* nothing half-written is left under the final name */
	return err ? err : (long long)rc->data_bytes;
}

void recorder_abort(struct recorder *rc)
{
	if (!rc->open)
		return;		/* nothing open: nothing to discard */
	fclose(rc->file);
	rc->file = NULL;
	rc->open = false;
	unlink(rc->tmp_path);
}
