// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* How a finished call ended (phone.h): the call log's result words. */
#include "phone.h"

enum phone_result phone_result_of(enum phone_dir dir, int answered, int refused, int blocked, int cause)
{
	if (answered)
		return PHONE_RES_ANSWERED;
	if (blocked)
		return PHONE_RES_BLOCKED;
	if (dir == PHONE_DIR_IN)
		return refused ? PHONE_RES_REJECTED : PHONE_RES_MISSED;
	if (cause == 17)
		return PHONE_RES_BUSY;
	return cause == 18 || cause == 19 ? PHONE_RES_NO_ANSWER : PHONE_RES_FAILED;
}

const char *phone_result_name(enum phone_result r)
{
	static const char *const names[] = { "answered", "missed", "rejected", "blocked", "busy", "no_answer", "failed" };

	return (unsigned int)r < sizeof(names) / sizeof(names[0]) ? names[r] : "failed";
}
