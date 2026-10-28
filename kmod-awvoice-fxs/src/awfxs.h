/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AWFXS_H
#define AWFXS_H

#include <linux/bits.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/awvoice-fxs.h>

#include "window.h"
#include "tap_ring.h"
#include "play_ring.h"

#define AWFXS_TAP_FRAMES	8192	/* capture ring capacity in frames, power of two: about one second of narrowband audio */
#define AWFXS_PLAY_FRAMES	4096	/* playback ring capacity in frames, power of two: 512 ms at 8 kHz, 256 ms at 16 kHz */

/*
 * Register blocks are described as overlay structures; a pointer to the first byte of the block is cast
 * to the structure and fields are accessed with readl()/writel() on their addresses.
 * Every physical address this module maps is named in this file. The flash controller's pin (14) is not
 * in any table here and must never be added.
 */

/* clock controller */
#define AW_PA_GCC		0x01800000UL
#define AW_GCC_ADSS_RESET	0x1c000		/* bit 0 holds the audio subsystem in reset */
#define AW_GCC_AUDIO_PLL	0x4b000
#define AW_GCC_PLL_VOTE		0x0b000		/* bit 1: the audio PLL is enabled by a vote */

struct aw_pll {
	u32 mode;
	u32 l;
	u32 alpha_lo;
	u32 alpha_hi;
	u32 user_ctl;
	u32 user_ctl_hi;
	u32 config_ctl;
	u32 test_ctl;
	u32 test_ctl_hi;
	u32 status;
	u32 unused[6];			/* the mapping is 0x40 bytes */
};

#define AW_PLL_OUT_ON		BIT(0)
#define AW_PLL_NOT_BYPASS	BIT(1)
#define AW_PLL_NOT_RESET	BIT(2)
#define AW_PLL_LOCKED		BIT(31)
#define AW_PLL_VOTE_AUDIO	BIT(1)
#define AW_PLL_USER_FRACTION	BIT(24)
#define AW_PLL_USER_POST_DIV4	(3u << 8)
#define AW_PLL_USER_MAIN	BIT(0)
/* 19.2 MHz reference x (40 + 0xf5c28f5d00 / 2^40) = 786.432 MHz; post divider 4 gives 196.608 MHz */
#define AW_PLL_INT_PART		0x28
#define AW_PLL_FRAC_LO		0xc28f5d00
#define AW_PLL_FRAC_HI		0xf5
#define AW_PLL_CONFIG		0x00004289

/* audio subsystem: global block (0x34 bytes) */
#define AW_PA_ADSS		0x07700000UL
struct aw_adss_glb {
	u32 pcm_reset;			/* 0x00 */
	u32 r04, r08;
	u32 mbox_ctl;			/* 0x0c */
	u32 chip_ctl;			/* 0x10 */
	u32 block_reset;		/* 0x14 */
	u32 clk_ctl;			/* 0x18 */
	u32 tdm_ctl;			/* 0x1c */
	u32 r20, r24, r28, r2c;
	u32 mode;			/* 0x30 */
};
#define AW_GLB_CHIP_CTL_ON	(GENMASK(1, 0) | BIT(3))
#define AW_GLB_MODE_TDM		(0x05 | BIT(28))
#define AW_GLB_CLK_OUT_ENABLE	(GENMASK(15, 12) | GENMASK(28, 27) | GENMASK(17, 16))
#define AW_GLB_TDM_VALUE	0x00210021
#define AW_GLB_ALL_BLOCKS	0xff

/* audio subsystem: PCM root clock generator, only the five decoded words are ever touched */
#define AW_PA_ADSS_CLK		(AW_PA_ADSS + 0x1a0)
struct aw_adss_clk {
	u32 command;			/* 0x1a0 */
	u32 config;			/* 0x1a4 */
	u32 misc;			/* 0x1a8 */
	u32 gate;			/* 0x1ac */
	u32 r1b0, r1b4;
	u32 zsi_gate;			/* 0x1b8 */
	u32 r1bc;
};
#define AW_CLK_CMD_UPDATE	BIT(0)
#define AW_CLK_CMD_ROOT_ON	BIT(1)
#define AW_CLK_CFG_FROM_PLL	(1u << 8)
#define AW_CLK_CFG_HALF_DIV	23	/* (23 + 1) / 2 = 12: 196.608 MHz / 12 = 16.384 MHz */
#define AW_CLK_MISC_DIV2	1	/* 8.192 MHz root clock */

#define AW_PA_STEREO_A		0x07709000UL
#define AW_PA_STEREO_B		0x0770f000UL
#define AW_STEREO_RESET		BIT(19)
#define AW_STEREO_RUN		(BIT(21) | BIT(17) | BIT(8))

/* PCM block */
#define AW_PA_PCM		0x07704000UL
struct aw_pcm {
	u32 slotmask;			/* 0x00 */
	u32 ctrl;			/* 0x04 */
	u32 offset;			/* 0x08 */
	u32 start;			/* 0x0c */
	u32 irq_status;			/* 0x10 */
	u32 irq_enable;			/* 0x14 */
	u32 rx8, tx8;			/* 0x18, 0x1c */
	u32 divider;			/* 0x20 */
	u32 threshold;			/* 0x24 */
	u32 fifo_count;			/* 0x28 */
	u32 fifo_error;			/* 0x2c */
	u32 rx16, tx16;			/* 0x30, 0x34 */
	u32 unused[2];			/* the mapping is 0x40 bytes */
};
#define AW_PCM_RX_COUNT(v)	(((v) >> 9) & 0x1ff)
#define AW_PCM_TX_COUNT(v)	((v) & 0x1ff)
#define AW_PCM_CPU_MODE		BIT(29)
#define AW_PCM_IRQ_ERRORS	0x6	/* transmit underrun / receive overrun flags, cleared by writing them back */

