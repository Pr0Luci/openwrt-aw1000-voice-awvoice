// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "fakes.h"

static void record(struct model *m, char kind, unsigned int ch, unsigned int addr, uint32_t value)
{
	if (m->log_count < sizeof(m->log) / sizeof(m->log[0])) {
		m->log[m->log_count].kind = kind;
		m->log[m->log_count].ch = ch;
		m->log[m->log_count].addr = addr;
		m->log[m->log_count].value = value;
		m->log[m->log_count].at = m->vtime_ms;
		m->log_count++;
	}
}

static int model_frame(void *ctx, const uint8_t tx[4], uint8_t rx[4])
{
	struct model *m = ctx;
	unsigned int ch = (tx[0] >> 4) & 1, reg = tx[1] & 0x7f;
	int read = (tx[0] & 0x40) != 0;

	m->frames++;
	memset(rx, 0xff, 4);
	if (read && reg == REG_LCRRTP && m->lcr_fail_ms && m->lcr_started) {
		unsigned long since = m->vtime_ms - m->lcr_origin;

		if (since >= (unsigned long)m->lcr_fail_ms && (!m->lcr_fail_end || since < (unsigned long)m->lcr_fail_end))
			return -EIO;
	}
	if (read) {
		if (reg == REG_ID)
			rx[2] = m->id;
		else if (reg == REG_LINEFEED && m->lf_read_value >= 0)
			rx[2] = (uint8_t)m->lf_read_value;
		else if (reg == REG_LCRRTP) {
			/* the script runs on the model's own clock: one entry per LCR_SCRIPT_MS of paused time */
			unsigned long at;

			if (!m->lcr_started) {
				m->lcr_started = 1;
				m->lcr_origin = m->vtime_ms;		/* the script starts at the first hook read */
			}
			at = (m->vtime_ms - m->lcr_origin) / (m->lcr_ms ? m->lcr_ms : LCR_SCRIPT_MS);

			rx[2] = m->lcr_count
				? m->lcr_bits[at < m->lcr_count ? at : m->lcr_count - 1]
				: m->reg[ch][reg];			/* a static register value when there is no script */
			if (m->lcr_reads && (rx[2] & LCR_ON_HOOK) != (unsigned int)m->lcr_prev)
				m->flag_set = 1;		/* the chip latches every change of its hook state */
			m->lcr_prev = rx[2] & LCR_ON_HOOK;
			m->lcr_reads++;
		} else if (reg == REG_IRQ2) {
			unsigned long since = m->lcr_started ? m->vtime_ms - m->lcr_origin : 0;

			rx[2] = m->reg[ch][reg];
			m->irq2_reads++;
			if (m->irq2_reads == 2 && ((m->lcr_count ? m->lcr_bits[0] : m->reg[ch][REG_LCRRTP]) & LCR_ON_HOOK))
				m->flag_set = 1;		/* the handset was already up: the chip latched that lift before the first read */
			if (m->dtmf_at && m->lcr_started && since >= (unsigned long)m->dtmf_at) {
				rx[2] |= DTMF_IRQ_BIT;
				m->dtmf_at = 0;			/* one event per key press */
			}
			if (m->dtmf_next < m->dtmf_event_count && m->lcr_started && since >= m->dtmf_events[m->dtmf_next].at) {
				rx[2] |= DTMF_IRQ_BIT;
				m->reg[ch][REG_TONDTMF] = m->dtmf_events[m->dtmf_next].code;
				m->dtmf_next++;
			}
			if (m->flag_at && m->lcr_started && since >= (unsigned long)m->flag_at) {
				m->flag_set = 1;
				m->flag_at = m->flag_every ? m->flag_at + m->flag_every : 0;
			}
			if (m->flag_set) {
				rx[2] |= LCR_IRQ_BIT;
				m->flag_set = 0;		/* reading clears the flag */
			}
		} else if (reg == REG_CALR3) {
			m->cal_reads++;
			if (m->cal_stuck || m->cal_pending[ch]) {
				if (m->cal_pending[ch])
					m->cal_pending[ch]--;
				rx[2] = CAL_BUSY;
			} else {
				rx[2] = 0;
			}
		} else if (reg == REG_RAMSTAT)
			rx[2] = m->busy_polls ? (m->busy_polls--, RAMSTAT_BUSY) : 0;
		else
			rx[2] = m->reg[ch][reg];
		return 0;
	}
	if (reg == REG_USER_MODE) {
		static const uint8_t seq[] = USER_MODE_STEPS;

		if (tx[2] == seq[m->step[ch]]) {
			if (++m->step[ch] == 4) {
				m->step[ch] = 0;
				m->user[ch] ^= 1;
			}
		} else {
			m->step[ch] = 0;
		}
		m->reg[ch][reg] = (uint8_t)m->user[ch];
		return 0;
	}
	m->reg[ch][reg] = tx[2];
	if (reg == REG_FSKDAT && (m->reg[ch][REG_IRQEN1] & IRQEN1_FSK_ROOM))
		m->reg[ch][REG_IRQ1] |= IRQ1_FSK_ROOM;		/* the enabled buffer took the byte: room for the next one */
	if (reg == REG_IRQ1)
		m->reg[ch][REG_IRQ1] &= (uint8_t)~tx[2];	/* a written-back flag clears */
	if (reg == REG_CALR3 && (tx[2] & CAL_BUSY))
		m->cal_pending[ch] = m->cal_busy_polls;
	if (reg != REG_RAM_WINDOW_HI && reg != REG_RAM_WINDOW_LO && (reg < REG_RAM_DATA0 || reg >= REG_RAM_DATA0 + 4))
		record(m, 'r', ch, reg, tx[2]);
	if (reg >= REG_RAM_DATA0 && reg < REG_RAM_DATA0 + 4)
		m->pending[ch] = 1;
	if (reg == REG_RAM_WINDOW_LO) {
		unsigned int addr = ((m->reg[ch][REG_RAM_WINDOW_HI] & 0xe0) << 3) | tx[2];
		uint32_t raw = m->reg[ch][6] | m->reg[ch][7] << 8 | m->reg[ch][8] << 16 | (uint32_t)m->reg[ch][9] << 24;

		/* an access that follows data bytes is a write, otherwise it is a read that loads the data bytes */
		int wrote = m->pending[ch];

		if (m->pending[ch] && m->user[ch] && m->ram_sticks) {
			if (addr == WINDOW_POINTER)
				m->win_w[ch] = m->win_r[ch] = raw >> 3;
			if (addr == WINDOW_DATA && m->win_w[ch] < 512)
				m->win[ch][m->win_w[ch]++] = raw >> 3;
			m->ram[ch][addr] = raw >> 3;
			record(m, 'm', ch, addr, raw >> 3);
			if (m->ram_write_count[ch] < 64)
				m->ram_write_addr[ch][m->ram_write_count[ch]++] = addr;
		}
		m->pending[ch] = 0;
		if (addr == RAM_MADC_ILOOP && m->iloop_count) {
			unsigned int at = m->iloop_reads < m->iloop_count ? m->iloop_reads : m->iloop_count - 1;

			m->ram[ch][addr] = (uint32_t)(m->iloop_ua[at] * ILOOP_PER_UA) & RAM_VALUE_MAX;
			m->iloop_reads++;
		}
		if (addr == RAM_VBAT_MEASURE && m->vbat_count) {
			unsigned int at = m->vbat_reads < m->vbat_count ? m->vbat_reads : m->vbat_count - 1;

			m->ram[ch][addr] = (uint32_t)(m->vbat_mv[at] * VBAT_PER_MV) & RAM_VALUE_MAX;
			m->vbat_reads++;
		}
		if (addr == WINDOW_DATA && !wrote && m->win_r[ch] < 512)
			m->ram[ch][addr] = m->win[ch][m->win_r[ch]++];
		if (m->ram_or_addr && addr == m->ram_or_addr)
			m->ram[ch][addr] |= m->ram_or_value;
		if (m->ram_forced_addr && addr == m->ram_forced_addr)
			m->ram[ch][addr] = m->ram_forced_value;
		raw = m->ram[ch][addr] << 3;
		m->reg[ch][6] = (uint8_t)raw;
		m->reg[ch][7] = (uint8_t)(raw >> 8);
		m->reg[ch][8] = (uint8_t)(raw >> 16);
		m->reg[ch][9] = (uint8_t)(raw >> 24);
	}
	return 0;
}

