// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "calllog.h"

void calllog_init(struct calllog *l, const char *ram_path, const char *flash_path, unsigned long long now_ms)
{
	memset(l, 0, sizeof(*l));
	l->next_id = 1;
	l->ram_path = ram_path;
	l->flash_path = flash_path;
	l->flash_saved_ms = now_ms;
}

const char *calllog_type_name(enum calllog_type t)
{
	return t == CALLLOG_IN ? "IN" : t == CALLLOG_OUT ? "OUT" : "MISS";
}

int calllog_type_parse(const char *text, enum calllog_type *t)
{
	if (!text)
		return -EINVAL;
	if (!strcmp(text, "IN"))
		*t = CALLLOG_IN;
	else if (!strcmp(text, "OUT"))
		*t = CALLLOG_OUT;
	else if (!strcmp(text, "MISS"))
		*t = CALLLOG_MISS;
	else
		return -EINVAL;
	return 0;
}

enum calllog_type calllog_type_of(enum phone_dir dir, enum phone_result r)
{
	if (dir == PHONE_DIR_OUT)
		return CALLLOG_OUT;
	return r == PHONE_RES_MISSED ? CALLLOG_MISS : CALLLOG_IN;
}

void calllog_add(struct calllog *l, const struct phone_call_record *rec, unsigned long long ts_epoch_s)
{
	struct calllog_entry *e;

	if (l->n < CALLLOG_MAX)
		l->n++;
	memmove(&l->e[1], &l->e[0], (l->n - 1) * sizeof(l->e[0]));
	e = &l->e[0];
	memset(e, 0, sizeof(*e));
	e->id = l->next_id++;
	e->type = calllog_type_of(rec->dir, rec->result);
	snprintf(e->number, sizeof(e->number), "%s", rec->number);
	e->ts = ts_epoch_s;
	e->duration_s = rec->answered_ms && rec->end_ms > rec->answered_ms ? (unsigned int)((rec->end_ms - rec->answered_ms) / 1000) : 0;
	e->result = rec->result;
	e->cause = rec->cause;
	l->ram_dirty = l->flash_dirty = 1;
}

unsigned int calllog_delete(struct calllog *l, const enum calllog_type *only)
{
	unsigned int keep = 0, k;

	for (k = 0; k < l->n; k++)
		if (only && l->e[k].type != *only)
			l->e[keep++] = l->e[k];
	k = l->n - keep;
	l->n = keep;
	if (k)
		l->ram_dirty = l->flash_dirty = 1;
	return k;
}

/* ---- the file ------------------------------------------------------------------------------------------------------- */

static int write_file(const struct calllog *l, const char *path)
{
	char tmp[160];
	FILE *f;
	unsigned int k;
	int err = 0;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -ENAMETOOLONG;
	f = fopen(tmp, "w");
	if (!f)
		return -errno;
	fchmod(fileno(f), 0600);
	fputs("[\n", f);
	for (k = 0; k < l->n; k++) {
		const struct calllog_entry *e = &l->e[k];

		fprintf(f, "{\"id\":%u,\"type\":\"%s\",\"number\":\"%s\",\"ts\":%llu,\"durationSec\":%u,\"result\":\"%s\",\"cause\":%d}%s\n",
			e->id, calllog_type_name(e->type), e->number, e->ts, e->duration_s, phone_result_name(e->result), e->cause,
			k + 1 < l->n ? "," : "");
	}
	fputs("]\n", f);
	if (fflush(f) || ferror(f) || fsync(fileno(f)))
		err = -EIO;
	if (fclose(f) && !err)
		err = -EIO;
	if (!err && rename(tmp, path))
		err = -errno;
	if (err)
		unlink(tmp);
	return err;
}

static void make_parent(const char *path)
{
	char dir[160];
	char *slash;

	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = 0;
		(void)mkdir(dir, 0700);		/* an existing directory is fine; a real failure shows at the open */
	}
}

/* the field after `"key":` in a record line of our own format; NULL when the key is not there */
static const char *field(const char *line, const char *key)
{
	char pat[24];
	const char *at;

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	at = strstr(line, pat);
	return at ? at + strlen(pat) : NULL;
}

static int num_field(const char *line, const char *key, unsigned long long *out)
{
	const char *at = field(line, key);
	char *end;

	if (!at || *at < '0' || *at > '9')
		return 0;
	*out = strtoull(at, &end, 10);
	return end != at;
}

