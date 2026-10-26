/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
/*
 * Facts about the line chip that the initialisation needs: register numbers, RAM locations and the layout of the board file's
 * "general" section. Numbers only; nothing here is a tuning value (those come from the board file).
 *
 * Provenance: A = two sources agree, S = the stock firmware's own code writes this location,
 * H = confirmed on the chip. Every row below is S (and A where the register map of a third-party implementation also has it);
 * the registers and the 36 general-table locations are also H (written and read back on both channels).
 */
#ifndef AW_SLIC_MAP_H
#define AW_SLIC_MAP_H

#include <stdint.h>

#define CHIP_ID_VALUE		0xc3

enum slic_reg {
	REG_ID = 0,
	REG_PCMMODE = 11,		/* the chip's codec: 0x03 = the setup values, 0x13 = running (bit 0x10) */
	REG_PCMTXLO = 12,		/* the transmit sample's slot, a 10-bit byte offset; bits 0..7 here */
	REG_PCMTXHI = 13,		/* fixed tag 0x10 plus the offset's bits 8..9 */
	REG_MSTRSTAT = 3,
	REG_RAMSTAT = 4,
	REG_RAM_WINDOW_HI = 5,		/* RAM address bits 10..8 in bits 7..5 */
	REG_RAM_DATA0 = 6,		/* four data bytes, 6..9 */
	REG_RAM_WINDOW_LO = 10,		/* RAM address bits 7..0; writing it starts the access */
	REG_PCM_RX_LO = 14,		/* the receive slot's offset, bits 0..7 */
	REG_PCMRXHI = 15,		/* its bits 8..9, other bits belong to the chip (read-modify-write) */
	REG_CALR0 = 26,			/* four calibration control registers, 26..29; bit 7 of the last one is the busy flag */
	REG_CALR3 = 29,
	REG_LINEFEED = 30,
	REG_ZCAL_EN = 46,
	REG_ENHANCE = 47,
	REG_OMODE = 48,			/* oscillator modes for the tone generator */
	REG_OCON = 49,			/* oscillator control: enable bits and timer mode */
	REG_O1_TALO = 50,		/* four timer registers per oscillator, 50..53 and 54..57 */
	REG_O2_TALO = 54,
	REG_RA = 45,			/* line impedance synthesis selection byte */
	REG_DIGCON = 44,		/* the chip's own dialing controls; bits 0x0c stay off (we dial with AT commands) */
	REG_AUTO = 80,
	REG_USER_MODE = 126,
};

/*
 * Interrupt and tone-detector registers: four group registers 18..21 with their enables 22..25; register 19 bit 0x10
 * is the tone-detector event, register 60 holds the detected tone code (digit = the low nibble), register 62 enables
 * the detector (0xe0). Register numbers and the 0x10 bit agree across the three register maps; the 0xe0 value and the
 * digit list below were verified by the earlier in-house project's tests.
 */
enum slic_reg_irq {
	REG_IRQ1 = 18,
	REG_IRQ2 = 19,
	REG_IRQEN1 = 22,
	REG_IRQEN2 = 23,
	REG_LCRRTP = 34,		/* the loop-current-change register; bit 0x2 = off-hook */
	REG_TONDTMF = 60,
	REG_TONEN = 62,
};

#define DTMF_IRQ_BIT		0x10		/* the tone-detector event of register 19 */
#define LCR_IRQ_BIT		0x02		/* the loop-current-change event of register 19 */
#define IRQEN1_FSK_ROOM		0x40		/* the FSK buffer room flag's enable bit (register 22) */
#define IRQEN2_DTMF		0x10		/* its enable bit */
#define IRQEN2_LCR		0x02		/* the hook event's enable bit */
#define LCR_ON_HOOK		0x02		/* register 34's off-hook bit */
#define TONEN_DTMF_DETECT	0xe0		/* enables the DTMF detector */
#define ON_HOOK_UA		5000		/* below the border of the two measured loop-current states */
static const char dtmf_digit_map[] = "D1234567890*#ABC";	/* register 60 code = the low nibble */

#define RAMSTAT_BUSY		0x01
#define USER_MODE_STEPS		{ 0x02, 0x08, 0x0e, 0x00 }	/* the same four writes switch the mode on or off */

/* General-configuration table of the board file ("general", 224 bytes): word at `offset` goes to RAM location `ram`. */
struct general_word {
	uint8_t offset;
	uint16_t ram;
};

