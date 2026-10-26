/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONE_CORE_CALL_H
#define AW_PHONE_CORE_CALL_H

#include <stdint.h>

#include "phone.h"

/*
 * Internal to the core and the stage tool (the daemon uses phone.h only): the watch without the line bring-up, so that
 * the stage tool's stages 6 and 7 run the same hook machine and watch loop as the service, on a line they set up
 * themselves.
 */

/* A watch on a fed channel: no chip access, nothing is brought up. The channel is `ch`; board / call / cfg / hooks may be NULL. */
struct phone *phone_attach(struct chip *c, unsigned int ch, const struct board *b, const struct call_ops *call,
	const struct phone_cfg *cfg, const struct phone_hooks *hooks, FILE *log);

/* One hook register sample into the hook machine (the stage 6 hold reads the register itself). */
void phone_hook_feed(struct phone *p, uint8_t lcr);

/* The end of a watch: the ring stops when it is on and the summary line is logged. Returns err, or -EIO when the ring would not stop. */
int phone_watch_end(struct phone *p, int err);

void phone_free(struct phone *p);

/*
 * The sample: the line feed state, the loop current and the battery, logged every `log_every` samples. 0, or the error
 * that must stop the hold. The readings are returned through the optional pointers.
 */
int observe(struct chip *c, unsigned int ch, unsigned int sample, unsigned int log_every, FILE *log, int *battery_mv, int *loop_ua);

#endif /* AW_PHONE_CORE_CALL_H */
