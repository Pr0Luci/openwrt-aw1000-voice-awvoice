// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * /dev/awvoice-fxs0: access to the line interface chip of the phone port.
 *
 * Loading the module only claims the chip pins (bus idle, chip held in reset); the clocks stay off until
 * the PCM up call. Commands are listed in a table and share one argument union: the dispatcher copies the
 * argument in and out according to the direction bits of the command number. One opener at a time.
 */
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "awfxs.h"

#define SNAP_WORDS_MAX		65536
#define SNAP_TIME_MAX_US	50000
#define TAP_READ_MAX_FRAMES	AWFXS_TAP_FRAMES	/* one read cannot need more than the ring holds */

static struct awfxs aw;
static unsigned long opened;

union aw_arg {
	struct awfxs_info info;
	struct awfxs_regs regs;
	struct awfxs_pcm_up up;
	struct awfxs_spi spi;
	struct awfxs_snap snap;
	struct awfxs_bridge bridge;
	struct awfxs_bridge_stats bstats;
	struct awfxs_tap_read tap_read;
	u32 scalar;
};

static int cmd_info(struct awfxs *z, union aw_arg *a)
{
	a->info.abi = AWFXS_ABI;
	a->info.state = z->state | awfxs_wd_state(z);
	a->info.wd_trips = atomic_read(&z->wd_trips);
	a->info.irq_level = (z->state & AWFXS_STATE_GPIO_OK) ? gpiod_get_raw_value(z->irq) : 0;
	return 0;
}

static int cmd_regs(struct awfxs *z, union aw_arg *a)
{
	awfxs_regs(z, &a->regs);
	return 0;
}

static int cmd_pcm_up(struct awfxs *z, union aw_arg *a)
{
	return awfxs_pcm_up(z, &a->up);
}

static int cmd_pcm_down(struct awfxs *z, union aw_arg *a)
{
	awfxs_pcm_down(z);
	return 0;
}

static int cmd_chip_reset(struct awfxs *z, union aw_arg *a)
{
	return awfxs_chip_reset(z, a->scalar);
}

static int cmd_spi(struct awfxs *z, union aw_arg *a)
{
	return awfxs_spi(z, &a->spi);
}

static int cmd_ring_arm(struct awfxs *z, union aw_arg *a)
{
	return awfxs_wd_arm(z, a->scalar);
}

static int cmd_bridge(struct awfxs *z, union aw_arg *a)
{
	return awfxs_bridge_set(z, &a->bridge);
}

static int cmd_bridge_stats(struct awfxs *z, union aw_arg *a)
{
	awfxs_bridge_stats(z, &a->bstats);
	return 0;
}

static int cmd_tap_set(struct awfxs *z, union aw_arg *a)
{
	return awfxs_tap_set(z, a->scalar);
}

static int cmd_tap_read(struct awfxs *z, union aw_arg *a)
{
	struct awfxs_tap_read *r = &a->tap_read;
	struct awfxs_tap_frame *frames;
	int err;

	if (!r->max_frames || r->max_frames > TAP_READ_MAX_FRAMES)
		return -EINVAL;
	frames = vmalloc(array_size(r->max_frames, sizeof(*frames)));
	if (!frames)
		return -ENOMEM;
	awfxs_tap_fill_read(z, r, frames);
	if (r->got && copy_to_user(u64_to_user_ptr(r->buf), frames, array_size(r->got, sizeof(*frames)))) {
		r->got = 0;
		err = -EFAULT;
	} else {
		err = 0;
	}
	vfree(frames);
	return err;
}

static int cmd_snap(struct awfxs *z, union aw_arg *a)
{
	struct awfxs_snap *s = &a->snap;
	u32 *buf;
	int err;

	if (!s->max_words || s->max_words > SNAP_WORDS_MAX || !s->timeout_us || s->timeout_us > SNAP_TIME_MAX_US)
		return -EINVAL;
	buf = vmalloc(array_size(s->max_words, sizeof(*buf)));
	if (!buf)
		return -ENOMEM;
	err = awfxs_snap(z, s, buf);
	if (!err && copy_to_user(u64_to_user_ptr(s->buf), buf, array_size(s->got, sizeof(*buf))))
		err = -EFAULT;
	vfree(buf);
	return err;
}