static void model_pause(void *ctx, unsigned int ms) { ((struct model *)ctx)->vtime_ms += ms; }
static unsigned long long model_now(void *ctx)
{
	struct model *m = ctx;

	return m->vtime_ms + (m->frames_per_ms ? m->frames / m->frames_per_ms : 0);
}
const struct chip_ops model_ops = { .frame = model_frame, .pause_ms = model_pause, .now_ms = model_now };

void init_model(struct model *m)
{
	memset(m, 0, sizeof(*m));
	m->id = CHIP_ID_VALUE;
	m->ram_sticks = 1;
	m->lf_read_value = -1;
}

/* build a board file in memory with the same layout the extractor writes */
size_t make_board_with(uint8_t *out, int with_general, uint32_t first_word, const uint32_t *converter)
{
	size_t n = 0;
	uint8_t general[GENERAL_SIZE];
	unsigned int k;
	uint32_t crc;

	memset(general, 0, sizeof(general));
	for (k = 0; k < GENERAL_WORDS; k++) {
		uint32_t v = first_word + k * 0x111u;
		memcpy(general + general_words[k].offset, &v, 4);	/* host is little endian here */
	}
	memcpy(out, "AWBC", 4);
	*(uint32_t *)(out + 4) = 1;
	*(uint32_t *)(out + 8) = (with_general ? 1 : 0) + (converter ? 1 : 0);
	memset(out + 12, 0x5a, 32);
	n = 44;
	if (with_general) {
		memset(out + n, 0, 16);
		memcpy(out + n, "general", 7);
		*(uint32_t *)(out + n + 16) = GENERAL_SIZE;
		memcpy(out + n + 20, general, GENERAL_SIZE);
		n += 20 + GENERAL_SIZE;
	}
	if (converter) {
		memset(out + n, 0, 16);
		memcpy(out + n, "converter", 9);
		*(uint32_t *)(out + n + 16) = CONVERTER_SIZE;
		memcpy(out + n + 20, converter, CONVERTER_SIZE);
		n += 20 + CONVERTER_SIZE;
	}
	crc = board_crc32(out, n);
	memcpy(out + n, &crc, 4);
	return n + 4;
}

