// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The call log: records, the 200 cap, delete by type, the files (atomic write, damaged file, flash interval): make -f Makefile.host test */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "calllog.h"

static char dir[64];
static char ram[96], flash[96];

static void fresh_dir(void)
{
	snprintf(dir, sizeof(dir), "/tmp/aw-calllog-XXXXXX");
	assert(mkdtemp(dir));
	snprintf(ram, sizeof(ram), "%s/run/calls.json", dir);
	snprintf(flash, sizeof(flash), "%s/calls.json", dir);
}

static struct phone_call_record rec(enum phone_dir d, enum phone_result r, const char *number, unsigned long long answered, unsigned long long end, int cause)
{
	struct phone_call_record x;

	memset(&x, 0, sizeof(x));
	x.dir = d;
	x.result = r;
	snprintf(x.number, sizeof(x.number), "%s", number);
	x.answered_ms = answered;
	x.end_ms = end;
	x.cause = cause;
	return x;
}

static struct calllog *new_log(unsigned long long now)
{
	struct calllog *l = malloc(sizeof(*l));

	assert(l);
	calllog_init(l, ram, flash, now);
	return l;
}

static void test_add_and_types(void)
{
	struct calllog *l = new_log(0);
	struct phone_call_record a = rec(PHONE_DIR_IN, PHONE_RES_ANSWERED, "5551", 1000, 66000, 0);
	struct phone_call_record m = rec(PHONE_DIR_IN, PHONE_RES_MISSED, "5552", 0, 9000, 0);
	struct phone_call_record b = rec(PHONE_DIR_IN, PHONE_RES_BLOCKED, "5553", 0, 9000, 0);
	struct phone_call_record o = rec(PHONE_DIR_OUT, PHONE_RES_BUSY, "45", 0, 9000, 17);

	calllog_add(l, &a, 1000, NULL);
	calllog_add(l, &m, 1100, NULL);
	calllog_add(l, &b, 1200, NULL);
	calllog_add(l, &o, 1300, NULL);
	assert(l->n == 4 && l->next_id == 5);
	/* newest first, ids rise */
	assert(l->e[0].id == 4 && l->e[3].id == 1);
	assert(l->e[0].type == CALLLOG_OUT && l->e[1].type == CALLLOG_IN && l->e[2].type == CALLLOG_MISS && l->e[3].type == CALLLOG_IN);
	/* only a missed call is MISS; a blocked or rejected one is IN with its own result */
	assert(l->e[1].result == PHONE_RES_BLOCKED && l->e[2].result == PHONE_RES_MISSED);
	assert(l->e[3].duration_s == 65 && l->e[2].duration_s == 0 && l->e[0].cause == 17);
	assert(!strcmp(l->e[3].number, "5551") && l->e[3].ts == 1000);
	assert(l->ram_dirty && l->flash_dirty);
	assert(!strcmp(calllog_type_name(CALLLOG_MISS), "MISS"));
	free(l);
}

static void test_cap(void)
{
	struct calllog *l = new_log(0);
	struct phone_call_record a = rec(PHONE_DIR_IN, PHONE_RES_ANSWERED, "7", 1, 2000, 0);
	unsigned int k;

	for (k = 0; k < CALLLOG_MAX + 5; k++)
		calllog_add(l, &a, k, NULL);
	assert(l->n == CALLLOG_MAX);
	assert(l->e[0].id == CALLLOG_MAX + 5 && l->e[CALLLOG_MAX - 1].id == 6);	/* the oldest five are gone */
	free(l);
}

