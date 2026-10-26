/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_SLIC_BOARD_H
#define AW_SLIC_BOARD_H

#include <stddef.h>
#include <stdint.h>

/* A board file as written by tools/provision/awvoice-board-extract.py; the sections point into the caller's buffer. */
#define BOARD_SECTIONS_MAX 16

struct board_section {
	char id[16];
	const uint8_t *data;
	uint32_t length;
};

struct board {
	struct board_section section[BOARD_SECTIONS_MAX];
	unsigned int count;
};

int board_parse(const uint8_t *file, size_t size, struct board *out);	/* 0 or -errno; checks magic, version and checksum */
const struct board_section *board_find(const struct board *b, const char *id);
uint32_t board_crc32(const uint8_t *data, size_t length);
uint32_t board_word(const struct board_section *s, unsigned int offset);
uint32_t board_word_at(const uint8_t *p, unsigned int offset);		/* the same for a plain pointer */	/* little-endian 32-bit word; the offset must lie inside */

#endif /* AW_SLIC_BOARD_H */
