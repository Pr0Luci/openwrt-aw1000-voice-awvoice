// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * Audio bridge between the modem and the line chip.
 *
 * Both devices are slaves of the SoC's clocks and both send on the SoC's receive line and listen on its transmit
 * line, so they cannot hear each other; this thread copies samples across.
 *
 * The PCM block hands over one 16-bit word per enabled slot and frame, each tagged with its slot number. A frame
 * of k words is kept as an array; a sample is a run of 16 bits that starts at a bit position counted from the
 * first bit of word 0 (most significant first). The modem's sample starts one bit clock after a slot boundary
 * ), so samples are cut from and pasted into the array at bit positions, not at slot boundaries.
 */
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/sched.h>

#include "awfxs.h"

#define MAX_SLOTS		12	/* the wideband frame reaches bit 175 (slot 10); one spare word for a straddling sample */
#define PRIME_FRAMES		24	/* reply frames queued before the block starts: 3 ms, above the 2.5 ms service gap allows (20 frames) */
#define FIFO_DEPTH_LIMIT	192	/* words */
#define IDLE_WORD		0xffff
#define SETTLE_VISITS		10	/* first visits after the start: error flags raised by the start itself are cleared, not counted */

struct rx_frame {
	u16 word[MAX_SLOTS + 1];	/* one spare word so a sample may straddle the last slot boundary */
	unsigned int next;		/* slot expected next */
};

/* the 16 bits that start at bit position pos */
static u16 cut(const u16 *word, unsigned int pos)
{
	unsigned int idx = pos / 16, skip = pos % 16;
	u32 pair = ((u32)word[idx] << 16) | word[idx + 1];

	return (pair >> (16 - skip)) & 0xffff;
}

static void paste(u16 *word, unsigned int pos, u16 value)
{
	unsigned int idx = pos / 16, skip = pos % 16;
	u32 pair = ((u32)word[idx] << 16) | word[idx + 1];
	u32 hole = 0xffffu << (16 - skip);

	pair = (pair & ~hole) | ((u32)value << (16 - skip));
	word[idx] = pair >> 16;
	word[idx + 1] = pair & 0xffff;
}

static u32 level_of(u16 sample)
{
	int v = (s16)sample;

	return v < 0 ? -v : v;
}

/* collect words in slot order; true when the last slot of a frame has arrived */
static bool frame_complete(struct rx_frame *f, u32 raw, unsigned int slots)
{
	unsigned int slot = (raw >> 16) & 0x1f;

	if (slot >= slots)
		return false;
	if (slot != f->next) {
		f->next = 0;		/* words were lost: wait for the next frame start */
		if (slot)
			return false;
	}
	f->word[slot] = raw & 0xffff;
	if (++f->next < slots)
		return false;
	f->next = 0;
	return true;
}

static void queue_words(struct awfxs *z, const u16 *word, unsigned int slots)
{
	unsigned int slot;

	for (slot = 0; slot < slots; slot++)
		writel(((u32)slot << 16) | word[slot], &z->pcm->tx16);
}

static void exchange_samples(struct awfxs *z, const struct rx_frame *in)
{
	const struct awfxs_bridge *map = &z->bridge;
	struct awfxs_bridge_stats *st = &z->bridge_stats;
	u16 reply[MAX_SLOTS + 1];
	u16 from_modem = cut(in->word, map->modem_rx);
	u32 window_peak;

	memset16(reply, IDLE_WORD, ARRAY_SIZE(reply));
	st->peak_modem = max(st->peak_modem, level_of(from_modem));
	if (level_window_feed(&z->modem_window, level_of(from_modem), AWFXS_WINDOW_FRAMES, &window_peak)) {
		st->modem_window_peak = window_peak;
		st->modem_windows++;
	}

	if (map->mode == AWFXS_BRIDGE_CROSS) {
		u16 from_line = cut(in->word, map->line_rx);

		st->peak_line = max(st->peak_line, level_of(from_line));
		paste(reply, map->modem_tx, from_line);
		paste(reply, map->line_tx, from_modem);
		if (map->flags & AWFXS_BRIDGE_WIDEBAND) {
			/* the second sample sits half a frame (128 bits) after the first on both sides: crossed the same way */
			paste(reply, map->modem_tx + 128, cut(in->word, map->line_rx + 128));
			paste(reply, map->line_tx + 128, cut(in->word, map->modem_rx + 128));
		}
	} else {
		paste(reply, map->modem_tx, from_modem);
		if (map->mode == AWFXS_BRIDGE_LOOP && (map->flags & AWFXS_BRIDGE_WIDEBAND))
			paste(reply, map->modem_tx + 128, cut(in->word, map->modem_rx + 128));
	}
	queue_words(z, reply, z->bridge_slots);
	st->frames++;
}

static void note_gap(struct awfxs_bridge_stats *st, u64 gap_ns)
{
	static const u32 edge_us[AWFXS_GAP_BUCKETS - 1] = { 1100, 1250, 1500, 2000, 2500 };
	u32 us = div_u64(gap_ns, 1000);
	unsigned int bucket = 0;

	while (bucket < ARRAY_SIZE(edge_us) && us >= edge_us[bucket])
		bucket++;
	st->gap_hist[bucket]++;
	st->max_gap_us = max(st->max_gap_us, us);
}

