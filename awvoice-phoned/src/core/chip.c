// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>

#include "chip.h"
#include "slic_map.h"

#define READ_FRAME	0x60	/* read, register space; bit 4 selects the channel */
#define WRITE_FRAME	0x20
#define RAM_POLL_MS	1
#define RAM_POLL_MAX	100

static uint8_t lead(uint8_t base, unsigned int ch)
{
	return base | (uint8_t)((ch & 1u) << 4);
}

int chip_reg_read(struct chip *c, unsigned int ch, unsigned int reg, uint8_t *value)
{
	const uint8_t tx[4] = { lead(READ_FRAME, ch), (uint8_t)reg, 0, 0 };
	uint8_t rx[4];
	int err = c->ops->frame(c->ctx, tx, rx);

	if (!err)
		*value = rx[2];
	return err;
}

int chip_reg_write(struct chip *c, unsigned int ch, unsigned int reg, uint8_t value)
{
	const uint8_t tx[4] = { lead(WRITE_FRAME, ch), (uint8_t)reg, value, 0 };
	uint8_t rx[4];

	return c->ops->frame(c->ctx, tx, rx);
}

static int ram_idle(struct chip *c, unsigned int ch)
{
	unsigned int tries;
	uint8_t status;
	int err;

	for (tries = 0; tries < RAM_POLL_MAX; tries++) {
		err = chip_reg_read(c, ch, REG_RAMSTAT, &status);
		if (err)
			return err;
		if (!(status & RAMSTAT_BUSY))
			return 0;
		c->ops->pause_ms(c->ctx, RAM_POLL_MS);
	}
	return -ETIMEDOUT;
}

static int window(struct chip *c, unsigned int ch, unsigned int addr)
{
	return chip_reg_write(c, ch, REG_RAM_WINDOW_HI, (addr >> 3) & 0xe0);
}

int chip_ram_write(struct chip *c, unsigned int ch, unsigned int addr, uint32_t value)
{
	unsigned int byte;
	int err;

	if (value > RAM_VALUE_MAX || addr > 0x7ff)
		return -EINVAL;
	err = ram_idle(c, ch);
	if (!err)
		err = window(c, ch, addr);
	/* the 29-bit value sits in the upper bits of four bytes, least significant byte first */
	for (byte = 0; !err && byte < 4; byte++)
		err = chip_reg_write(c, ch, REG_RAM_DATA0 + byte, (uint8_t)((value << 3) >> (8 * byte)));
	if (!err)
		err = chip_reg_write(c, ch, REG_RAM_WINDOW_LO, addr & 0xff);
	return err ? err : ram_idle(c, ch);
}

int chip_ram_read(struct chip *c, unsigned int ch, unsigned int addr, uint32_t *value)
{
	uint32_t raw = 0;
	unsigned int byte;
	int err;

	if (addr > 0x7ff)
		return -EINVAL;
	err = ram_idle(c, ch);
	if (!err)
		err = window(c, ch, addr);
	if (!err)
		err = chip_reg_write(c, ch, REG_RAM_WINDOW_LO, addr & 0xff);
	if (!err)
		err = ram_idle(c, ch);
	for (byte = 0; !err && byte < 4; byte++) {
		uint8_t part;

		err = chip_reg_read(c, ch, REG_RAM_DATA0 + byte, &part);
		raw |= (uint32_t)part << (8 * byte);
	}
	if (!err)
		*value = raw >> 3;
	return err;
}

int chip_user_mode(struct chip *c, unsigned int ch, int on)
{
	static const uint8_t steps[] = USER_MODE_STEPS;
	uint8_t state;
	unsigned int k;
	int err = chip_reg_read(c, ch, REG_USER_MODE, &state);

	if (err || ((state & 1) != 0) == (on != 0))
		return err;
	for (k = 0; !err && k < sizeof(steps); k++)
		err = chip_reg_write(c, ch, REG_USER_MODE, steps[k]);
	if (err)
		return err;
	err = chip_reg_read(c, ch, REG_USER_MODE, &state);
	if (err)
		return err;
	return ((state & 1) != 0) == (on != 0) ? 0 : -EIO;
}
