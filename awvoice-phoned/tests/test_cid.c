// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * The caller-id message and its FSK send: the multiple-data-message bytes, and the send over the fake chip, which
 * takes the bytes only from the on-hook transmission state (as the line did). make -f Makefile.host test
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "chip.h"
#include "cid.h"
#include "fakes.h"
#include "slic_map.h"

static void test_message(void)
{
	struct cid_info info = { 10, 28, 14, 5, "0133 1234567", "Pr0Luci" };
	struct cid_info none = { 1, 1, 0, 0, NULL, NULL };
	struct cid_info bad = { 0, 1, 0, 0, "1", NULL };
	uint8_t msg[CID_MESSAGE_MAX];
	unsigned int k, sum;
	int len;

	len = cid_message(&info, msg, sizeof(msg));
	assert(len == 35);
	assert(msg[0] == CID_TYPE_MDMF && msg[1] == len - 3);	/* the length counts the parameters, not type, length or check */
	assert(msg[2] == CID_PARAM_DATETIME && msg[3] == 8 && !memcmp(msg + 4, "10281405", 8));
	assert(msg[12] == CID_PARAM_NUMBER && msg[13] == 11 && !memcmp(msg + 14, "01331234567", 11));
	assert(msg[25] == CID_PARAM_NAME && msg[26] == 7 && !memcmp(msg + 27, "Pr0Luci", 7));
	for (k = 0, sum = 0; k < (unsigned int)len; k++)
		sum += msg[k];
	assert((sum & 0xff) == 0);		/* the bytes and the check add up to zero */

	len = cid_message(&none, msg, sizeof(msg));
	assert(len == 16);
	assert(msg[2] == CID_PARAM_DATETIME && msg[12] == CID_PARAM_REASON && msg[13] == 1 && msg[14] == 'O');

	/* more digits than the message may carry are cut; a name with no printable character is left out */
	info.number = "123456789012345678901234567890";
	info.name = "";
	len = cid_message(&info, msg, sizeof(msg));
	assert(len == 2 + 10 + 2 + CID_NUMBER_MAX + 1 && msg[13] == CID_NUMBER_MAX);

	assert(cid_message(&bad, msg, sizeof(msg)) == -EINVAL);		/* month 0 */
	bad.month = 1;
	bad.day = 32;
	assert(cid_message(&bad, msg, sizeof(msg)) == -EINVAL);
	bad.day = 1;
	bad.hour = 24;
	assert(cid_message(&bad, msg, sizeof(msg)) == -EINVAL);
	bad.hour = 0;
	bad.minute = 60;
	assert(cid_message(&bad, msg, sizeof(msg)) == -EINVAL);
	bad.minute = 0;
	assert(cid_message(&bad, msg, sizeof(msg)) == 16);
	assert(cid_message(&bad, msg, CID_MESSAGE_MAX - 1) == -EINVAL);
}

struct send_rig {
	struct model m;
	struct chip c;
	struct board b;
	uint8_t file[1024];
	FILE *log;
	char *text;
	size_t text_len;
};

/* the board's ring file carries the "fsk" pairs and the general section; the line starts in the given state */
static void rig_init(struct send_rig *r, uint8_t lf)
{
	struct test_board t = { 0 };
	size_t size;

	memset(r, 0, sizeof(*r));
	t.with_ring = 1;
	size = make_linefeed_board(r->file, &t);
	assert(board_parse(r->file, size, &r->b) == 0);
	init_model(&r->m);
	r->m.user[TEST_CH] = 1;			/* the daemon holds protected mode for the caller-id setup */
	r->m.reg[TEST_CH][REG_LINEFEED] = lf;
	r->c = (struct chip){ .ops = &model_ops, .ctx = &r->m };
	r->log = open_memstream(&r->text, &r->text_len);
}

/* the line-feed writes' first and last index in the model's record, and the first data byte's */
static void timeline(struct send_rig *r, int *lf_first, int *lf_last, int *dat_first, unsigned int *dat_count)
{
	unsigned int k;

	*lf_first = *lf_last = *dat_first = -1;
	*dat_count = 0;
	for (k = 0; k < r->m.log_count; k++) {
		if (r->m.log[k].kind != 'r')
			continue;
		if (r->m.log[k].addr == REG_LINEFEED) {
			if (*lf_first < 0)
				*lf_first = (int)k;
			*lf_last = (int)k;
		} else if (r->m.log[k].addr == REG_FSKDAT) {
			if (*dat_first < 0)
				*dat_first = (int)k;
			(*dat_count)++;
		}
	}
}

/* the bytes go out from on-hook transmission and the line-feed register comes back as it was read (a ringing line as
 * forward active: the chip starts a burst whenever the line enters the ringing state) */
static void test_send(uint8_t before, uint8_t after, int standard)
{
	struct cid_info info = { 10, 28, 14, 5, "01331234567", NULL };
	uint8_t msg[CID_MESSAGE_MAX];
	int len = cid_message(&info, msg, sizeof(msg)), lf_first, lf_last, dat_first;
	unsigned int dat_count;
	struct send_rig r;

	assert(len == 26);
	rig_init(&r, before);
	assert(cid_send(&r.c, &r.b, TEST_CH, standard, msg, (size_t)len, NULL, r.log) == 0);
	timeline(&r, &lf_first, &lf_last, &dat_first, &dat_count);
	assert(lf_first >= 0 && dat_first > lf_first && lf_last > dat_first);
	assert(r.m.log[lf_first].value == LF_CODE_FWD_OHT);
	assert(r.m.log[lf_last].value == after);
	assert(dat_count == FSK_SEIZURE_BYTES + FSK_MARK_BYTES + 26);
	fclose(r.log);
	free(r.text);
}