static int text_field(const char *line, const char *key, char *out, size_t cap)
{
	const char *at = field(line, key), *close;

	if (!at || *at != '"')
		return 0;
	close = strchr(at + 1, '"');
	if (!close || (size_t)(close - at - 1) >= cap)
		return 0;
	memcpy(out, at + 1, (size_t)(close - at - 1));
	out[close - at - 1] = 0;
	return 1;
}

static int result_parse(const char *text, enum phone_result *r)
{
	enum phone_result k;

	for (k = PHONE_RES_ANSWERED; k <= PHONE_RES_FAILED; k++)
		if (!strcmp(text, phone_result_name(k))) {
			*r = k;
			return 1;
		}
	return 0;
}

static int parse_line(const char *line, struct calllog_entry *e)
{
	unsigned long long id, ts, dur, cause;
	char type[8], number[PHONE_NUMBER_MAX + 2], result[16];
	const char *c;

	memset(e, 0, sizeof(*e));
	if (!num_field(line, "id", &id) || !num_field(line, "ts", &ts) || !num_field(line, "durationSec", &dur) ||
	    !text_field(line, "type", type, sizeof(type)) || !text_field(line, "number", number, sizeof(number)) ||
	    !text_field(line, "result", result, sizeof(result)) || calllog_type_parse(type, &e->type) ||
	    !result_parse(result, &e->result) || strlen(number) > PHONE_NUMBER_MAX || id == 0 || id > 0xffffffffu || dur > 0xffffffffu)
		return 0;
	c = field(line, "cause");		/* may be negative in principle; ours are not */
	if (!c)
		return 0;
	cause = (unsigned long long)strtoll(c, NULL, 10);
	e->id = (unsigned int)id;
	snprintf(e->number, sizeof(e->number), "%s", number);
	e->ts = ts;
	e->duration_s = (unsigned int)dur;
	e->cause = (int)cause;
	return 1;
}

/* read one file: 0 read, -ENOENT missing, -EBADMSG damaged (the log is left empty) */
static int read_file(struct calllog *l, const char *path)
{
	char line[256];
	FILE *f = fopen(path, "r");
	unsigned int n = 0;
	int bad = 0;

	if (!f)
		return errno == ENOENT ? -ENOENT : -EBADMSG;
	if (!fgets(line, sizeof(line), f) || strncmp(line, "[", 1))
		bad = 1;
	while (!bad && fgets(line, sizeof(line), f)) {
		if (line[0] == ']')
			break;
		if (n >= CALLLOG_MAX || !strchr(line, '\n') || !parse_line(line, &l->e[n])) {
			bad = 1;
			break;
		}
		n++;
	}
	if (!bad && line[0] != ']')
		bad = 1;		/* no closing bracket: cut short */
	fclose(f);
	if (bad) {
		l->n = 0;
		return -EBADMSG;
	}
	l->n = n;
	return 0;
}

int calllog_load(struct calllog *l, FILE *log)
{
	const char *paths[2] = { l->ram_path, l->flash_path };
	unsigned int k, m;
	int found = -ENOENT;

	for (k = 0; k < 2 && found; k++) {
		int err;

		if (!paths[k])
			continue;
		err = read_file(l, paths[k]);
		if (err == -EBADMSG) {
			char bad[170];

			snprintf(bad, sizeof(bad), "%s.bad", paths[k]);
			if (log)
				fprintf(log, "the call log file %s could not be read: it is kept as .bad and the log starts empty\n", k ? "in flash" : "in RAM");
			(void)rename(paths[k], bad);
		} else if (!err) {
			found = 0;
		}
	}
	if (found)
		l->n = 0;
	for (m = 0; m < l->n; m++)
		if (l->e[m].id >= l->next_id)
			l->next_id = l->e[m].id + 1;
	l->ram_dirty = 0;
	l->flash_dirty = 0;
	return found;
}

int calllog_persist(struct calllog *l, unsigned long long now_ms, int final)
{
	int err = 0, e2;

	if (l->ram_dirty && l->ram_path) {
		make_parent(l->ram_path);
		err = write_file(l, l->ram_path);
		if (!err)
			l->ram_dirty = 0;
	}
	if (l->flash_dirty && l->flash_path && (final || now_ms - l->flash_saved_ms >= CALLLOG_FLASH_MS)) {
		e2 = write_file(l, l->flash_path);
		l->flash_saved_ms = now_ms;		/* a failed write is not retried at every call: the next interval tries again */
		if (!e2)
			l->flash_dirty = 0;
		else if (!err)
			err = e2;
	}
	return err;
}