#define GENERAL_SIZE		224
static const struct general_word general_words[] = {
	{ .offset = 12,  .ram = 764 },  { .offset = 16,  .ram = 768 },  { .offset = 20,  .ram = 767 },
	{ .offset = 24,  .ram = 915 },  { .offset = 28,  .ram = 916 },  { .offset = 32,  .ram = 919 },
	{ .offset = 36,  .ram = 920 },  { .offset = 56,  .ram = 970 },  { .offset = 64,  .ram = 1004 },
	{ .offset = 68,  .ram = 1005 }, { .offset = 72,  .ram = 1006 }, { .offset = 76,  .ram = 1007 },
	{ .offset = 80,  .ram = 1540 }, { .offset = 84,  .ram = 1541 }, { .offset = 88,  .ram = 1542 },
	{ .offset = 96,  .ram = 1545 }, { .offset = 100, .ram = 1546 }, { .offset = 104, .ram = 1547 },
	{ .offset = 116, .ram = 1554 }, { .offset = 124, .ram = 1558 }, { .offset = 128, .ram = 1560 },
	{ .offset = 136, .ram = 1585 }, { .offset = 140, .ram = 1586 }, { .offset = 144, .ram = 1587 },
	{ .offset = 148, .ram = 1588 }, { .offset = 152, .ram = 1589 }, { .offset = 156, .ram = 1590 },
	{ .offset = 176, .ram = 759 },  { .offset = 180, .ram = 756 },  { .offset = 192, .ram = 967 },
	{ .offset = 200, .ram = 1018 }, { .offset = 204, .ram = 1017 }, { .offset = 208, .ram = 1013 },
	{ .offset = 212, .ram = 1012 }, { .offset = 216, .ram = 1011 }, { .offset = 220, .ram = 1010 },
};
#define GENERAL_WORDS		(sizeof(general_words) / sizeof(general_words[0]))

/* Fixed register writes that precede the table (stock firmware, same for every board). */
#define ENHANCE_INIT		0x00
#define AUTO_INIT		0x2f

/*
 * Battery converter (stage 2). RAM locations and command words as the stock firmware writes them; the order of the steps
 * and the two voltage checks are the stock firmware's too. Voltages are 29-bit signed counts, 1074 per millivolt.
 * The variant selectors are words of the board's "general" section: offset 4 is the converter kind, offset 8 the control scheme.
 */
enum slic_ram {
	RAM_VBAT_MEASURE = 3,
	RAM_VBAT_HIGH_EXPECT = 767,
	RAM_DCDC_CONTROL = 1538,
	RAM_DCDC_OITHRESH = 1548,
	RAM_DCDC_STATUS = 1551,
	RAM_DCDC_PUMP = 1555,
	RAM_OSC1_FREQ = 26,		/* oscillator 1 or 2: frequency, amplitude, phase; the pair starts at 26 or 29
					 * (all three register maps agree) */
	RAM_OSC1_AMP = 27,
	RAM_OSC1_PHAS = 28,
	RAM_OSC2_FREQ = 29,
	RAM_OSC2_AMP = 30,
	RAM_OSC2_PHAS = 31,
	RAM_TXACEQ_C0 = 540,		/* transmit equalizer, four words 540..543 */
	RAM_TXACGAIN = 544,
	RAM_RXACGAIN = 545,
	RAM_RXACEQ_C0 = 546,		/* receive equalizer, four words 546..549 */
	RAM_ECFIR_C2 = 563,		/* hybrid echo cancellor, twelve words 563..574 */
	RAM_ZSYNTH_B0 = 653,		/* line impedance synthesis, five words 653..657 (A1, A2 follow B0..B2) */
	RAM_RXACHPF_B0 = 658,		/* receive high-pass filter, three words 658..660 */
	RAM_RXACGAIN_SAVE = 906,
};

#define VBAT_PER_MV		1074

/*
 * The converter's command words are vendor-provided constants and stay out of the repository: the board file's
 * "converter" section holds them as six little-endian words in this order.
 *   OFF_BIT   bit of the control word that is set while the converter is powered down
 *   PREPARE, START, TRACK, SHUTDOWN   what is written to the control word at each step
 *   PUMP_ON   what is written to the charge-pump word
 *   CHIP_BITS bits of the control word that the chip sets itself (set from reset on); ignored when a write is read back
 */