size_t make_board(uint8_t *out, int with_general, uint32_t first_word)
{
	return make_board_with(out, with_general, first_word, NULL);
}

/* Test command words: small numbers with the same shape as the real ones (the off bit is set while powered down). */
const uint32_t TW[CW_COUNT] = { 0x0100, 0x0107, 0x0006, 0x0004, 0x0103, 0x0001, 0x0200 };

size_t make_linefeed_board(uint8_t *out, const struct test_board *t)
{
	size_t n = 44;
	uint32_t crc;
	unsigned int k;

	memset(out, 0, 1024);
	memcpy(out, "AWBC", 4);
	*(uint32_t *)(out + 4) = 1;
	*(uint32_t *)(out + 8) = (t->with_converter ? 1 : 0) + (t->with_dcfeed ? 1 : 0)
			       + (t->with_imped ? 1 : 0) + (t->with_tone ? 1 : 0) + (t->with_iir ? 1 : 0) + (t->with_ring ? 3 : 0);
	memset(out + 12, 0x5a, 32);
	if (t->with_converter) {
		memset(out + n, 0, 16);
		memcpy(out + n, "converter", 9);
		*(uint32_t *)(out + n + 16) = CONVERTER_SIZE;
		for (k = 0; k < CW_COUNT; k++)
			memcpy(out + n + 20 + 4 * k, &TW[k], 4);
		n += 20 + CONVERTER_SIZE;
	}
	if (t->with_dcfeed) {
		memset(out + n, 0, 16);
		memcpy(out + n, "dcfeed", 6);
		*(uint32_t *)(out + n + 16) = t->dcfeed_len;
		for (k = 0; k < DC_FEED_WORDS; k++) {
			uint32_t v = 0x1000 + k * 0x111u;

			memcpy(out + n + 20 + 4 * k, &v, 4);
		}
		n += 20 + t->dcfeed_len;
	}
	if (t->with_iir) {
		memset(out + n, 0, 16);
		memcpy(out + n, "iir", 3);
		*(uint32_t *)(out + n + 16) = IIR_SECTION_SIZE;
		for (k = 0; k < 2 * IIR_WORDS; k++) {
			uint32_t v = 0x2000u + k * 0x400u;

			memcpy(out + n + 20 + 4 * k, &v, 4);
		}
		n += 20 + IIR_SECTION_SIZE;
	}
	if (t->with_tone) {
		memset(out + n, 0, 16);
		memcpy(out + n, "tone", 4);
		*(uint32_t *)(out + n + 16) = TONE_PRESET_SIZE;
		{
			uint32_t v[3] = { t->tone_word0, 0x000e2000u, 0 };

			memcpy(out + n + 20, v, 12);			/* oscillator 1 */
			memcpy(out + n + 20 + 16, v, 12);		/* oscillator 2 */
			out[n + 20 + 32] = 0x66;			/* oscillator mode */
		}
		n += 20 + TONE_PRESET_SIZE;
	}
	if (t->with_imped) {
		memset(out + n, 0, 16);
		memcpy(out + n, "imped", 5);
		*(uint32_t *)(out + n + 16) = IMPED_PRESET_SIZE;
		for (k = 0; k < IMPED_RAM_WORDS; k++) {
			uint32_t v = t->imped_word0 + imped_ram_word[k].off;

			memcpy(out + n + 20 + imped_ram_word[k].off, &v, 4);
		}
		out[n + 20 + IMPED_RA_OFFSET] = 0x53;
		n += 20 + IMPED_PRESET_SIZE;
	}
	if (t->with_ring) {
		uint32_t v;

		memset(out + n, 0, 16);
		memcpy(out + n, "ring", 4);
		*(uint32_t *)(out + n + 16) = RING_PRESET_COUNT * RING_PRESET_SIZE;
		memset(out + n + 20, 0, RING_PRESET_COUNT * RING_PRESET_SIZE);
		for (k = 0; k < RING_PRESET_SIZE / 4; k++) {
			v = 0x10000u + 4 * k;			/* every word carries its own byte offset */
			memcpy(out + n + 20 + 4 * k, &v, 4);
		}
		v = 0x1fffffffu;				/* above the general section's battery rail: clamped */
		memcpy(out + n + 20 + RING_VBATR_OFFSET, &v, 4);
		out[n + 20 + RING_TIMER_BYTES_OFFSET] = 0x11;	/* the preset's own timers are replaced by ours */
		out[n + 20 + RING_CON_OFFSET] = 0x98;
		out[n + 20 + RING_USERSTAT_OFFSET] = 0x07;
		n += 20 + RING_PRESET_COUNT * RING_PRESET_SIZE;
		memset(out + n, 0, 16);
		memcpy(out + n, "fsk", 3);
		*(uint32_t *)(out + n + 16) = FSK_PRESET_COUNT * FSK_PRESET_SIZE;
		memset(out + n + 20, 0, FSK_PRESET_COUNT * FSK_PRESET_SIZE);
		for (k = 0; k < FSK_PRESET_COUNT * 6; k++) {
			v = 0x30000u + 0x100u * (k / 6) + k % 6;	/* preset, word */
			memcpy(out + n + 20 + FSK_PRESET_SIZE * (k / 6) + 4 * (k % 6), &v, 4);
		}
		out[n + 20 + FSK_PRESET_SIZE + FSK_DEPTH_OFFSET] = 0x05;
		n += 20 + FSK_PRESET_COUNT * FSK_PRESET_SIZE;
		memset(out + n, 0, 16);
		memcpy(out + n, "general", 7);
		*(uint32_t *)(out + n + 16) = GENERAL_SIZE;
		memset(out + n + 20, 0, GENERAL_SIZE);
		v = 0x500000u;
		memcpy(out + n + 20 + GENERAL_VBATR_OFFSET, &v, 4);
		n += 20 + GENERAL_SIZE;
	}
	crc = board_crc32(out, n);
	memcpy(out + n, &crc, 4);
	return n + 4;
}

