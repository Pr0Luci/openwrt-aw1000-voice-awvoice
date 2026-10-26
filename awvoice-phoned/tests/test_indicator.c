/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
/* The phone LED: the state machine and its levels, the blink timing on the step clock, the readiness rule,
 * the dark at the stop, and the settings' rules (the LED name, the two rates) that config_set keeps. The
 * sysfs side is driven through a temporary file standing in for the brightness file. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "indicator.h"
#include "phone.h"

static struct phone_status status(int ready, int off_hook, int ringing)
{
	struct phone_status st;

	memset(&st, 0, sizeof(st));
	st.modem_up = 1;
	st.registered = ready ? 1 : 0;
	st.ims_registered = ready ? 1 : 0;
	st.off_hook = off_hook;
	st.line = ringing ? PHONE_LINE_RINGING : PHONE_LINE_IDLE;
	return st;
}

static char level(FILE *led)
{
	char c = 'x';

	pread(fileno(led), &c, 1, 0);		/* the daemon writes with pwrite: read the byte behind its back */
	return c;
}

int main(void)
{
	struct indicator ind;
	struct phone_status st;
	FILE *log = tmpfile(), *led = tmpfile();

	assert(log && led);

	indicator_init(&ind);
	assert(ind.fd == -1 && ind.lit == -1 && ind.ring_rate_ms == 300 && ind.offhook_rate_ms == 1000);
	assert(!strcmp(indicator_state_name(&ind), "off"));

	/* a LED the host does not have: the setting is cleared with one log line, nothing crashes */
	indicator_apply(&ind, "green:phone", 0, 0, log);
	assert(!ind.led[0] && ind.fd == -1);
	fseek(log, 0, SEEK_END);

	/* the state machine with the brightness file standing in */
	indicator_init(&ind);
	snprintf(ind.led, sizeof(ind.led), "green:phone");
	ind.fd = fileno(led);

	st = status(1, 0, 0);
	indicator_step(&ind, &st, 1000);			/* ready: solid */
	assert(ind.state == IND_READY && level(led) == '1');

	st = status(1, 1, 0);
	indicator_step(&ind, &st, 2000);			/* the handset up: the slow blink starts lit, half of 1000 ms */
	assert(ind.state == IND_OFFHOOK && level(led) == '1');
	indicator_step(&ind, &st, 2400);
	assert(ind.state == IND_OFFHOOK && level(led) == '1');	/* before the edge: no change */
	indicator_step(&ind, &st, 2501);
	assert(ind.state == IND_OFFHOOK && level(led) == '0');	/* the edge: dark */
	indicator_step(&ind, &st, 3001);
	assert(ind.state == IND_OFFHOOK && level(led) == '1');

	st = status(1, 0, 1);
	indicator_step(&ind, &st, 4000);			/* an incoming call: the fast blink, half of 300 ms */
	assert(ind.state == IND_RING && level(led) == '1');
	indicator_step(&ind, &st, 4100);
	assert(ind.state == IND_RING && level(led) == '1');
	indicator_step(&ind, &st, 4151);
	assert(ind.state == IND_RING && level(led) == '0');
	indicator_step(&ind, &st, 4301);
	assert(ind.state == IND_RING && level(led) == '1');

	st = status(0, 0, 0);
	indicator_step(&ind, &st, 5000);			/* not ready (a registration dropped): dark */
	assert(ind.state == IND_OFF && level(led) == '0');

	st = status(1, 0, 0);
	indicator_step(&ind, &st, 6000);			/* ready again: solid */
	assert(ind.state == IND_READY && level(led) == '1');

	/* another program blots the LED out: the read-back restores the level (said once) */
	if (pwrite(ind.fd, "0", 1, 0) != 1)
		return 1;
	indicator_step(&ind, &st, 8500);
	assert(ind.state == IND_READY && level(led) == '1');

	int rfd = dup(ind.fd);			/* close() takes the descriptor the levels were read through */
	char last = 'x';

	indicator_close(&ind);
	pread(rfd, &last, 1, 0);
	close(rfd);
	assert(ind.fd == -1 && ind.state == IND_OFF && last == '0');

	/* the settings' rules */
	struct aw_config c;

	config_defaults(&c);
	assert(c.ring_rate_ms == 300 && c.offhook_rate_ms == 1000 && !c.led[0]);
	assert(!config_set(&c, "led", "green:phone", NULL) && !strcmp(c.led, "green:phone"));
	assert(!config_set(&c, "led", "", NULL) && !c.led[0]);
	assert(config_set(&c, "led", "has space", NULL) == -EINVAL);
	assert(config_set(&c, "led", "x/slash", NULL) == -EINVAL);
	assert(!config_set(&c, "ring_rate_ms", "100", NULL) && c.ring_rate_ms == 100);
	assert(config_set(&c, "ring_rate_ms", "99", NULL) == -EINVAL);
	assert(config_set(&c, "ring_rate_ms", "3001", NULL) == -EINVAL);
	assert(!config_set(&c, "offhook_rate_ms", "200", NULL) && c.offhook_rate_ms == 200);
	assert(config_set(&c, "offhook_rate_ms", "100", NULL) == -EINVAL);
	assert(config_set(&c, "offhook_rate_ms", "10001", NULL) == -EINVAL);

	fclose(log);
	fclose(led);
	printf("indicator: all tests passed\n");
	return 0;
}