#define CONVERTER_SIZE		28
enum converter_word {
	CW_OFF_BIT,
	CW_PREPARE,
	CW_START,
	CW_TRACK,
	CW_SHUTDOWN,
	CW_PUMP_ON,
	CW_CHIP_BITS,
	CW_COUNT,
};
#define ENHANCE_KEEP_MASK	0x07		/* bits of the enhancement register that stay set while the converter starts */
#define OITHRESH_OFFSET		72		/* general section word copied to RAM_DCDC_OITHRESH */
#define SCHEME_OFFSET		8
#define SCHEME_TRACKING		1
#define KIND_OFFSET		4
#define KIND_NO_PUMP_A		3		/* these kinds start with the charge pump off: not implemented */
#define KIND_NO_PUMP_B		9

/*
 * Calibration (stages 2 and 3): write the four control registers, then wait until the busy flag in the last one clears.
 * Round A (bandgap/measurement, converter still off) runs before the converter starts, round B (general) after it is
 * tracking. Values, wait limits and the final settings are the stock firmware's; both rounds' values and the final registers also
 * appear in the earlier project's notes and in a second third-party implementation (status A). Wait limits are counts of 10 ms polls.
 */
#define CAL_BUSY		0x80
#define CAL_ROUND_A		{ 0x00, 0x00, 0x01, 0x80 }
#define CAL_ROUND_B		{ 0x00, 0xc0, 0x18, 0x80 }
#define CAL_ROUND_A_POLLS	100
#define CAL_ROUND_B_POLLS	300
#define CAL_POLL_MS		10
/* After round B the final settings of three registers come from the board's general section (one byte each). */
#define FINAL_ENHANCE_OFFSET	190
#define FINAL_AUTO_OFFSET	185
#define FINAL_ZCAL_OFFSET	196

/*
 * Line feed state (register 30). The public datasheet documents the states (open, forward/reverse active, OHT, tip/ring
 * open, ringing, diagnostics); the two codes below agree between two independent register maps
 * ), and the register reads back as the state code (0 reads back exactly after a write of 0).
 * Compare only the low four bits: bits above them are unclear until the next hardware run (TO-VERIFY on the line).
 */
#define LF_STATE_MASK		0x0f
#define LF_CODE_OPEN		0x0
#define LF_CODE_FWD_ACTIVE	0x1

/*
 * DC feed (stage 4): a preset is 24 RAM words written while the line stays open. Each preset word goes to its paired
 * RAM location (both register maps agree on the pairs; the pairs are listed by ascending RAM address).
 * The words themselves are vendor data and stay out of the repository: the board file's "dcfeed" section holds
 * presets of 96 bytes each, and the first one is the preset this board was brought up with (it matches the 24
 * values a second independent build of this board used).
 */
#define DC_FEED_WORDS		24
#define DCFEED_PRESET_SIZE	96
#define DCFEED_PRESET_OFFSET	0			/* the first preset of the board file's "dcfeed" section */
static const struct { uint16_t ram; uint8_t word; } dc_feed_word[DC_FEED_WORDS] = {
	{ 634, 0 },  { 635, 1 },  { 636, 2 },  { 638, 3 },  { 639, 4 },  { 640, 5 },
	{ 641, 6 },  { 642, 7 },  { 643, 8 },  { 644, 9 },  { 645, 10 }, { 701, 13 },
	{ 702, 16 }, { 748, 21 }, { 751, 23 }, { 752, 22 }, { 852, 12 }, { 853, 11 },
	{ 854, 17 }, { 855, 18 }, { 856, 19 }, { 857, 20 }, { 858, 14 }, { 859, 15 },
};

/*
 * Hook timing (stages 7 and 8): the hook register is read every HOOK_POLL_MS on the monotonic clock and a break is
 * classified by its measured length (packages/awvoice-phoned/src/core/hookwatch.h): the windows are the phone settings
 * page's flash_min_ms and flash_max_ms. What the chip itself passes to the register is not known below a few tens of
 * milliseconds (TO-VERIFY, the hook bench prints it); nothing here assumes a debounce time.
 */
