/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONE_CORE_BLOCKING_H
#define AW_PHONE_CORE_BLOCKING_H

#include <stdio.h>

#include "numbers.h"
#include "phone.h"

/*
 * Call blocking (the page's U19..U21): exact numbers and number prefixes, for incoming and for outgoing calls, and one switch for
 * callers who withhold their number. Numbers are compared in international form (numbers.h), so a rule typed as a national number
 * matches a caller shown with the country code. An emergency number is never blocked: number_is_emergency() decides first and wins.
 * Pure data and decisions; the daemon fills it from the UCI section of type `blocking`.
 */
#define BLOCK_LIST_MAX	50

enum block_list { BLOCK_IN, BLOCK_IN_PREFIX, BLOCK_OUT, BLOCK_OUT_PREFIX, BLOCK_LISTS };

struct blocking {
	int anonymous;				/* 1 = refuse incoming calls that come without a number */
	char entry[BLOCK_LISTS][BLOCK_LIST_MAX][PHONE_NUMBER_MAX + 1];
	unsigned int count[BLOCK_LISTS];
	const struct number_region *region;	/* NULL = the default region */
};

void blocking_clear(struct blocking *b, const struct number_region *region);

/*
 * Add one rule. Exact rules follow the number rule (digits, '*', '#', a leading '+'; 1..PHONE_NUMBER_MAX); prefixes are digits with an optional
 * leading '+'. 0; -EINVAL (not valid), -ENOSPC (list full). On refusal `log` gets one line that never holds the number.
 */
int blocking_add(struct blocking *b, enum block_list list, const char *value, FILE *log);

/* 1 when an incoming call from `number` ("" = withheld) is to be refused; an emergency number never is */
int blocking_incoming(const struct blocking *b, const char *number);

/* 1 when dialling `number` is to be refused with the congestion tone; an emergency number never is */
int blocking_outgoing(const struct blocking *b, const char *number);

#endif /* AW_PHONE_CORE_BLOCKING_H */
