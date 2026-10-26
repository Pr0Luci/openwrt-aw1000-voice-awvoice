/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_SLIC_CHIP_H
#define AW_SLIC_CHIP_H

#include <stdint.h>

/* Control interface of the line chip over the register bus: four-byte frames, one frame per access. */
struct chip_ops {
	int (*frame)(void *ctx, const uint8_t tx[4], uint8_t rx[4]);	/* 0 or -errno */
	void (*pause_ms)(void *ctx, unsigned int ms);			/* also keeps the line watchdog alive */
	unsigned long long (*now_ms)(void *ctx);			/* monotonic milliseconds; NULL = the system clock */
};

struct chip {
	const struct chip_ops *ops;
	void *ctx;
};

/* Registers (numbers are the chip's own; see slic_map.h) */
int chip_reg_read(struct chip *c, unsigned int ch, unsigned int reg, uint8_t *value);
int chip_reg_write(struct chip *c, unsigned int ch, unsigned int reg, uint8_t value);

/* Internal RAM: 29-bit words reached through a register window; values are written and read back as plain integers. */
#define RAM_VALUE_MAX 0x1fffffffu
int chip_ram_read(struct chip *c, unsigned int ch, unsigned int addr, uint32_t *value);
int chip_ram_write(struct chip *c, unsigned int ch, unsigned int addr, uint32_t value);

/* Access to the protected part of the register space (needed before the configuration RAM is written). */
int chip_user_mode(struct chip *c, unsigned int ch, int on);

#endif /* AW_SLIC_CHIP_H */