static void test_delete(void)
{
	struct calllog *l = new_log(0);
	struct phone_call_record a = rec(PHONE_DIR_IN, PHONE_RES_ANSWERED, "1", 1, 2000, 0);
	struct phone_call_record m = rec(PHONE_DIR_IN, PHONE_RES_MISSED, "2", 0, 0, 0);
	struct phone_call_record o = rec(PHONE_DIR_OUT, PHONE_RES_FAILED, "3", 0, 0, 0);
	enum calllog_type t;

	calllog_add(l, &a, 1, NULL);
	calllog_add(l, &m, 2, NULL);
	calllog_add(l, &o, 3, NULL);
	calllog_add(l, &m, 4, NULL);
	l->ram_dirty = l->flash_dirty = 0;
	t = CALLLOG_MISS;
	assert(calllog_delete(l, &t) == 2 && l->n == 2 && l->ram_dirty && l->flash_dirty);
	assert(l->e[0].type == CALLLOG_OUT && l->e[1].type == CALLLOG_IN);	/* order kept */
	l->ram_dirty = 0;
	assert(calllog_delete(l, &t) == 0 && !l->ram_dirty);			/* nothing removed, nothing to write */
	assert(calllog_delete(l, NULL) == 2 && l->n == 0);
	assert(calllog_delete(l, NULL) == 0);
	/* the ids keep rising after a delete */
	calllog_add(l, &a, 5, NULL);
	assert(l->e[0].id == 5);
	assert(calllog_type_parse("OUT", &t) == 0 && t == CALLLOG_OUT);
	assert(calllog_type_parse("out", &t) == -EINVAL && calllog_type_parse("", &t) == -EINVAL && calllog_type_parse(NULL, &t) == -EINVAL);
	free(l);
}

static char *slurp(const char *path)
{
	static char buf[8192];
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return NULL;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	fclose(f);
	return buf;
}

static void test_files(void)
{
	struct calllog *l, *back;
	struct phone_call_record a = rec(PHONE_DIR_IN, PHONE_RES_ANSWERED, "5551", 1000, 66000, 0);
	struct phone_call_record o = rec(PHONE_DIR_OUT, PHONE_RES_BUSY, "45", 0, 9000, 17);
	char tmp[128];
	struct stat sb;
	FILE *log;
	char *lbuf = NULL;
	size_t lsize = 0;

	fresh_dir();
	l = new_log(0);
	calllog_add(l, &a, 1700000000ull, NULL);
	calllog_add(l, &o, 1700000100ull, "1700000100-9.wav");
	/* the RAM copy is written at once (the directory is made), the flash copy only when its interval is over */
	assert(calllog_persist(l, 5000, 0) == 0);
	assert(slurp(ram) && !slurp(flash) && !l->ram_dirty && l->flash_dirty);
	snprintf(tmp, sizeof(tmp), "%s.tmp", ram);
	assert(access(tmp, F_OK) != 0);					/* the temporary name is gone: the write was a rename */
	assert(stat(ram, &sb) == 0 && (sb.st_mode & 0777) == 0600);
	assert(strstr(slurp(ram), "{\"id\":2,\"type\":\"OUT\",\"number\":\"45\",\"ts\":1700000100,\"durationSec\":0,\"result\":\"busy\",\"cause\":17,\"recording\":\"1700000100-9.wav\"},"));
	assert(strstr(slurp(ram), "\"recording\":\"1700000100-9.wav\""));	/* the recording rides the entry that owns it */
	assert(calllog_persist(l, CALLLOG_FLASH_MS - 1, 0) == 0 && !slurp(flash));
	assert(calllog_persist(l, CALLLOG_FLASH_MS, 0) == 0 && slurp(flash) && !l->flash_dirty);
	/* a new call after a flash write waits for the next interval, but a final write goes at once */
	calllog_add(l, &a, 1700000200ull, NULL);
	assert(calllog_persist(l, CALLLOG_FLASH_MS + 10, 0) == 0);
	assert(strstr(slurp(ram), "\"id\":3") && !strstr(slurp(flash), "\"id\":3") && l->flash_dirty);
	assert(calllog_persist(l, CALLLOG_FLASH_MS + 20, 1) == 0 && strstr(slurp(flash), "\"id\":3"));

	/* a restart: the RAM copy is read, and the ids go on */
	back = new_log(0);
	log = open_memstream(&lbuf, &lsize);
	assert(calllog_load(back, log) == 0 && back->n == 3 && back->next_id == 4);
	assert(back->e[0].id == 3 && !strcmp(back->e[1].number, "45") && back->e[1].result == PHONE_RES_BUSY && back->e[1].cause == 17);
	assert(back->e[2].duration_s == 65 && back->e[2].ts == 1700000000ull && back->e[2].type == CALLLOG_IN);
	assert(!strcmp(back->e[1].recording, "1700000100-9.wav") && !back->e[2].recording[0]);	/* the name round-trips, the absent stays absent */
	assert(!back->ram_dirty && !back->flash_dirty);
	/* after a reboot only the flash copy is there */
	unlink(ram);
	calllog_init(back, ram, flash, 0);
	assert(calllog_load(back, log) == 0 && back->n == 3);
	/* neither file: an empty log */
	unlink(flash);
	calllog_init(back, ram, flash, 0);
	assert(calllog_load(back, log) == -ENOENT && back->n == 0 && back->next_id == 1);
	fclose(log);
	free(lbuf);
	free(back);
	free(l);
}

