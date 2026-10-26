/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_HOOKWATCH_H
#define AW_HOOKWATCH_H

#include <stddef.h>

/*
 * The hook engine. It turns timestamped samples of the line chip's hook state into the events the call logic needs,
 * following the access-signalling classification (ETSI TS 101 868 clause 4.7): a loop interruption (a hook break)
 * is classified by its duration t, measured on a monotonic clock, never by counting polls:
 *
 *   t < flash_min          a bounce or a dial-pulse remnant: ignored (HW_BOUNCE)
 *   flash_min <= t < flash_max  the handset's flash key (register recall): HW_FLASH when the handset comes back
 *   t >= flash_max         on hook (the hang-up window): HW_ONHOOK is reported as soon as the break has lasted that long, the line
 *                          is then cleared; the next lift is a new call (HW_LIFT). A break that was not watched
 *                          (no samples while it lasted) and is at least that long also ends in HW_LIFT.
 *
 * Both limits are national / operator / handset parameters (a handset may send 100, 300, 600 or 1000 ms), so they
 * are configuration, not constants; the engine does no I/O and no allocation.
 */
#define HOOKWATCH_FLASH_MIN_DEFAULT_MS	80
#define HOOKWATCH_FLASH_MAX_DEFAULT_MS	300	/* quick clear-down; a handset whose flash key is 300 ms or longer needs a higher value */

struct hookwatch_cfg {
	unsigned int flash_min_ms;
	unsigned int flash_max_ms;
};

enum hookwatch_event {
	HW_NONE,
	HW_LIFT,	/* the handset was lifted for a new call (from the cleared state) */
	HW_FLASH,	/* a break of flash_min_ms..flash_max_ms ended; *width_ms is its length */
	HW_BOUNCE,	/* a break shorter than flash_min_ms ended: no change */
	HW_ONHOOK,	/* the break has lasted flash_max_ms: hang up; *width_ms = flash_max_ms */
	HW_GLITCH,	/* the chip's change flag fired but no break was seen: a loop change shorter than the poll spacing */
};

struct hookwatch {
	struct hookwatch_cfg cfg;
	int off_hook;			/* the last sample: 1 = handset up */
	int cleared;			/* on hook was reported (or the line never was off hook) */
	unsigned long long broke_at;	/* the monotonic time of the first on-hook sample of the break */
	unsigned int edges;		/* level changes fed since the chip's change flag was last taken */
	unsigned int glitches;		/* loop changes the chip flagged that no sample showed */
};

/* the configuration as typed (UCI options flash_min_ms and flash_max_ms of config phone 'main', set on the phone
 * settings page); NULL or empty selects the default.
 * 0, or -EINVAL with a message in why (flash_min 20..500, flash_max 300..3000, flash_max >= flash_min + 100) */
int hookwatch_cfg_parse(const char *flash_min, const char *flash_max, struct hookwatch_cfg *out, char *why, size_t cap);

void hookwatch_init(struct hookwatch *hw, const struct hookwatch_cfg *cfg, int off_hook, unsigned long long now_ms);

/* one sample; now_ms must not go backwards */
enum hookwatch_event hookwatch_feed(struct hookwatch *hw, unsigned long long now_ms, int off_hook, unsigned int *width_ms);

/*
 * The line chip also latches a loop-change flag (a debounced change of its hook state sets it, reading clears it), so
 * a change that came and went between two samples still leaves a trace. Call this with the flag each time it is read:
 * a flag that no fed sample explains (no level change since the last flag) is a loop change shorter than the poll
 * spacing -- HW_GLITCH. It never changes the call: a change that no sample measured has no duration to classify. A
 * flag with flag == 0 does nothing.
 */
enum hookwatch_event hookwatch_flag(struct hookwatch *hw, int flag);

#endif