#define GUARD_MS		1000		/* the feed guard (line feed state, loop current, battery) samples this often while the digit watch runs */
#define GUARD_LOG_EVERY		60		/* ... and shows its reading in the log once in this many samples (an abnormal reading is always shown) */
#define CALL_POLL_MS		1000		/* the modem's call list is asked for once in this many ms (the next query counts from the last reply) */
#define INFO_EVERY_MS		2000		/* a read-only question to the modem (signal, network, SIM, registration) every this many ms while the line is quiet */
#define POLL_BAD_MAX		3		/* call lists in a row without an answer after which the modem port is treated as lost (a single late answer is a busy modem) */
#define TRY_SPACING_MS		700		/* an answer or a refusal is not sent again sooner than this */
#define CALL_TRY_MAX		3		/* ATA retries before the tool gives up and waits for the next poll */
#define MODEM_CMD_MAX		48		/* the longest command text (ATD + DIAL_NUMBER_MAX digits + ;) */
#define MODEM_QUEUE		8		/* commands that may wait for the open request (the keys pressed in a call are among them) */
#define CALL_FAST_POLL_MS	250		/* the call list is asked for this often while a call is being set up, while our hang-up is pending and just after an answer ... */
#define FAST_AFTER_ANSWER_MS	2000		/* ... which is this long after the answer */

/*
 * The watch's own timing record: a gap between two hook reads longer than this is logged with the slowest task of
 * that gap, so a stall of the loop shows in the log instead of hiding as a missed press.
 */
#define READ_FAIL_MAX		3		/* chip reads of the watch (hook, events) that may fail in a row before the hold stops */
#define TONE_RETRY_MS		250		/* a tone that could not be started or stopped is tried again after this long */
#define HOOK_GAP_LOG_MS		20
#define HOOK_GAP_LOG_MAX	20		/* gap lines logged per run; the end-of-run summary counts them all */

/*
 * The digit watch runs on the clock in short steps: the tone detector's event bit is one level, not a queue, so two
 * digits arriving inside one step would lose the first one (the handset's redial sends its bursts much faster than
 * a finger presses). A standard digit lasts at least 40 ms of tone (ITU-T Q.24), so these steps see every one of them.
 */
#define HOOK_POLL_MS		5		/* the hook register is read this often, between everything else */

/*
 * The hook sensor's bench (--hook-trace): the line is fed and the hook register is watched in HOOK_POLL_MS steps
 * with monotonic stamps, so a physical toggle's width and what the register itself reports are measured, not
 * guessed. The bench also reads the chip's loop-change flag, the loop current and the battery voltage, so a press that the
 * register never shows (too short for the chip's own filter) still leaves a record.
 */

/*
 * Outgoing calls (stage 8): the digits the caller presses are collected and sent as ATD...; (3GPP TS 27.007), on
 * the send key # (3GPP TS 22.030) or when the pauses between digits end. While the remote is rung the ringing
 * tone plays and after a release before the answer the busy or the congestion tone plays, on the board's own tone
 * preset with the ITU-T E.180 cadences; a release tone stops after half a minute at most. The network's release
 * cause (AT+CEER, the 3GPP TS 24.008 numbering) picks the cadence: number and network problems ring fast
 * (congestion), a busy or an absent caller rings slow (busy).
 */
#define FLASH_CODE_MS		3000	/* after a flash in a call, the next key within this time is a code (DESIGN.md, call waiting) */
#define WAIT_TONE_ON_MS		500	/* the call waiting tone: a burst this long (ITU-T E.180: 0.1..0.5 s) ... */
#define WAIT_TONE_PERIOD_MS	5000	/* ... every this long, the short end of the usual repeat (line-tested: the
					   300 ms / 10 s cadence did not catch the ear) */
#define ALARM_CHECK_MS		30000	/* how often the alarm clock is looked at */
#define ALARM_RING_MS		60000	/* an alarm rings at most this long */
#define DIAL_NUMBER_MAX		20		/* digits of one outgoing number */
#define DIAL_IDLE_MS		6000		/* the dial goes out this long after the last DTMF event while the handset is up */
#define NET_AUDIO_PEAK		100		/* a 100 ms window of the modem's audio with a peak above this is the network's own tone or tune (silence reads a small value) ... */
#define NET_AUDIO_WINDOWS	3		/* ... when this many windows in a row are above it */
#define RINGBACK_ON_MS		1000		/* the ringing tone: 1 s on ... */
#define RINGBACK_OFF_MS		4000		/* ... then 4 s off */
#define BUSY_ON_MS		500		/* the busy tone: 0.5 s on / 0.5 s off */
#define CONGEST_ON_MS		250		/* the congestion tone: 0.25 s on / 0.25 s off */
#define RELEASE_TONE_MAX_MS	30000		/* a release tone plays for at most 30 s, then the waiting state returns */

