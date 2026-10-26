/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_SLIC_CONTACTS_H
#define AW_SLIC_CONTACTS_H

#include <stddef.h>

/*
 * The caller's name from a contacts file: one entry per line, a number, a space, the name. Numbers are compared as
 * digit strings; two numbers are the same person when the shorter one (at least CONTACTS_MATCH_MIN digits) is the
 * tail of the longer one, which covers a national number against the same number with its country code.
 * Returns 0 and the name in out, or a negative errno (-ENOENT: no such contact or no file).
 */
#define CONTACTS_MATCH_MIN 7

int contacts_find(const char *path, const char *number, char *out, size_t cap);

/* the same test on two digit strings, for the tests */
int contacts_same(const char *a, const char *b);

#endif
