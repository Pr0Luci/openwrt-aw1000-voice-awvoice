// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/awvoice-fxs.h>

#include "hw.h"

#define WATCHDOG_MS	3000
#define PCM_CTRL	0x31000002u	/* host-FIFO mode, 16-bit slots, receive phase 2 */
#define PCM_SLOTS	0x7u	/* the modem's sample in slot 0 and the chip's at bits 32..47 (slot 2); slot 1 stays idle */
#define RESET_LOW_MS	10

#define FEED_EVERY_MS	500	/* long runs of accesses without a pause still restart the watchdog */

static unsigned long long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long long)t.tv_sec * 1000u + (unsigned long long)t.tv_nsec / 1000000u;
}

static int keep_alive(struct device *d)
{
	__u32 ms = WATCHDOG_MS;

	d->fed_ms = now_ms();
	return ioctl(d->fd, AWFXS_IOC_RING_ARM, &ms) ? -errno : 0;
}

static int frame(void *ctx, const uint8_t tx[4], uint8_t rx[4])
{
	struct awfxs_spi x = { .len = AWFXS_SPI_FRAME };
	struct device *d = ctx;
	int err = now_ms() - d->fed_ms >= FEED_EVERY_MS ? keep_alive(d) : 0;

	if (err)
		return err;
	memcpy(x.tx, tx, 4);
	if (ioctl(d->fd, AWFXS_IOC_SPI, &x))
		return -errno;
	memcpy(rx, x.rx, 4);
	return 0;
}

static void pause_ms(void *ctx, unsigned int ms)
{
	struct device *d = ctx;

	if (now_ms() - d->fed_ms >= FEED_EVERY_MS)	/* the hook is read every 5 ms: the watchdog is not restarted that often */
		keep_alive(d);
	usleep(ms * 1000u);
}

static unsigned long long clock_ms(void *ctx)
{
	(void)ctx;
	return now_ms();
}

const struct chip_ops dev_ops = { .frame = frame, .pause_ms = pause_ms, .now_ms = clock_ms };

int dev_open(struct device *d)
{
	d->fed_ms = 0;
	d->fd = open("/dev/awvoice-fxs0", O_RDWR);
	return d->fd < 0 ? -errno : 0;
}

int dev_prepare(struct device *d, int wideband)
{
	/* the wideband frame reaches bit 175: the slots 0..10 of the 16 the 256-bit frame holds */
	struct awfxs_pcm_up up = { .cfg = { .ctrl = PCM_CTRL, .bitmap = wideband ? 0x7ffu : PCM_SLOTS, .divider = 1 } };
	__u32 reset_ms = RESET_LOW_MS;

	if (ioctl(d->fd, AWFXS_IOC_PCM_UP, &up) || ioctl(d->fd, AWFXS_IOC_CHIP_RESET, &reset_ms))
		return -errno;
	return keep_alive(d);
}

void dev_close(struct device *d)
{
	close(d->fd);
}

int dev_bridge(struct device *d, const struct awfxs_bridge *b)
{
	return ioctl(d->fd, AWFXS_IOC_BRIDGE, (struct awfxs_bridge *)b) ? -errno : 0;
}

int dev_bridge_start(void *ctx, unsigned int modem_rx, unsigned int modem_tx, unsigned int line_rx, unsigned int line_tx, unsigned int flags)
{
	struct awfxs_bridge b = { .mode = AWFXS_BRIDGE_CROSS,
		.modem_rx = modem_rx, .modem_tx = modem_tx, .line_rx = line_rx, .line_tx = line_tx, .flags = flags };
	int err = dev_bridge(ctx, &b);

	if (err == -EBUSY) {
		/* a bridge is already running (an HD switch changed its flags): stop it and start again */
		b.mode = AWFXS_BRIDGE_OFF;
		if (dev_bridge(ctx, &b))
			return -EIO;
		b = (struct awfxs_bridge){ .mode = AWFXS_BRIDGE_CROSS,
			.modem_rx = modem_rx, .modem_tx = modem_tx, .line_rx = line_rx, .line_tx = line_tx, .flags = flags };
		err = dev_bridge(ctx, &b);
	}
	return err;
}

int dev_bridge_stats(void *ctx, void *out)
{
	struct device *d = ctx;

	return ioctl(d->fd, AWFXS_IOC_BRIDGE_STATS, (struct awfxs_bridge_stats *)out) ? -errno : 0;
}

int dev_tap_set(struct device *d, int on)
{
	return ioctl(d->fd, AWFXS_IOC_TAP_SET, &on) ? -errno : 0;
}

unsigned int dev_tap_read(struct device *d, struct awfxs_tap_frame *frames, unsigned int max, struct awfxs_tap_read *meta)
{
	memset(meta, 0, sizeof(*meta));
	meta->buf = (unsigned long long)(unsigned long)frames;	/* this process's own buffer: the kernel copies into it */
	meta->max_frames = max;
	if (ioctl(d->fd, AWFXS_IOC_TAP_READ, meta))
		return 0;
	return meta->got;
}

int dev_play_set(struct device *d, int on)
{
	return ioctl(d->fd, AWFXS_IOC_PLAY_SET, &on) ? -errno : 0;
}

int dev_play_write(struct device *d, const struct awfxs_play_frame *frames, unsigned int count, struct awfxs_play_write *meta)
{
	memset(meta, 0, sizeof(*meta));
	meta->buf = (unsigned long long)(unsigned long)frames;	/* this process's own buffer: the kernel copies from it */
	meta->frames = count;
	return ioctl(d->fd, AWFXS_IOC_PLAY_WRITE, meta) ? -errno : 0;
}

int dev_play_status(struct device *d, struct awfxs_play_status *out)
{
	memset(out, 0, sizeof(*out));
	return ioctl(d->fd, AWFXS_IOC_PLAY_READ, out) ? -errno : 0;
}