/*
 * Voice (stage 8): the chip's codec over the shared PCM. The chip's start counts are in bit clocks (PCLK) from the
 * frame sync, not in bytes (the manufacturer's documentation counts them in bit clocks; the earlier project's
 * slot base 0 with a stride of 16 says the same). The chip sends its sample one bit
 * clock after its transmit count and listens from its receive count, exactly as the modem's sample sits one bit
 * clock after the slot boundary), so a chip that is to use frame bits 32..47 both ways gets transmit count 31
 * and receive count 32. The modem's sample is at bits 1 (heard) and 0 (sent) (measured). The kernel bridge cuts
 * and pastes at those bit positions, so the chip's counts are derived from the same two numbers below and cannot
 * drift apart from them. The register numbers and the mode values agree between the two register maps); the receive-slot register's other bits belong to the chip and are only read-modify-written. One
 * enhancement-register bit is cleared for the voice path (the second build's setup does the same; one source).
 */
/* the kernel bridge's cut positions: the modem's sample and the chip's own sample (frame bit numbers) */
#define BRIDGE_MODEM_RX		1
#define BRIDGE_MODEM_TX		0
#define BRIDGE_LINE_RX		32		/* where the chip's transmitted sample sits */
#define BRIDGE_LINE_TX		32		/* where the chip listens for its received sample */
#define PHONE_BRIDGE_WIDEBAND	1		/* the bridge flag for HD voice: a second sample half a frame after each of the four */

/*
 * Voice filters (stage 8): the chip's transmit and receive IIR chains (13 RAM words each) come from the board file's
 * "iir" section: the transmit words first, then the receive ones. Both archived implementations of this board carry
 * the same 26 values; they are vendor data and .
 */
#define RAM_TXIIR		518
#define RAM_RXIIR		550
#define IIR_WORDS		13
#define IIR_SECTION_SIZE	104

#define PCM_TX_START_PCLK	(BRIDGE_LINE_RX - 1)	/* the chip sends one bit clock after this count */
#define PCM_RX_START_PCLK	BRIDGE_LINE_TX		/* the chip listens from this count */
#define PCMMODE_SETUP		0x03
#define PCMMODE_RUN		0x13
#define PCMMODE_ACTIVE		0x10
#define PCTXHI_TAG		0x10
#define DIGCON_VOICE_MASK	0x0c
#define ENHANCE_VOICE_BIT	0x01

/*
 * Line feed hold (stage 5): tip and ring are fed for this many seconds, then the line returns to open (closing the
 * device also holds the chip in reset). During the hold the battery voltage is logged (fast samples first, then once
 * per second) together with the loop current; if it falls below the expected value minus the guard while no loop
 * current flows, or the chip leaves the active state, the line goes open and the stage stops.
 */
#define LF_HOLD_SECONDS		30
#define LF_HOLD_MAX_SECONDS	600
#define HOLD_TICK_MS		250		/* per-second logging is too slow for a hook-change sequence */
#define LF_HOLD_GUARD_MV	15000

/*
 * Dial tone (stage 6): the board file's "tone" section holds 36-byte presets; the first one (the stock firmware's
 * dial tone for this board) is the one the second independent build and the stock present. Each preset: oscillator 1
 * frequency, amplitude, phase (29-bit RAM words) and four timer bytes, the same for oscillator 2 (RAM 29..31,
 * timers 54..57), then the oscillator mode byte. The values are vendor data and .
 * Starting sets the enable of both oscillators (0x11, the second independent build's start for a dual-tone preset);
 * stopping clears 0x77.
 */
#define TONE_PRESET_SIZE	36
#define TONE_PRESET_OFFSET	0			/* the first preset of the board file's "tone" section (dial tone) */
#define OCON_ENABLE_SET		0x11			/* both oscillators, no timer mode */
#define OCON_ENABLE_CLEAR	0x77

/*
 * Impedance preset (stage 6, loaded before the tone preset): the board file's "imped" section holds 128-byte presets
 * (the coefficient values are vendor data and stay out of the repository). Within a preset: the transmit and
 * receive equalizer words (8), the hybrid echo cancellor words (12), the impedance synthesis words (5) and the RA
 * register byte, then the transmit gain word, the receive gain word (written to two locations) and the receive
 * high-pass filter words (3). Both register maps agree on every RAM location; the pairs below are
 * listed by ascending RAM address. While a preset is loaded the line goes open for the write and a quick calibration
 * (00 40 00 80, at most 500 polls of 1 ms; both sources) runs afterwards, then the line comes back. The first preset
 * is this board's; the preset's per-country gain database fields are not applied yet (TO-VERIFY after the first
 * listening test).
 */