void linefeed_model(struct model *m, const int32_t *script, unsigned int count,
	const int32_t *iloop_script, unsigned int iloop_count, const uint8_t *lcr_script, unsigned int lcr_count)
{
	init_model(m);
	m->ram[TEST_CH][RAM_DCDC_CONTROL] = TW[CW_TRACK];	/* the off bit is clear: the converter runs */
	m->ram[TEST_CH][RAM_VBAT_HIGH_EXPECT] = (uint32_t)TEST_HIGH_MV * VBAT_PER_MV;
	memcpy(m->vbat_mv, script, count * sizeof(script[0]));
	m->vbat_count = count;
	if (iloop_count) {
		memcpy(m->iloop_ua, iloop_script, iloop_count * sizeof(iloop_script[0]));
		m->iloop_count = iloop_count;
	}
	if (lcr_count) {
		memcpy(m->lcr_bits, lcr_script, lcr_count);
		m->lcr_count = lcr_count;
	}
}

int fm_begin(void *ctx, enum call_req kind, const char *cmd)
{
	struct fake_modem *f = ctx;

	if (f->lose_after && f->begins >= f->lose_after)
		f->lost = 1;
	if (f->lost)
		return -ENODEV;
	if (f->open)
		return -EBUSY;
	f->open = 1;
	f->kind = kind;
	snprintf(f->cmd, sizeof(f->cmd), "%s", cmd ? cmd : "");
	f->left = f->latency;
	f->begins++;
	return 0;
}

