/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "indicator.h"
#include "phone.h"

#define LEDS_ROOT	"/sys/class/leds"

void indicator_init(struct indicator *ind)
{
	*ind = (struct indicator){ .fd = -1, .lit = -1, .ring_rate_ms = 300, .offhook_rate_ms = 1000 };
}

/* one write into a file of the LED's sysfs directory; 0, or -errno */
static int led_write(const char *led, const char *file, const char *text)
{
	char path[sizeof(LEDS_ROOT) + 64 + 16];
	int fd, err;

	snprintf(path, sizeof(path), LEDS_ROOT "/%s/%s", led, file);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -errno;
	err = write(fd, text, strlen(text)) == (ssize_t)strlen(text) ? 0 : -errno;
	close(fd);
	return err;
}

static int led_open_brt(const char *led)
{
	char path[sizeof(LEDS_ROOT) + 64 + 16];
	int fd;

	snprintf(path, sizeof(path), LEDS_ROOT "/%s/brightness", led);
	fd = open(path, O_RDWR);	/* read too: the level is verified against another writer */
	return fd >= 0 ? fd : -errno;
}

/* the level on the line, written only when it changes; a failed write leaves the LED dark until the setting changes */
static void led_level(struct indicator *ind, int lit)
{
	if (!ind->led[0] || ind->broken || ind->fd < 0 || ind->lit == lit)
		return;
	if (pwrite(ind->fd, lit ? "1" : "0", 1, 0) != 1) {
		ind->broken = 1;		/* the LED went away (a module, a rename) */
		printf("the indicator LED %s cannot be driven: it stays dark\n", ind->led);
		return;
	}
	ind->lit = lit;
}

void indicator_apply(struct indicator *ind, const char *led, unsigned int ring_rate_ms, unsigned int offhook_rate_ms, FILE *log)
{
	if (ring_rate_ms)
		ind->ring_rate_ms = ring_rate_ms;
	if (offhook_rate_ms)
		ind->offhook_rate_ms = offhook_rate_ms;
	if (!led || !strcmp(led, ind->led))
		return;

	indicator_close(ind);
	snprintf(ind->led, sizeof(ind->led), "%s", led[0] ? led : "");
	if (!ind->led[0])
		return;
	int err = led_write(ind->led, "trigger", "none");
	int fd = err ? -1 : led_open_brt(ind->led);

	if (err || fd < 0) {
		ind->led[0] = 0;
		fprintf(log, "the indicator LED %s cannot be taken (%s): no indicator\n", led, strerror(err ? err : fd));
		return;
	}
	ind->fd = fd;
	led_level(ind, 0);
	fprintf(log, "the indicator LED is %s: solid when ready, a fast blink for a call, a slow blink off hook\n", ind->led);
}

#define LED_VERIFY_MS	5	/* the written level is read back every step and restored when another writer changed it */

/* the level is read back and restored when another program wrote it (the earlier LED service keeps its
 * overlay for the old voice stack): the daemon's level wins within one step of that writer's write */
static void led_verify(struct indicator *ind, unsigned long long now_ms)
{
	char cur;

	if (ind->fd < 0 || ind->broken || ind->lit < 0 || (long long)(now_ms - ind->verify_ms) < 0)
		return;
	ind->verify_ms = now_ms + LED_VERIFY_MS;
	if (pread(ind->fd, &cur, 1, 0) != 1 || cur == (ind->lit ? '1' : '0'))
		return;
	if (pwrite(ind->fd, ind->lit ? "1" : "0", 1, 0) == 1 && !ind->foreign_warned) {
		ind->foreign_warned = 1;
		printf("the indicator LED is written by another program too: the level here is restored\n");
	}
}

void indicator_step(struct indicator *ind, const struct phone_status *st, unsigned long long now_ms)
{
	const int ready = st->modem_up && st->registered == 1 && st->ims_registered == 1;
	const enum ind_state new_state = st->line == PHONE_LINE_RINGING ? IND_RING
		: st->off_hook ? IND_OFFHOOK
		: ready ? IND_READY : IND_OFF;

	led_verify(ind, now_ms);
	if (new_state != ind->state) {
		ind->state = new_state;
		ind->lit = -1;				/* the pattern restarts from its lit half */
	}
	if (ind->state == IND_RING || ind->state == IND_OFFHOOK) {
		unsigned int half = (ind->state == IND_RING ? ind->ring_rate_ms : ind->offhook_rate_ms) / 2u;

		if (ind->lit < 0) {			/* led_level keeps ind->lit: never set it here first */
			ind->flip_ms = now_ms + half;
			led_level(ind, 1);
		} else if ((long long)(now_ms - ind->flip_ms) >= 0) {
			led_level(ind, !ind->lit);
			ind->flip_ms = now_ms + half;
		}
	} else {
		led_level(ind, ind->state == IND_READY);
	}
}

void indicator_close(struct indicator *ind)
{
	if (ind->fd >= 0) {
		if (pwrite(ind->fd, "0", 1, 0) != 1)
			printf("the indicator LED %s stayed lit at the stop\n", ind->led);
		close(ind->fd);
	}
	ind->fd = -1;
	ind->lit = -1;
	ind->state = IND_OFF;
	/* the name stays: the page keeps showing the setting, a later apply reopens the LED */
}

const char *indicator_state_name(const struct indicator *ind)
{
	return (const char *const[]){ "off", "ready", "ring", "offhook" }[ind->state];
}

int indicator_scan_leds(char *buf, size_t cap, const char *names[], unsigned int max)
{
	DIR *dir = opendir(LEDS_ROOT);
	struct dirent *de;
	size_t used = 0;
	unsigned int n = 0;

	if (!dir)
		return -errno;
	while (n < max && (de = readdir(dir))) {
		size_t len;

		if (de->d_name[0] == '.')
			continue;
		len = strlen(de->d_name) + 1;
		if (used + len > cap)
			break;
		memcpy(buf + used, de->d_name, len);
		names[n++] = buf + used;
		used += len;
	}
	closedir(dir);
	return (int)n;
}