/* one visit to the FIFOs: clear error flags, take what has arrived, answer every complete frame */
static void service_fifos(struct awfxs *z, struct rx_frame *f, bool settling)
{
	struct awfxs_bridge_stats *st = &z->bridge_stats;
	u32 flags = readl(&z->pcm->irq_status) & AW_PCM_IRQ_ERRORS;
	u32 level = readl(&z->pcm->fifo_count);
	u32 waiting = min_t(u32, AW_PCM_RX_COUNT(level), FIFO_DEPTH_LIMIT);
	u32 queued = AW_PCM_TX_COUNT(level);

	if (flags) {
		st->resyncs += !settling;
		writel(flags, &z->pcm->irq_status);
	}
	for (; waiting; waiting--) {
		if (!frame_complete(f, readl(&z->pcm->rx16), z->bridge_slots))
			continue;
		if (queued + z->bridge_slots > FIFO_DEPTH_LIMIT) {
			st->tx_skipped++;
			continue;
		}
		exchange_samples(z, f);
		queued += z->bridge_slots;
	}
}

static int bridge_loop(void *arg)
{
	struct awfxs *z = arg;
	struct rx_frame frame = { .next = 0 };
	u64 previous = ktime_get_ns(), now;
	unsigned int settle = SETTLE_VISITS;

	sched_set_fifo_low(current);
	do {
		usleep_range(900, 1100);
		now = ktime_get_ns();
		note_gap(&z->bridge_stats, now - previous);
		previous = now;
		service_fifos(z, &frame, settle > 0);
		settle -= settle > 0;
	} while (!kthread_should_stop());
	return 0;
}

void awfxs_bridge_stop(struct awfxs *z)
{
	if (!z->bridge_task)
		return;
	kthread_stop(z->bridge_task);
	z->bridge_task = NULL;
	z->bridge_stats.running = 0;
}

/* how many slots the four positions need (and the second samples when wideband), and whether the PCM block is set up for them */
static int slots_needed(struct awfxs *z, const struct awfxs_bridge *b, unsigned int *slots)
{
	unsigned int last = max(b->modem_rx, b->modem_tx);
	unsigned int span = b->flags & AWFXS_BRIDGE_WIDEBAND ? 128 : 0;

	if (b->flags & ~AWFXS_BRIDGE_WIDEBAND)
		return -EINVAL;
	if (b->mode == AWFXS_BRIDGE_CROSS)
		last = max3(last, b->line_rx, b->line_tx);
	else if (b->mode != AWFXS_BRIDGE_LOOP)
		return -EINVAL;
	if (max(max(b->modem_rx, b->modem_tx), max(b->line_rx, b->line_tx)) > AWFXS_BRIDGE_POS_MAX)
		return -EINVAL;
	*slots = DIV_ROUND_UP(last + span + 16, 16);

	if (!(z->state & AWFXS_STATE_PCM_RUN) || !(readl(&z->pcm->ctrl) & AW_PCM_CPU_MODE))
		return -ENODEV;
	/* only the slots the frames are made of may be enabled, otherwise they cannot be told apart */
	return readl(&z->pcm->slotmask) == GENMASK(*slots - 1, 0) ? 0 : -EINVAL;
}

/* empty receive FIFO, queued idle replies, then start: every reply needs a frame to answer */
static int restart_block(struct awfxs *z, unsigned int slots)
{
	unsigned int i;
	u16 idle[MAX_SLOTS];

	memset16(idle, IDLE_WORD, ARRAY_SIZE(idle));
	writel(0, &z->pcm->start);
	usleep_range(250, 500);
	for (i = 0; i < 2 * FIFO_DEPTH_LIMIT && AW_PCM_RX_COUNT(readl(&z->pcm->fifo_count)); i++)
		(void)readl(&z->pcm->rx16);
	if (AW_PCM_TX_COUNT(readl(&z->pcm->fifo_count)))
		return -EAGAIN;		/* words left from an earlier run: bring the PCM block up again first */

	for (i = 0; i < PRIME_FRAMES; i++)
		queue_words(z, idle, slots);
	writel(AW_PCM_IRQ_ERRORS, &z->pcm->irq_status);
	writel(1, &z->pcm->start);
	return 0;
}

int awfxs_bridge_set(struct awfxs *z, const struct awfxs_bridge *b)
{
	unsigned int slots;
	int err;

	if (b->mode == AWFXS_BRIDGE_OFF) {
		awfxs_bridge_stop(z);
		return 0;
	}
	if (z->bridge_task)
		return -EBUSY;
	err = slots_needed(z, b, &slots);
	if (!err)
		err = restart_block(z, slots);
	if (err)
		return err;

	z->bridge = *b;
	z->bridge_slots = slots;
	memset(&z->bridge_stats, 0, sizeof(z->bridge_stats));
	memset(&z->modem_window, 0, sizeof(z->modem_window));
	z->bridge_stats.slots = slots;
	z->bridge_stats.running = 1;
	z->bridge_task = kthread_run(bridge_loop, z, "awvoice-fxs-bridge");
	if (IS_ERR(z->bridge_task)) {
		err = PTR_ERR(z->bridge_task);
		z->bridge_task = NULL;
		z->bridge_stats.running = 0;
		return err;
	}
	return 0;
}

void awfxs_bridge_stats(struct awfxs *z, struct awfxs_bridge_stats *s)
{
	*s = z->bridge_stats;
	s->running = z->bridge_task != NULL;
}
