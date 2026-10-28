/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* Copyright (C) 2026 Pr0Luci */
#ifndef _UAPI_LINUX_AW_FXS_H
#define _UAPI_LINUX_AW_FXS_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* Interface of /dev/awvoice-fxs0. Every structure has a fixed layout; unused fields must be zero. */
#define AWFXS_ABI		1

/* awfxs_info.state */
#define AWFXS_STATE_PLL_UP	0x01
#define AWFXS_STATE_PCM_RUN	0x02
#define AWFXS_STATE_GPIO_OK	0x04
#define AWFXS_STATE_WD_ARMED	0x08	/* line watchdog running */
#define AWFXS_STATE_WD_TRIPPED	0x10	/* watchdog expired or device closed: chip held in reset until the next reset call */

struct awfxs_info {
	__u32 abi;
	__u32 state;
	__u32 irq_level;	/* line-chip interrupt pin, 1 = idle (active low) */
	__u32 wd_trips;		/* number of watchdog expiries since the module was loaded */
};

/* PCM block set-up; the register values are applied as given. */
struct awfxs_pcm_cfg {
	__u32 ctrl;		/* control register value (see PCM_CTRL_* in the module) */
	__u32 bitmap;		/* active slots, one bit per slot */
	__u32 offset;		/* frame offset in bit clocks */
	__u32 divider;		/* bit-clock divider after the 8.192 MHz root clock */
};

struct awfxs_pcm_up {
	struct awfxs_pcm_cfg cfg;	/* in */
	__u32 pll_mode;		/* out: PLL mode register after programming */
	__u32 pll_locked;	/* out: 1 = lock detected */
	__u32 pclk_changes;	/* out: level changes seen on the bit-clock pin in 2048 reads */
	__u32 fsync_changes;	/* out: same for the frame-sync pin */
};

/* One register access is one frame of exactly four bytes: control, address, data, pad. */
#define AWFXS_SPI_FRAME		4
#define AWFXS_SPI_MAX		8
struct awfxs_spi {
	__u8 len;		/* must be AWFXS_SPI_FRAME */
	__u8 tx[AWFXS_SPI_MAX];
	__u8 rx[AWFXS_SPI_MAX];
	__u8 pad[7];
};

struct awfxs_snap {
	__u64 buf;		/* in: user buffer of __u32 words */
	__u32 max_words;	/* in */
	__u32 timeout_us;	/* in: at most 50000 */
	__u32 got;		/* out */
	__u32 fifo_err;		/* out: FIFO error register after the capture */
	__u32 pad;
};

struct awfxs_regs {
	__u32 flags;		/* bit0: PLL/reset registers valid, bit1: ADSS and PCM registers valid */
	__u32 pll[10];		/* mode, L, alpha, alpha-hi, user ctl, user ctl hi, config, test, test hi, status */
	__u32 adss_reset;
	__u32 glb[13];		/* ADSS global registers 0x00..0x30 in steps of 4 (words not read are 0) */
	__u32 clk[8];		/* ADSS PCM clock registers 0x1a0..0x1bc in steps of 4 (words not read are 0) */
	__u32 pcm[14];		/* PCM block registers 0x00..0x34 in steps of 4 (words not read are 0) */
	__u32 gcc_pll_vote;	/* clock-controller PLL vote register (bit 1 = audio PLL voted by someone) */
};

#define AWFXS_IOC_MAGIC		'Z'
#define AWFXS_IOC_INFO		_IOR(AWFXS_IOC_MAGIC, 0, struct awfxs_info)
#define AWFXS_IOC_REGS		_IOR(AWFXS_IOC_MAGIC, 1, struct awfxs_regs)
#define AWFXS_IOC_PCM_UP	_IOWR(AWFXS_IOC_MAGIC, 2, struct awfxs_pcm_up)
#define AWFXS_IOC_PCM_DOWN	_IO(AWFXS_IOC_MAGIC, 3)
#define AWFXS_IOC_CHIP_RESET	_IOW(AWFXS_IOC_MAGIC, 4, __u32)	/* argument: low time in ms (1..100) */
#define AWFXS_IOC_SPI		_IOWR(AWFXS_IOC_MAGIC, 5, struct awfxs_spi)
#define AWFXS_IOC_SNAP		_IOWR(AWFXS_IOC_MAGIC, 6, struct awfxs_snap)

/*
 * Line watchdog. The argument is a timeout in ms: 0 stops it, AWFXS_WD_MIN_MS..AWFXS_WD_MAX_MS starts or restarts it.
 * When it runs out, or when the device is closed, the line chip is held in reset (line voltage and ringing removed)
 * until AWFXS_IOC_CHIP_RESET is called again. Register writes to the chip are refused while it is not running.
 */
