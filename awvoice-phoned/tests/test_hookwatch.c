// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <assert.h>
#include <errno.h>
#include <string.h>

#include "../src/core/hookwatch.h"

/* feed a break of `down` ms (sampled every `step` ms) and return the event that closes it; *during gets any earlier event */
static enum hookwatch_event run_break(struct hookwatch *hw, unsigned long long *t, unsigned int down, unsigned int step,
	enum hookwatch_event *during, unsigned int *width)
{
	unsigned int at;
	enum hookwatch_event e;

	*during = HW_NONE;
	for (at = step; at < down; at += step) {
		*t += step;
		e = hookwatch_feed(hw, *t, 0, width);
		if (e != HW_NONE)
			*during = e;
	}
	*t += step;
	return hookwatch_feed(hw, *t, 1, width);
}

int main(void)
{
	struct hookwatch hw;
	struct hookwatch_cfg cfg;
	unsigned long long t = 1000;
	enum hookwatch_event during;
	unsigned int w;
	char why[120];

	assert(hookwatch_cfg_parse(NULL, NULL, &cfg, why, sizeof(why)) == 0);
	assert(cfg.flash_min_ms == 80 && cfg.flash_max_ms == 300);

	/* a lift from the idle (on-hook) line is a new call */
	hookwatch_init(&hw, &cfg, 0, t);
	assert(hookwatch_feed(&hw, t += 10, 0, &w) == HW_NONE);
	assert(hookwatch_feed(&hw, t += 10, 1, &w) == HW_LIFT);

	/* the handset's flash keys: with the limit raised (the user sets it once on the phone settings page) the keys of
	 * 100, 300, 600 and 1000 ms are all flashes */
	assert(hookwatch_cfg_parse("80", "1200", &cfg, why, sizeof(why)) == 0);
	hookwatch_init(&hw, &cfg, 0, t);
	t += 10;
	assert(hookwatch_feed(&hw, t, 1, &w) == HW_LIFT);
	{
		static const unsigned int keys[] = { 100, 300, 600, 1000 };
		unsigned int k;

		for (k = 0; k < 4; k++) {
			t += 500;
			hw.off_hook = 1;
			assert(hookwatch_feed(&hw, t, 0, &w) == HW_NONE);
			assert(run_break(&hw, &t, keys[k], 10, &during, &w) == HW_FLASH && during == HW_NONE);
			assert(w >= keys[k] - 10 && w <= keys[k] + 10);
		}
	}

	/* with the defaults (80..300 ms) a 100 ms key is a flash and a 600 ms break is on hook, reported 300 ms into the break */
	assert(hookwatch_cfg_parse(NULL, NULL, &cfg, why, sizeof(why)) == 0);
	hookwatch_init(&hw, &cfg, 1, t);
	t += 500;
	assert(hookwatch_feed(&hw, t, 0, &w) == HW_NONE);
	assert(run_break(&hw, &t, 100, 10, &during, &w) == HW_FLASH && during == HW_NONE);
	t += 500;
	assert(hookwatch_feed(&hw, t, 0, &w) == HW_NONE);
	assert(run_break(&hw, &t, 600, 10, &during, &w) == HW_LIFT && during == HW_ONHOOK);

	/* a bounce below flash_min changes nothing */
	hookwatch_init(&hw, &cfg, 1, t);
	t += 500;
	assert(hookwatch_feed(&hw, t, 0, &w) == HW_NONE);
	assert(run_break(&hw, &t, 40, 10, &during, &w) == HW_BOUNCE);

	/* a break of flash_max_ms: on hook is reported while the handset is still down, the lift after it is a new call */
	t += 500;
	assert(hookwatch_feed(&hw, t, 0, &w) == HW_NONE);
	assert(run_break(&hw, &t, 1500, 25, &during, &w) == HW_LIFT && during == HW_ONHOOK);

	/* the on-hook event fires once */
	t += 500;
	hookwatch_feed(&hw, t, 0, &w);
	{
		unsigned int onhooks = 0, k;

		for (k = 0; k < 200; k++)
			onhooks += hookwatch_feed(&hw, t += 25, 0, &w) == HW_ONHOOK;
		assert(onhooks == 1);
	}

	/* the timing is the clock's, not the poll rate's: the same 600 ms break at 5 ms and at 100 ms steps */
	assert(hookwatch_cfg_parse("80", "800", &cfg, why, sizeof(why)) == 0);
	{
		static const unsigned int steps[] = { 5, 100 };
		unsigned int k;

		for (k = 0; k < 2; k++) {
			hookwatch_init(&hw, &cfg, 1, t);
			t += 500;
			assert(hookwatch_feed(&hw, t, 0, &w) == HW_NONE);
			assert(run_break(&hw, &t, 600, steps[k], &during, &w) == HW_FLASH && w >= 600 && w <= 700);
		}
	}

	/* the limits are configurable: a 2 s hang-up window keeps a 1.5 s break a flash */
	assert(hookwatch_cfg_parse("80", "2000", &cfg, why, sizeof(why)) == 0 && cfg.flash_max_ms == 2000);
	hookwatch_init(&hw, &cfg, 1, t);
	t += 500;
	assert(hookwatch_feed(&hw, t, 0, &w) == HW_NONE);
	assert(run_break(&hw, &t, 1500, 25, &during, &w) == HW_FLASH && during == HW_NONE);

	/* the chip's change flag: explained by a fed break, or a loop change shorter than the poll spacing */
	hookwatch_init(&hw, &cfg, 1, t);
	assert(hookwatch_flag(&hw, 0) == HW_NONE);
	assert(hookwatch_flag(&hw, 1) == HW_GLITCH && hw.glitches == 1);	/* a flag with no break seen */
	t += 500;
	assert(hookwatch_feed(&hw, t, 0, &w) == HW_NONE);
	assert(run_break(&hw, &t, 100, 5, &during, &w) == HW_FLASH);
	assert(hookwatch_flag(&hw, 1) == HW_NONE && hw.glitches == 1);		/* the break's own flag */
	assert(hookwatch_flag(&hw, 1) == HW_GLITCH && hw.glitches == 2);	/* a second flag with nothing behind it */
	t += 500;
	assert(hookwatch_feed(&hw, t, 0, &w) == HW_NONE);			/* a break seen before its flag was readable ... */
	assert(hookwatch_flag(&hw, 0) == HW_NONE);
	assert(run_break(&hw, &t, 100, 5, &during, &w) == HW_FLASH);
	assert(hookwatch_flag(&hw, 1) == HW_NONE && hw.glitches == 2);		/* ... is not called a glitch when the flag comes late */

	/* the configuration check */
	assert(hookwatch_cfg_parse("10", NULL, &cfg, why, sizeof(why)) == -EINVAL && strstr(why, "flash_min_ms"));
	assert(hookwatch_cfg_parse("80", "250", &cfg, why, sizeof(why)) == -EINVAL && strstr(why, "flash_max_ms"));
	assert(hookwatch_cfg_parse("400", "450", &cfg, why, sizeof(why)) == -EINVAL && strstr(why, "above"));
	assert(hookwatch_cfg_parse("abc", NULL, &cfg, why, sizeof(why)) == -EINVAL);
	assert(hookwatch_cfg_parse("", "", &cfg, why, sizeof(why)) == 0 && cfg.flash_min_ms == 80);
	return 0;
}
