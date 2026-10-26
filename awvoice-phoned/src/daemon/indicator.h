/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_INDICATOR_H
#define AW_PHONED_INDICATOR_H

#include <stddef.h>
#include <stdio.h>

struct phone_status;

/*
 * The phone indicator LED (this router's `green:phone`), driven by the daemon through /sys/class/leds, as the ATA
 * practice has it: solid while the line is ready for calls, a fast blink
 * while an incoming call rings, a slow blink while the handset is off hook, dark otherwise - dark also when the
 * service stops. Ready is the page's readiness rule (the health reply's): the modem answers and both registrations
 * hold. Taking an LED switches its trigger off; the rates are the page's settings (awvoice.main.ring_rate_ms,
 * offhook_rate_ms).
 */
enum ind_state { IND_OFF, IND_READY, IND_RING, IND_OFFHOOK };

struct indicator {
	char led[64];			/* the name under /sys/class/leds, "" = none */
	int fd;				/* the brightness file, -1 while none */
	int broken;			/* a write failed: nothing is written again until the LED changes */
	int foreign_warned;		/* the once-per-run note about another writer of the LED */
	unsigned int ring_rate_ms, offhook_rate_ms;
	enum ind_state state;
	int lit;			/* the level written last, -1 = unknown */
	unsigned long long flip_ms;	/* the next blink edge */
	unsigned long long verify_ms;	/* the next read-back of the level */
};

/* dark and unopened */
void indicator_init(struct indicator *ind);

/*
 * Take the named LED with the two rates (a reload calls this too: only a change acts). An LED that cannot be opened
 * or driven is logged once and left dark; the setting itself stays so the page keeps showing it.
 */
void indicator_apply(struct indicator *ind, const char *led, unsigned int ring_rate_ms, unsigned int offhook_rate_ms, FILE *log);

/* one step of the loop: the pattern state machine; writes happen only on an edge */
void indicator_step(struct indicator *ind, const struct phone_status *st, unsigned long long now_ms);

/* the LED dark and the file closed (service stop) */
void indicator_close(struct indicator *ind);

/* the state the LED shows: off, ready, ring, offhook */
const char *indicator_state_name(const struct indicator *ind);

/* the LED names of the router (a directory read of /sys/class/leds) into names[0..max), each string inside buf;
 * returns the count, or a negative errno */
int indicator_scan_leds(char *buf, size_t cap, const char *names[], unsigned int max);

#endif