/* pin controller: one 0x1000 window per pin; word 0 configuration, word 1 level */
#define AW_PA_TLMM		0x01000000UL
#define AW_TLMM_WINDOW		0x1000
/* function 1, 8 mA, pull-up: the setting used by the stock device tree for the four audio pins */
#define AW_TLMM_AUDIO_FUNC	((1u << 2) | (3u << 6) | 3u)

/* wd_flags */
enum { AW_WD_ARMED, AW_WD_TRIPPED };

/* raw gpio numbers on the 1000000.pinctrl controller */
enum aw_pin {
	AW_PIN_AUDIO_RX = 33,
	AW_PIN_AUDIO_TX = 34,
	AW_PIN_AUDIO_FRAME = 35,
	AW_PIN_AUDIO_BIT = 36,
	AW_PIN_CHIP_RESET = 46,
	AW_PIN_CHIP_IRQ = 47,
	AW_PIN_BUS_CLK = 50,		/* wiring found by trial */
	AW_PIN_BUS_SEL = 51,
	AW_PIN_BUS_MISO = 52,
	AW_PIN_BUS_MOSI = 53,
};

struct awfxs {
	struct mutex lock;
	u32 state;

	u32 __iomem *gcc_reset;
	u32 __iomem *pll_vote;
	struct aw_pll __iomem *pll;
	struct aw_adss_glb __iomem *glb;
	struct aw_adss_clk __iomem *clk;
	struct aw_pcm __iomem *pcm;
	u32 __iomem *bit_pin;		/* pin window of the bit clock */
	u32 __iomem *frame_pin;		/* pin window of the frame sync */

	bool pll_started;		/* the PLL was started by this module and is stopped again on unload */
	bool gates_saved;
	u32 gate_was, zsi_gate_was;	/* clock gates as found, restored on unload */

		struct task_struct *bridge_task;
	struct awfxs_bridge bridge;
	struct awfxs_bridge_stats bridge_stats;
	struct level_window modem_window;	/* the running window of the modem's sample peaks (the bridge thread's) */
	unsigned int bridge_slots;

	struct awfxs_tap {
		bool on;		/* armed by AWFXS_IOC_TAP_SET; disarms on every bridge stop / restart */
		u32 generation;		/* bumped on every bridge stop / restart; readers re-arm on a change */
		struct awfxs_tap_ring ring;
		struct awfxs_tap_frame slot[AWFXS_TAP_FRAMES];
	} tap;

	struct awfxs_play {
		bool on;		/* armed by AWFXS_IOC_PLAY_SET; disarms on every bridge stop / restart */
		u32 generation;		/* bumped on every bridge stop / restart; the writer re-arms on a change */
		struct awfxs_play_ring ring;
		struct awfxs_play_frame slot[AWFXS_PLAY_FRAMES];
		u64 frames;		/* frames the bridge took from here since the arm, silent ones included */
		u32 underruns;		/* frames sent as silence because the ring was empty, since the arm */
	} play;

	struct timer_list wd_timer;
	unsigned long wd_flags;		/* AW_WD_* bits, touched from the timer too */
	atomic_t wd_trips;

	struct gpio_chip *chip;
	struct gpio_desc *miso, *mosi, *sck, *sel, *reset, *irq;
};

int awfxs_hw_map(struct awfxs *z);
void awfxs_hw_unmap(struct awfxs *z);
int awfxs_pcm_up(struct awfxs *z, struct awfxs_pcm_up *up);
void awfxs_pcm_down(struct awfxs *z);
void awfxs_hw_release(struct awfxs *z);
int awfxs_snap(struct awfxs *z, struct awfxs_snap *snap, u32 *words);
void awfxs_regs(struct awfxs *z, struct awfxs_regs *r);

int awfxs_gpio_get(struct awfxs *z);
void awfxs_gpio_put(struct awfxs *z);
int awfxs_chip_reset(struct awfxs *z, unsigned int low_ms);
int awfxs_spi(struct awfxs *z, struct awfxs_spi *x);

/* bridge.c: moves audio between the modem and the line chip through the host FIFOs */
int awfxs_bridge_set(struct awfxs *z, const struct awfxs_bridge *b);
void awfxs_bridge_stop(struct awfxs *z);
void awfxs_bridge_stats(struct awfxs *z, struct awfxs_bridge_stats *s);

/* bridge.c: the capture tap (armed reads mirror every bridge frame into the ring) */
void awfxs_tap_disarm(struct awfxs *z);
int awfxs_tap_set(struct awfxs *z, u32 on);
void awfxs_tap_fill_read(struct awfxs *z, struct awfxs_tap_read *r, struct awfxs_tap_frame *frames);

/* bridge.c: the playback ring (armed writes replace what the bridge sends; an empty ring sends silence) */
void awfxs_play_disarm(struct awfxs *z);
int awfxs_play_set(struct awfxs *z, u32 on);
int awfxs_play_fill_write(struct awfxs *z, struct awfxs_play_write *w, const struct awfxs_play_frame *frames);
void awfxs_play_fill_status(struct awfxs *z, struct awfxs_play_status *s);

/* watchdog.c: keeps the chip in a safe state when nobody is looking after the line */
void awfxs_wd_init(struct awfxs *z);
int awfxs_wd_arm(struct awfxs *z, u32 ms);
void awfxs_wd_hold_safe(struct awfxs *z);
void awfxs_wd_exit(struct awfxs *z);
u32 awfxs_wd_state(struct awfxs *z);

#endif /* AWFXS_H */