#define AWFXS_WD_MIN_MS		100
#define AWFXS_WD_MAX_MS		5000
#define AWFXS_IOC_RING_ARM	_IOW(AWFXS_IOC_MAGIC, 7, __u32)

/*
 * Audio bridge. The PCM block must be up in host-FIFO mode with the slots 0..k-1 enabled, where k is the number of
 * 16-bit slots that hold all four samples: k = (highest position + 16 + 15) / 16. Positions are bit offsets inside
 * the frame (0..48); a sample occupies the 16 bits from its position.
 *   CROSS: what the modem sends goes to the line chip and what the line chip sends goes to the modem.
 *   LOOP : what the modem sends goes back to the modem (test; the line positions are ignored).
 * The bridge stops when the device is closed or the PCM block goes down.
 */
#define AWFXS_BRIDGE_OFF	0
#define AWFXS_BRIDGE_CROSS	1
#define AWFXS_BRIDGE_LOOP	2
#define AWFXS_BRIDGE_POS_MAX	48
#define AWFXS_BRIDGE_WIDEBAND	1	/* flag: a second sample follows each of the four at +128 bits (16 kHz voice on the 8 kHz frame) */

struct awfxs_bridge {
	__u32 mode;
	__u32 modem_rx;		/* position of the modem's sample in what we receive */
	__u32 modem_tx;		/* position of the modem's sample in what we send */
	__u32 line_rx;
	__u32 line_tx;
	__u32 flags;		/* AWFXS_BRIDGE_WIDEBAND or 0; the wideband frame needs slots 0..11 enabled */
	__u32 pad[2];
};

#define AWFXS_GAP_BUCKETS	6	/* loop gaps: <1.1, <1.25, <1.5, <2, <2.5, >=2.5 ms */
struct awfxs_bridge_stats {
	__u64 frames;		/* frames moved */
	__u32 running;
	__u32 slots;		/* k */
	__u32 resyncs;		/* FIFO error flags seen (underrun / overrun) */
	__u32 tx_skipped;	/* frames not sent because the transmit FIFO was full */
	__u32 max_gap_us;	/* longest time between two services of the FIFOs */
	__u32 gap_hist[AWFXS_GAP_BUCKETS];
	__u32 peak_modem;	/* largest magnitude of the modem / line sample since the start */
	__u32 peak_line;
	__u32 modem_window_peak;	/* largest magnitude of the modem's samples in the last completed window of AWFXS_WINDOW_FRAMES frames (100 ms): is audio arriving from the modem now */
	__u32 modem_windows;	/* windows completed since the start; a reader that sees it unchanged has read the same window twice */
};
#define AWFXS_WINDOW_FRAMES	800	/* 100 ms at 8000 frames per second */
#define AWFXS_IOC_BRIDGE	_IOW(AWFXS_IOC_MAGIC, 8, struct awfxs_bridge)
#define AWFXS_IOC_BRIDGE_STATS	_IOR(AWFXS_IOC_MAGIC, 9, struct awfxs_bridge_stats)

/*
 * Capture tap. While armed (and a bridge is running) every bridge frame is mirrored into a ring that a reader drains
 * with AWFXS_IOC_TAP_READ; the tap is inert while disarmed and costs the bridge thread nothing. Arming needs a
 * running bridge. The tap disarms on every bridge stop or restart — a restart may also change the wideband layout —
 * and bumps the generation: a reader that sees a new generation discards what it had and arms again.
 * Each frame read back is one awfxs_tap_frame: four little-endian __u16 — modem, line, wideband second modem,
 * wideband second line (the seconds are 0 when narrowband; line is 0 in LOOP mode).
 */
struct awfxs_tap_frame {
	__u16 modem;
	__u16 line;
	__u16 modem2;
	__u16 line2;
};

struct awfxs_tap_read {
	__u64 buf;		/* in: user array of frames, four __u16 each */
	__u32 max_frames;	/* in */
	__u32 got;		/* out */
	__u32 dropped;		/* out: frames dropped when the ring was full, since the arm */
	__u32 wideband;		/* out: 1 when the armed bridge carries second samples */
	__u32 generation;	/* out: changes on every bridge stop / restart */
	__u32 pad;
};

#define AWFXS_IOC_TAP_SET	_IOW(AWFXS_IOC_MAGIC, 10, __u32)	/* 1 arm, 0 disarm */
#define AWFXS_IOC_TAP_READ	_IOWR(AWFXS_IOC_MAGIC, 11, struct awfxs_tap_read)

#endif /* _UAPI_LINUX_AW_FXS_H */
