/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_HW_H
#define AW_PHONED_HW_H

#include "chip.h"

#include <linux/awvoice-fxs.h>

/* The chip behind /dev/awvoice-fxs0 (kernel module kmod-awvoice-fxs). */
struct device {
	int fd;
	unsigned long long fed_ms;	/* when the line watchdog was last restarted */
};

extern const struct chip_ops dev_ops;

int dev_open(struct device *d);		/* 0 or -errno */
int dev_prepare(struct device *d, int wideband);	/* clocks running, chip reset with the clocks present, watchdog armed */
void dev_close(struct device *d);	/* closing holds the chip in reset */
int dev_bridge(struct device *d, const struct awfxs_bridge *b);	/* 0 or -errno */

/* api_ctx.bridge_stats for ctx = struct device *: fills a struct awfxs_bridge_stats; 0 or -errno */
int dev_bridge_stats(void *ctx, void *out);

/* phone_hooks.bridge_start for ctx = struct device *: the cross bridge between the modem's and the line's PCM slots */
int dev_bridge_start(void *ctx, unsigned int modem_rx, unsigned int modem_tx, unsigned int line_rx, unsigned int line_tx, unsigned int flags);

#endif /* AW_PHONED_HW_H */