static void write_text(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	assert(f);
	fputs(text, f);
	fclose(f);
}

static void test_damaged(void)
{
	static const char *const bad_files[] = {
		"garbage\n",
		"[\n{\"id\":1,\"type\":\"IN\",\"number\":\"5\",\"ts\":1,\"durationSec\":0,\"result\":\"missed\",\"cause\":0}\n",	/* cut short */
		"[\n{\"id\":1,\"type\":\"XX\",\"number\":\"5\",\"ts\":1,\"durationSec\":0,\"result\":\"missed\",\"cause\":0}\n]\n",
		"[\n{\"id\":0,\"type\":\"IN\",\"number\":\"5\",\"ts\":1,\"durationSec\":0,\"result\":\"missed\",\"cause\":0}\n]\n",
		"[\n{\"id\":1,\"type\":\"IN\",\"number\":\"5\",\"ts\":1,\"durationSec\":0,\"result\":\"lost\",\"cause\":0}\n]\n",
		"[\n{\"id\":1,\"type\":\"IN\",\"number\":\"12345678901234567890123456789012345\",\"ts\":1,\"durationSec\":0,\"result\":\"missed\",\"cause\":0}\n]\n",
		"[\n{\"id\":1,\"type\":\"IN\",\"ts\":1}\n]\n",
	};
	unsigned int k;

	for (k = 0; k < sizeof(bad_files) / sizeof(bad_files[0]); k++) {
		struct calllog *l;
		char badpath[128], *lbuf = NULL;
		size_t lsize = 0;
		FILE *log = open_memstream(&lbuf, &lsize);

		fresh_dir();
		snprintf(badpath, sizeof(badpath), "%s/run", dir);
		mkdir(badpath, 0700);
		write_text(ram, bad_files[k]);
		l = new_log(0);
		assert(calllog_load(l, log) == -ENOENT && l->n == 0 && l->next_id == 1);
		fflush(log);
		snprintf(badpath, sizeof(badpath), "%s.bad", ram);
		assert(access(badpath, F_OK) == 0 && access(ram, F_OK) != 0);		/* kept for a look, out of the way */
		assert(lbuf && strstr(lbuf, "could not be read") && !strstr(lbuf, "5551") && !strstr(lbuf, "garbage"));
		fclose(log);
		free(lbuf);
		free(l);
	}
	/* a damaged RAM copy does not hide a good flash copy */
	{
		struct calllog *l, *w;
		struct phone_call_record a = rec(PHONE_DIR_IN, PHONE_RES_ANSWERED, "5551", 1000, 66000, 0);
		char badpath[128];

		fresh_dir();
		w = new_log(0);
		calllog_add(w, &a, 7, NULL);
		assert(calllog_persist(w, 0, 1) == 0);
		snprintf(badpath, sizeof(badpath), "%s/run", dir);
		mkdir(badpath, 0700);
		write_text(ram, "[\nnot json\n");
		l = new_log(0);
		assert(calllog_load(l, NULL) == 0 && l->n == 1 && l->e[0].ts == 7);
		free(l);
		free(w);
	}
}

static void test_unwritable(void)
{
	struct calllog *l;
	struct phone_call_record a = rec(PHONE_DIR_IN, PHONE_RES_ANSWERED, "5551", 1000, 66000, 0);

	fresh_dir();
	l = new_log(0);
	snprintf(ram, sizeof(ram), "%s/missing/deeper/calls.json", dir);
	l->ram_path = ram;
	calllog_add(l, &a, 1, NULL);
	assert(calllog_persist(l, 0, 0) != 0 && l->ram_dirty);		/* stays dirty: tried again */
	free(l);
}

int main(void)
{
	test_add_and_types();
	test_cap();
	test_delete();
	test_files();
	test_damaged();
	test_unwritable();
	puts("calllog: all tests passed");
	return 0;
}
