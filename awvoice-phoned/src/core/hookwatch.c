// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "hookwatch.h"

#define FLASH_MIN_LOW		20
#define FLASH_MIN_HIGH		500
#define FLASH_MAX_LOW		300
#define FLASH_MAX_HIGH		3000
#define MIN_SPREAD		100	/* the hang-up limit must clear the flash limit by this much */

static int one_value(const char *text, unsigned int low, unsigned int high, unsigned int fallback, const char *name,
	unsigned int *out, char *why, size_t cap)
{
	char *end;
	unsigned long v;

	if (!text || !*text) {
		*out = fallback;
		return 0;
	}
	v = strtoul(text, &end, 10);
	if (*end || end == text || v < low || v > high) {
		snprintf(why, cap, "%s must be a whole number of milliseconds from %u to %u", name, low, high);
		return -EINVAL;
	}
	*out = (unsigned int)v;
	return 0;
}

int hookwatch_cfg_parse(const char *flash_min, const char *flash_max, struct hookwatch_cfg *out, char *why, size_t cap)
{
	struct hookwatch_cfg c;
	int err = one_value(flash_min, FLASH_MIN_LOW, FLASH_MIN_HIGH, HOOKWATCH_FLASH_MIN_DEFAULT_MS, "flash_min_ms",
		&c.flash_min_ms, why, cap);

	if (!err)
		err = one_value(flash_max, FLASH_MAX_LOW, FLASH_MAX_HIGH, HOOKWATCH_FLASH_MAX_DEFAULT_MS, "flash_max_ms", &c.flash_max_ms, why, cap);
	if (!err && c.flash_max_ms < c.flash_min_ms + MIN_SPREAD) {
		snprintf(why, cap, "flash_max_ms must be at least %u ms above flash_min_ms", MIN_SPREAD);
		err = -EINVAL;
	}
	if (!err)
		*out = c;
	return err;
}

void hookwatch_init(struct hookwatch *hw, const struct hookwatch_cfg *cfg, int off_hook, unsigned long long now_ms)
{
	hw->cfg = *cfg;
	hw->off_hook = !!off_hook;
	hw->cleared = !off_hook;
	hw->broke_at = now_ms;
	hw->edges = 0;
	hw->glitches = 0;
}

enum hookwatch_event hookwatch_feed(struct hookwatch *hw, unsigned long long now_ms, int off_hook, unsigned int *width_ms)
{
	unsigned long long down;

	off_hook = !!off_hook;
	if (hw->off_hook && !off_hook) {			/* the break begins */
		hw->off_hook = 0;
		hw->edges++;
		hw->broke_at = now_ms;
		return HW_NONE;
	}
	down = now_ms - hw->broke_at;
	if (!hw->off_hook && !off_hook) {			/* the break goes on: on hook once it is long enough */
		if (!hw->cleared && down >= hw->cfg.flash_max_ms) {
			hw->cleared = 1;
			*width_ms = hw->cfg.flash_max_ms;
			return HW_ONHOOK;
		}
		return HW_NONE;
	}
	if (!hw->off_hook && off_hook) {			/* the handset is back */
		hw->off_hook = 1;
		hw->edges++;
		*width_ms = (unsigned int)down;
		/* a break that was not watched (nobody fed the engine while it lasted) and is long enough is a clear-down too */
		if (hw->cleared || down >= hw->cfg.flash_max_ms) {
			hw->cleared = 0;
			return HW_LIFT;
		}
		return down >= hw->cfg.flash_min_ms ? HW_FLASH : HW_BOUNCE;
	}
	return HW_NONE;
}

enum hookwatch_event hookwatch_flag(struct hookwatch *hw, int flag)
{
	if (!flag)
		return HW_NONE;			/* the changes seen so far stay owed to a later flag */
	if (hw->edges) {
		hw->edges = 0;			/* the flag is explained by the samples */
		return HW_NONE;
	}
	hw->glitches++;
	return HW_GLITCH;
}