static int always_stop(void *ctx)
{
	(void)ctx;
	return 1;
}

/* The mode's framing bit around the bytes: set (plain bits) for the seizure and the mark, clear (a start and a stop
 * bit on every byte) from the message's first byte on. The line showed this order on 2026-10-29 -- the reverse of
 * what the bit's name suggested: the message's bytes left the chip as 8-bit runs and the handset refused them. */
static void test_framing(void)
{
	struct cid_info info = { 10, 28, 14, 5, "01331234567", NULL };
	uint8_t msg[CID_MESSAGE_MAX];
	int len = cid_message(&info, msg, sizeof(msg)), raw_before = -1, framed_before = -1;
	const unsigned int message_at = FSK_SEIZURE_BYTES + FSK_MARK_BYTES;
	struct send_rig r;
	unsigned int k, dat = 0;

	rig_init(&r, LF_CODE_RING);
	assert(cid_send(&r.c, &r.b, TEST_CH, CID_BELLCORE, msg, (size_t)len, NULL, r.log) == 0);
	for (k = 0; k < r.m.log_count; k++) {
		if (r.m.log[k].kind != 'r')
			continue;
		if (r.m.log[k].addr == REG_FSKDAT)
			dat++;
		else if (r.m.log[k].addr == REG_OMODE) {
			if (r.m.log[k].value & OMODE_FSKRAW) {
				if (raw_before < 0)
					raw_before = (int)dat;
			} else if (dat == message_at && framed_before < 0)
				framed_before = (int)dat;
		}
	}
	assert(raw_before == 0);				/* the plain mode is in force before the seizure's first byte */
	assert(framed_before == (int)message_at);		/* and the framed one starts the message */
	fclose(r.log);
	free(r.text);
}

/* a lift during the send stops it, and the line still goes back to the state it came from */
static void test_stopped(void)
{
	struct cid_info info = { 10, 28, 14, 5, "01331234567", NULL };
	uint8_t msg[CID_MESSAGE_MAX];
	int len = cid_message(&info, msg, sizeof(msg)), lf_first, lf_last, dat_first;
	const struct cid_guard guard = { always_stop, NULL };
	unsigned int dat_count;
	struct send_rig r;

	rig_init(&r, LF_CODE_RING);
	assert(cid_send(&r.c, &r.b, TEST_CH, CID_BELLCORE, msg, (size_t)len, &guard, r.log) == -ECANCELED);
	timeline(&r, &lf_first, &lf_last, &dat_first, &dat_count);
	assert(lf_first >= 0 && dat_count == 0);
	assert(r.m.log[lf_first].value == LF_CODE_FWD_OHT);
	assert(r.m.log[lf_last].value == LF_CODE_FWD_ACTIVE);	/* a ringing line stays forward active: no burst after the message */
	fclose(r.log);
	free(r.text);
}

/* the room flag the data send waits for: absent from a plain forward active line, present in on-hook transmission */
static void test_gate(void)
{
	struct send_rig r;
	uint8_t irq;

	rig_init(&r, LF_CODE_FWD_ACTIVE);
	assert(chip_reg_write(&r.c, TEST_CH, REG_IRQEN1, IRQEN1_FSK_ROOM) == 0);
	assert(chip_reg_write(&r.c, TEST_CH, REG_FSKDAT, 0x55) == 0);
	assert(chip_reg_read(&r.c, TEST_CH, REG_IRQ1, &irq) == 0);
	assert(!(irq & IRQ1_FSK_ROOM));
	assert(chip_reg_write(&r.c, TEST_CH, REG_LINEFEED, LF_CODE_FWD_OHT) == 0);
	assert(chip_reg_write(&r.c, TEST_CH, REG_FSKDAT, 0x55) == 0);
	assert(chip_reg_read(&r.c, TEST_CH, REG_IRQ1, &irq) == 0);
	assert(irq & IRQ1_FSK_ROOM);
	fclose(r.log);
	free(r.text);
}

int main(void)
{
	test_message();
	test_send(LF_CODE_RING, LF_CODE_FWD_ACTIVE, CID_ETSI);		/* the caller id between the ring bursts */
	test_send(LF_CODE_RING | 0x20, LF_CODE_FWD_ACTIVE, CID_ETSI);	/* the ring's pause bits as the line showed them */
	test_send(LF_CODE_FWD_ACTIVE, LF_CODE_FWD_ACTIVE, CID_BELLCORE);	/* the caller id before the first ring */
	test_send(LF_CODE_FWD_ACTIVE | 0x20, LF_CODE_FWD_ACTIVE | 0x20, CID_ETSI);	/* another state, whole */
	test_framing();
	test_stopped();
	test_gate();
	printf("cid: all tests passed\n");
	return 0;
}