#define IMPED_PRESET_SIZE	128
#define IMPED_PRESET_OFFSET	0			/* the first preset of the board file's "imped" section */
static const struct { uint16_t ram; uint8_t off; } imped_ram_word[] = {
	{ 540, 0 },  { 541, 4 },  { 542, 8 },  { 543, 12 }, { 544, 104 }, { 545, 108 },
	{ 546, 16 }, { 547, 20 }, { 548, 24 }, { 549, 28 }, { 563, 32 },  { 564, 36 },
	{ 565, 40 }, { 566, 44 }, { 567, 48 }, { 568, 52 }, { 569, 56 },  { 570, 60 },
	{ 571, 64 }, { 572, 68 }, { 573, 72 }, { 574, 76 }, { 653, 80 },  { 654, 84 },
	{ 655, 88 }, { 656, 92 }, { 657, 96 }, { 658, 112 }, { 659, 116 }, { 660, 120 },
	{ 906, 108 },
};
#define IMPED_RAM_WORDS		(sizeof(imped_ram_word) / sizeof(imped_ram_word[0]))
#define IMPED_RA_OFFSET		100
#define CAL_ROUND_Z		{ 0x00, 0x40, 0x00, 0x80 }
#define CAL_Z_POLLS		500
#define CAL_Z_POLL_MS		1
/*
 * Longitudinal balance calibration (the chip maker's official canned procedure, after the impedance preset; the
 * stock's own init runs it the same way): the line goes to forward active, all four LB rounds start, then the
 * line feed goes back. Poll the busy flag every 10 ms like the official loop does.
 */
#define CAL_LB_ALL		0x0f
#define CAL_LB_POLLS		2410
#define CAL_LB_POLL_MS		10

/*
 * Line measurements (stage 5 hold): battery voltage is a 29-bit signed count with VBAT_PER_MV per millivolt; the loop
 * current is a 29-bit signed count with ILOOP_PER_UA per microamp. Both locations and both scale factors agree between
 * the two register maps. A battery fall while loop current flows is the converter following its load —
 * normal feed behaviour under load, not a fault.
 */
#define RAM_MADC_ILOOP		10
#define ILOOP_PER_UA		597
#define HOLD_FAST_MS		100
#define HOLD_FAST_SAMPLES	10

/*
 * The phone's two loop-current states, measured on this board: on-hook stays
 * below this value, off-hook above it. The stages stop the dial tone when the caller goes on-hook (standard FXS
 * behaviour; the second independent build stops it there too) and on the first digit.
 */

#define VBAT_BITS		29
#define VBAT_MARGIN_MV		5000		/* tracking is reached within this distance below the expected high battery voltage */
#define CONVERTER_SETTLE_MS	10
#define CONVERTER_START_MS	50
#define CONVERTER_POLL_MS	10
#define CONVERTER_POLL_MAX	201

/*
 * Ringing (stage 8 with --ring). The ring generator's RAM words, the ring registers and the line-feed code for
 * ringing. Every address was compared value by value between two independent register maps; the
 * words come from the board file's "ring" section, preset 0 (four presets of RING_PRESET_SIZE bytes, the stock
 * firmware's own values; they differ in amplitude and the ring control byte only). The preset's byte offsets
 * follow the field order of the manufacturer's documented structure, checked against the board file: the
 * words are listed in the order they are written. The ring cadence is ours: the chip's own timers count in
 * RING_TIMER_US and make the on/off cadence by themselves, so the stop latency does not depend on this tool.
 */