int fm_pump(void *ctx, int *result)
{
	struct fake_modem *f = ctx;

	f->pumps++;
	if (!f->open)
		return 0;
	if (f->lost) {
		f->open = 0;
		*result = -ENODEV;
		return 1;
	}
	if (f->left) {
		f->left--;
		return 0;
	}
	f->open = 0;
	if (f->kind == CALL_REQ_STATE && f->state_timeout)
		*result = -ETIMEDOUT;
	else if (f->kind == CALL_REQ_STATE)
		*result = f->poll && f->poll(f->user, result) ? -EIO : *result;
	else if (f->kind == CALL_REQ_CMD)
		*result = f->act ? f->act(f->user, f->cmd) : 0;
	else if (f->kind == CALL_REQ_INFO) {
		snprintf(f->body, sizeof(f->body), "%s", f->text ? f->text(f->user, f->cmd) : "");
		*result = 0;
	}
	else
		*result = f->cause ? f->cause(f->user) : 0;
	return 1;
}

const char *fm_body(void *ctx)
{
	return ((struct fake_modem *)ctx)->body;
}

unsigned int fm_legs(void *ctx, struct call_leg *out, unsigned int max)
{
	struct fake_modem *f = ctx;
	unsigned int n = f->nlegs < max ? f->nlegs : max;

	memcpy(out, f->legs, n * sizeof(out[0]));
	return n;
}
