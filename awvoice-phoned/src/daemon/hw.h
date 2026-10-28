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

/* the kernel bridge's capture tap (uapi AWFXS_IOC_TAP_*): arm or disarm it, then drain frames while it runs.
 * dev_tap_read fills `meta` always (generation, wideband, dropped) and returns the frames copied (0 when the ring is empty). */
int dev_tap_set(struct device *d, int on);	/* 0 or -errno; arming needs a running bridge */
unsigned int dev_tap_read(struct device *d, struct awfxs_tap_frame *frames, unsigned int max, struct awfxs_tap_read *meta);

/* the kernel bridge's playback ring (uapi AWFXS_IOC_PLAY_*): while armed the caller's side hears the frames written
 * here instead of the bridge's own; an empty ring is silence. dev_play_write fills `meta` (written, dropped,
 * generation) whether the write succeeded or not. */
int dev_play_set(struct device *d, int on);	/* 0 or -errno; arming needs a running bridge */
int dev_play_write(struct device *d, const struct awfxs_play_frame *frames, unsigned int count, struct awfxs_play_write *meta);
int dev_play_status(struct device *d, struct awfxs_play_status *out);	/* 0 or -errno; fills `out` always */

#endif /* AW_PHONED_HW_H */
