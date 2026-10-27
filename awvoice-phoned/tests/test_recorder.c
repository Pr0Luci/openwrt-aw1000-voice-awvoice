// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* One recording's file lifecycle, on real files under /tmp: make -f Makefile.host test */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "recorder.h"

static u32 le16(const unsigned char *p)
{
	return p[0] | (u32)p[1] << 8;
}

static u32 le32(const unsigned char *p)
{
	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

/* the whole file, or -1 when it is not there */
static long read_all(const char *path, unsigned char *buf, size_t cap)
{
	FILE *f = fopen(path, "rb");
	long n;

	if (!f)
		return -1;
	n = (long)fread(buf, 1, cap, f);
	if (ferror(f))
		n = -1;
	fclose(f);
	return n;
}

/* the file is there and only there when it should be */
static int gone(const char *path)
{
	return access(path, F_OK) == -1 && errno == ENOENT;
}

/* start, feed, finish: the final file is the 44-byte header with the real sizes plus the samples fed, byte for byte */
static void test_lifecycle(const char *dir)
{
	struct recorder rc;
	static const u16 samples[] = { 0x7FFF, 0x8000, 0x00FF, 0xFF00, 0x1234 };
	unsigned char buf[512];
	char path[300];
	long n;
	u32 k;

	snprintf(path, sizeof(path), "%s/call-1.wav", dir);
	assert(recorder_start(&rc, dir, "call-1", 8000, 1) == 0);
	assert(rc.open && rc.file && rc.rate == 8000 && rc.data_bytes == 0);
	assert(!strcmp(rc.final_path, path));
	snprintf(path, sizeof(path), "%s/call-1.wav.tmp", dir);
	assert(!strcmp(rc.tmp_path, path));
	assert(recorder_feed(&rc, samples, 5) == 0 && rc.data_bytes == 10);
	assert(recorder_feed(&rc, samples, 0) == 0 && rc.data_bytes == 10);	/* nothing fed: nothing written */
	assert(recorder_finish(&rc) == 10);
	assert(!rc.open && !rc.file);
	assert(gone(rc.tmp_path));
	n = read_all(rc.final_path, buf, sizeof(buf));
	assert(n == 44 + 10);
	assert(!memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4));
	assert(!memcmp(buf + 12, "fmt ", 4) && !memcmp(buf + 36, "data", 4));
	assert(le32(buf + 4) == 36 + 10 && le32(buf + 16) == 16);
	assert(le16(buf + 20) == 1 && le16(buf + 22) == 1);
	assert(le32(buf + 24) == 8000 && le32(buf + 28) == 16000);
	assert(le16(buf + 32) == 2 && le16(buf + 34) == 16);
	assert(le32(buf + 40) == 10);
	for (k = 0; k < 5; k++) {		/* the samples round-trip, little-endian */
		assert(buf[44 + 2 * k] == (samples[k] & 0xFF));
		assert(buf[45 + 2 * k] == (samples[k] >> 8));
	}
	unlink(rc.final_path);
}

/* feed then abort: neither the tmp nor the final path exists, and aborting twice is fine */
static void test_abort(const char *dir)
{
	struct recorder rc;
	const u16 samples[4] = { 1, 2, 3, 4 };
	char path[300];

	assert(recorder_start(&rc, dir, "call-2", 16000, 1) == 0);
	assert(recorder_feed(&rc, samples, 4) == 0 && rc.data_bytes == 8);
	recorder_abort(&rc);
	assert(!rc.open && !rc.file);
	snprintf(path, sizeof(path), "%s/call-2.wav.tmp", dir);
	assert(gone(path));
	snprintf(path, sizeof(path), "%s/call-2.wav", dir);
	assert(gone(path));		/* nothing half-written under the final name */
	recorder_abort(&rc);
}

/* a recording with no samples is still a valid 44-byte WAV, with a zero data size */
static void test_zero(const char *dir)
{
	struct recorder rc;
	unsigned char buf[64];
	char path[300];

	assert(recorder_start(&rc, dir, "call-3", 16000, 1) == 0);
	assert(recorder_finish(&rc) == 0);
	assert(!rc.open && !rc.file);
	snprintf(path, sizeof(path), "%s/call-3.wav", dir);
	assert(read_all(path, buf, sizeof(buf)) == 44);
	assert(le32(buf + 24) == 16000 && le32(buf + 28) == 32000 && le32(buf + 40) == 0);
	unlink(path);
}

/* the directory may be missing: the mkdir at start creates its last element (deeper chains stay the caller's job) */
static void test_missing_dir(const char *dir)
{
	struct recorder rc;
	unsigned char buf[64];
	char parent[300], nested[300], path[300];

	snprintf(parent, sizeof(parent), "%s/d1", dir);
	assert(mkdir(parent, 0755) == 0);
	snprintf(nested, sizeof(nested), "%s/d2", parent);
	assert(recorder_start(&rc, nested, "call-4", 8000, 1) == 0);
	assert(recorder_finish(&rc) == 0);
	snprintf(path, sizeof(path), "%s/call-4.wav", nested);
	assert(read_all(path, buf, sizeof(buf)) == 44);
	unlink(path);
	rmdir(nested);
	rmdir(parent);
}

/* a finish on a recorder that is not open is -EINVAL and changes nothing, an abort on one is a no-op */
static void test_not_open(const char *dir)
{
	struct recorder rc;
	unsigned char buf[64];

	memset(&rc, 0, sizeof(rc));
	assert(recorder_finish(&rc) == -EINVAL);
	recorder_abort(&rc);
	assert(recorder_start(&rc, dir, "call-5", 8000, 1) == 0);
	assert(recorder_finish(&rc) == 0);
	assert(recorder_finish(&rc) == -EINVAL);
	assert(read_all(rc.final_path, buf, sizeof(buf)) == 44);	/* the finished file is untouched */
	unlink(rc.final_path);
}

/* a rate the PCM bus never runs at is refused before anything is created */
static void test_bad_rate(const char *dir)
{
	struct recorder rc;
	char path[300];

	assert(recorder_start(&rc, dir, "call-6", 44100, 1) == -EINVAL);
	assert(!rc.open && !rc.file);
	snprintf(path, sizeof(path), "%s/call-6.wav.tmp", dir);
	assert(gone(path));
	snprintf(path, sizeof(path), "%s/call-6.wav", dir);
	assert(gone(path));
}

int main(void)
{
	char dir[128];

	snprintf(dir, sizeof(dir), "/tmp/awvoice-recorder-test.%ld", (long)getpid());
	assert(mkdir(dir, 0755) == 0 || errno == EEXIST);
	test_lifecycle(dir);
	test_abort(dir);
	test_zero(dir);
	test_missing_dir(dir);
	test_not_open(dir);
	test_bad_rate(dir);
	rmdir(dir);
	puts("recorder: all tests passed");
	return 0;
}