#define LF_CODE_RING		0x4
#define RING_PRESET_SIZE	132
#define RING_PRESET_COUNT	4
#define RING_PRESET_USED	0
#define RING_VBATR_OFFSET	52		/* the preset's battery-rail word is clamped to the general section's (offset 16) */
#define GENERAL_VBATR_OFFSET	16
#define RING_TIMER_BYTES_OFFSET	56		/* four bytes: on-time low, on-time high, off-time low, off-time high (overwritten) */
#define RING_CON_OFFSET		100
#define RING_USERSTAT_OFFSET	101
#define REG_RINGCON		38
#define REG_RING_TALO		39		/* 39..42: on-time low/high, off-time low/high */
#define REG_USERSTAT		66
#define RING_TIMER_US		125
#define RING_ON_MS		2000		/* default cadence: country profiles are an open item */
#define RING_OFF_MS		4000
#define RING_POLL_MS		50		/* the ring supervision (supply log, caller-id timing, test time) runs this often while the phone rings */
#define RING_SUPPLY_LOG_MS	4000		/* a service logs the supply readings only this long after a ring starts */
#define RING_LOG_MS		500		/* the battery (the ring supply) is logged this often while the phone rings */
#define RING_TEST_MAX_SECONDS	10		/* a ring test stops by itself after this long */
#define RAM_RTPER		755
#define RAM_RINGFR		844
#define RAM_RINGAMP		845
#define RAM_RINGPHAS		846
#define RAM_RINGOF		843
#define RAM_SLOPE_RING		637
#define RAM_IRING_LIM		860
#define RAM_RTACTH		848
#define RAM_RTDCTH		847
#define RAM_RTACDB		850
#define RAM_RTDCDB		849
#define RAM_VOV_RING_BAT	753
#define RAM_VOV_RING_GND	896
#define RAM_VBATR_EXPECT	768
#define RAM_VBAT_TRACK_MIN_RNG	920
#define RAM_VCM_RING		749
#define RAM_VCM_RING_FIXED	482
#define RAM_DELTA_VCM		483
#define RAM_DCDC_RNGTYPE	1560
#define RAM_VOV_DCDC_SLOPE	973
#define RAM_VOV_DCDC_OS		974
#define RAM_VOV_RING_BAT_MAX	975

/*
 * Calling-line identity (FSK). Registers and RAM words agree between the second independent build and the
 * manufacturer's register map; the chip's words for the two signalling variants are the board
 * file's "fsk" section: two presets of FSK_PRESET_SIZE bytes (preset 0 = 1200/2200 Hz, preset 1 = 1300/2100 Hz,
 * identified by their frequency words), each: transition 0->1, transition 1->0, amplitude 0 and 1, frequency 0 and 1
 * (words), then the 8-bit flag and the buffer depth (bytes). The buffer room flag (register 18, bit 0x40, cleared by
 * writing it back) reports only when it is enabled in register 22 (bit 0x40) and the buffer machinery is armed by the
 * depth pair 8 then 0 before the bytes; without either the per-byte polls time out at the first byte (measured on the
 * line, 2026-10-19). Timing
 * follows the signalling specifications: 300 alternating bits, about 180 mark bits, the message at 1200 baud with
 * start and stop bits; the message is sent in the silence at least CID_GAP_MS after the first ring burst.
 */
#define REG_FSKDAT		58
#define REG_FSKDEPTH		59
#define RAM_FSKFREQ0		834
#define RAM_FSKFREQ1		835
#define RAM_FSKAMP0		836
#define RAM_FSKAMP1		837
#define RAM_FSK01		838
#define RAM_FSK10		839
#define IRQ1_FSK_ROOM		0x40
#define OMODE_FSK		0x0a
#define OMODE_STARTSTOP		0x80
#define OCON_FSK		0x05
#define FSK_O1_TALO		0x13
#define FSK_PRESET_SIZE		28
#define FSK_PRESET_COUNT	2
#define FSK_DEPTH_OFFSET	25
#define FSK_DEPTH_PRIME		0x08
#define FSK_SEIZURE_BYTES	38		/* 304 bits of 0x55 */
#define FSK_MARK_BYTES		23		/* 184 mark bits */
#define FSK_ROOM_WAIT_MS	40		/* per byte: about five byte times at 1200 baud */
#define FSK_DRAIN_MS		20		/* after the last byte's room: one byte time plus margin before the carrier stops */
#define CID_TYPE_MDMF		0x80
#define CID_PARAM_DATETIME	0x01
#define CID_PARAM_NUMBER	0x02
#define CID_PARAM_REASON	0x04
#define CID_PARAM_NAME		0x07
#define LF_RING_PHASE_MASK	0xf0		/* the line feed register's upper half shows the ring burst (TO-VERIFY on the line) */
#define LF_RING_PHASE_ON	0x40
#define CID_GAP_MS		600		/* silence after the ring burst before the data (the specifications ask 500 ms or more) */
#define CID_PRERING_GAP_MS	600		/* after a caller id sent before the first ring, the silence before the first burst */

#endif /* AW_SLIC_MAP_H */
