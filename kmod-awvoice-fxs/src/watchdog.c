// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * Line watchdog: whoever drives the line (the daemon) must keep restarting the timer. If it stops, or the
 * device is closed, the chip is held in reset, which removes line voltage and ringing within the time of
 * a timer expiry or immediately on close. Nothing here sleeps, so the same code runs in the timer and in
 * process context.
 */
#include <linux/gpio/consumer.h>
#include <linux/jiffies.h>
#include <linux/printk.h>

#include "awfxs.h"

static void hold_in_reset(struct awfxs *z)
{
	if (z->reset)
		gpiod_set_raw_value(z->reset, 0);
}

static void expired(struct timer_list *t)
{
	struct awfxs *z = from_timer(z, t, wd_timer);

	hold_in_reset(z);
	clear_bit(AW_WD_ARMED, &z->wd_flags);
	set_bit(AW_WD_TRIPPED, &z->wd_flags);
	atomic_inc(&z->wd_trips);
	pr_warn("awvoice_fxs: line watchdog expired, chip held in reset\n");
}

void awfxs_wd_init(struct awfxs *z)
{
	timer_setup(&z->wd_timer, expired, 0);
}

int awfxs_wd_arm(struct awfxs *z, u32 ms)
{
	if (!(z->state & AWFXS_STATE_GPIO_OK))
		return -ENODEV;
	if (!ms) {
		timer_delete_sync(&z->wd_timer);
		clear_bit(AW_WD_ARMED, &z->wd_flags);
		return 0;
	}
	if (ms < AWFXS_WD_MIN_MS || ms > AWFXS_WD_MAX_MS)
		return -EINVAL;
	/* after an expiry the chip is in reset: it has to be reset and set up again before it is watched again */
	if (test_bit(AW_WD_TRIPPED, &z->wd_flags))
		return -EAGAIN;
	set_bit(AW_WD_ARMED, &z->wd_flags);
	mod_timer(&z->wd_timer, jiffies + msecs_to_jiffies(ms));
	return 0;
}

/* closing the device, or unloading the module, leaves the line in the safe state */
void awfxs_wd_hold_safe(struct awfxs *z)
{
	timer_delete_sync(&z->wd_timer);
	clear_bit(AW_WD_ARMED, &z->wd_flags);
	if (z->state & AWFXS_STATE_GPIO_OK) {
		hold_in_reset(z);
		set_bit(AW_WD_TRIPPED, &z->wd_flags);
	}
}

void awfxs_wd_exit(struct awfxs *z)
{
	timer_delete_sync(&z->wd_timer);
}

u32 awfxs_wd_state(struct awfxs *z)
{
	return (test_bit(AW_WD_ARMED, &z->wd_flags) ? AWFXS_STATE_WD_ARMED : 0) |
	       (test_bit(AW_WD_TRIPPED, &z->wd_flags) ? AWFXS_STATE_WD_TRIPPED : 0);
}
