/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_CALLLOG_H
#define AW_PHONED_CALLLOG_H

#include <stddef.h>

#include "../core/phone.h"

/*
 * The call log: the last CALLLOG_MAX calls, newest first, one record per finished call (the page's `Call`, data contract v2).
 * The numbers in it are the one place a number may be stored; the file is readable by root only and the ubus methods
 * that return it sit behind the page's ACL.
 *
 * Two copies: the RAM copy (/var/run/awvoice-phoned/calls.json) is written after every call; the flash copy
 * (/etc/awvoice/calls.json) at most every CALLLOG_FLASH_MS (flash wear) and at stop. A file is written whole to a temporary
 * name and renamed over the old one, so a power cut leaves the old file or the new one. A file that cannot be read is renamed
 * to `<path>.bad`, logged without its content, and the log starts empty: never fatal. Retention 200 and the flash copy are
 * the defaults awaiting a decision.
 *
 * The file is one JSON array, one record per line, so that it is also readable by `jsonfilter`.
 */
#define CALLLOG_MAX		200
#define CALLLOG_FLASH_MS	(10ull * 60 * 1000)

enum calllog_type { CALLLOG_IN, CALLLOG_OUT, CALLLOG_MISS };

struct calllog_entry {
	unsigned int id;
	enum calllog_type type;
	char number[PHONE_NUMBER_MAX + 1];
	unsigned long long ts;		/* epoch seconds of the call's beginning */
	unsigned int duration_s;	/* seconds of speech (0 = not answered) */
	enum phone_result result;
	int cause;			/* TS 24.008, 0 = unknown */
	char recording[48];		/* the recording's file name in the recordings directory, "" = none */
};

struct calllog {
	struct calllog_entry e[CALLLOG_MAX];	/* e[0] is the newest */
	unsigned int n;
	unsigned int next_id;
	const char *ram_path, *flash_path;
	int ram_dirty, flash_dirty;
	unsigned long long flash_saved_ms;
};

/* empty log with the two paths (not copied) and the clock time the flash interval counts from */
void calllog_init(struct calllog *l, const char *ram_path, const char *flash_path, unsigned long long now_ms);

const char *calllog_type_name(enum calllog_type t);
/* "IN", "OUT" or "MISS" -> 0; anything else -> -EINVAL */
int calllog_type_parse(const char *text, enum calllog_type *t);

/* an incoming call that was not answered is MISS (missed, rejected, blocked all show as in the contract: only `missed` is MISS), else IN; outgoing is OUT */
enum calllog_type calllog_type_of(enum phone_dir dir, enum phone_result r);

/* add the finished call (newest first; the 201st oldest is dropped); marks both copies changed */
void calllog_add(struct calllog *l, const struct phone_call_record *rec, unsigned long long ts_epoch_s, const char *recording);

/* remove every record, or only those of one type; the number removed */
unsigned int calllog_delete(struct calllog *l, const enum calllog_type *only);

/* read the RAM copy, else the flash copy; 0 when a file was read, -ENOENT when neither exists. A damaged file is renamed `.bad`, and `log` gets one line without content. */
int calllog_load(struct calllog *l, FILE *log);

/* write what changed: the RAM copy now, the flash copy when its interval is over or `final`. 0 or the first -errno. */
int calllog_persist(struct calllog *l, unsigned long long now_ms, int final);

#endif /* AW_PHONED_CALLLOG_H */