static const struct {
	unsigned int cmd;
	int (*run)(struct awfxs *z, union aw_arg *a);
} commands[] = {
	{ AWFXS_IOC_INFO,       cmd_info },
	{ AWFXS_IOC_REGS,       cmd_regs },
	{ AWFXS_IOC_PCM_UP,     cmd_pcm_up },
	{ AWFXS_IOC_PCM_DOWN,   cmd_pcm_down },
	{ AWFXS_IOC_CHIP_RESET, cmd_chip_reset },
	{ AWFXS_IOC_SPI,        cmd_spi },
	{ AWFXS_IOC_SNAP,       cmd_snap },
	{ AWFXS_IOC_RING_ARM,   cmd_ring_arm },
	{ AWFXS_IOC_BRIDGE,     cmd_bridge },
	{ AWFXS_IOC_BRIDGE_STATS, cmd_bridge_stats },
	{ AWFXS_IOC_TAP_SET,    cmd_tap_set },
	{ AWFXS_IOC_TAP_READ,   cmd_tap_read },
};

static long awfxs_ioctl(struct file *file, unsigned int cmd, unsigned long user)
{
	union aw_arg arg;
	size_t i, size = _IOC_SIZE(cmd);
	int err;

	for (i = 0; i < ARRAY_SIZE(commands) && commands[i].cmd != cmd; i++)
		;
	if (i == ARRAY_SIZE(commands))
		return -ENOTTY;

	memset(&arg, 0, sizeof(arg));
	if ((_IOC_DIR(cmd) & _IOC_WRITE) && copy_from_user(&arg, (void __user *)user, size))
		return -EFAULT;

	mutex_lock(&aw.lock);
	err = commands[i].run(&aw, &arg);
	mutex_unlock(&aw.lock);

	/* results are returned even when the command failed half way: the bring-up call reports why */
	if ((_IOC_DIR(cmd) & _IOC_READ) && copy_to_user((void __user *)user, &arg, size) && !err)
		err = -EFAULT;
	return err;
}

static int awfxs_open(struct inode *inode, struct file *file)
{
	return test_and_set_bit(0, &opened) ? -EBUSY : 0;
}

static int awfxs_release(struct inode *inode, struct file *file)
{
	mutex_lock(&aw.lock);
	awfxs_bridge_stop(&aw);
	awfxs_wd_hold_safe(&aw);
	mutex_unlock(&aw.lock);
	clear_bit(0, &opened);
	return 0;
}

static const struct file_operations awfxs_fops = {
	.owner		= THIS_MODULE,
	.open		= awfxs_open,
	.release	= awfxs_release,
	.unlocked_ioctl	= awfxs_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct miscdevice awfxs_misc;

static void describe_node(void)
{
	awfxs_misc.name = "awvoice-fxs0";
	awfxs_misc.minor = MISC_DYNAMIC_MINOR;
	awfxs_misc.mode = 0600;
	awfxs_misc.fops = &awfxs_fops;
}

static int __init awfxs_init(void)
{
	int err;

	describe_node();
	mutex_init(&aw.lock);
	awfxs_wd_init(&aw);
	err = awfxs_hw_map(&aw);
	if (err)
		return err;
	err = awfxs_gpio_get(&aw);
	if (!err)
		err = misc_register(&awfxs_misc);
	if (err) {
		awfxs_gpio_put(&aw);
		awfxs_hw_unmap(&aw);
	}
	return err;
}

static void __exit awfxs_exit(void)
{
	misc_deregister(&awfxs_misc);
	awfxs_wd_hold_safe(&aw);
	awfxs_wd_exit(&aw);
	awfxs_hw_release(&aw);
	awfxs_gpio_put(&aw);
	awfxs_hw_unmap(&aw);
}

module_init(awfxs_init);
module_exit(awfxs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Line interface chip access for the phone port");
MODULE_AUTHOR("Pr0Luci");
