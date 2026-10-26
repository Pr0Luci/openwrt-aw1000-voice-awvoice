// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <string.h>

#include "board.h"

#define HEADER_SIZE	44	/* magic, version, count, sha256 of the stock module */
#define ENTRY_HEAD	20	/* 16-byte id and a length */
#define BOARD_VERSION	1

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t board_crc32(const uint8_t *data, size_t length)
{
	uint32_t crc = 0xffffffffu;
	unsigned int bit;

	while (length--) {
		crc ^= *data++;
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
	}
	return ~crc;
}

int board_parse(const uint8_t *file, size_t size, struct board *out)
{
	size_t pos = HEADER_SIZE;
	unsigned int n, count;

	memset(out, 0, sizeof(*out));
	if (size < HEADER_SIZE + 4 || memcmp(file, "AWBC", 4))
		return -EINVAL;
	if (le32(file + size - 4) != board_crc32(file, size - 4))
		return -EBADMSG;
	if (le32(file + 4) != BOARD_VERSION)
		return -ENOTSUP;
	count = le32(file + 8);
	if (count > BOARD_SECTIONS_MAX)
		return -E2BIG;

	for (n = 0; n < count; n++) {
		struct board_section *s = &out->section[n];
		uint32_t length;

		if (pos + ENTRY_HEAD > size - 4)
			return -EPROTO;
		memcpy(s->id, file + pos, 15);
		length = le32(file + pos + 16);
		pos += ENTRY_HEAD;
		if (length > size - 4 - pos)
			return -EPROTO;
		s->data = file + pos;
		s->length = length;
		pos += length;
	}
	out->count = count;
	return pos == size - 4 ? 0 : -EPROTO;
}

const struct board_section *board_find(const struct board *b, const char *id)
{
	unsigned int n;

	for (n = 0; n < b->count; n++)
		if (!strcmp(b->section[n].id, id))
			return &b->section[n];
	return NULL;
}

uint32_t board_word_at(const uint8_t *p, unsigned int offset)
{
	p += offset;
	return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t board_word(const struct board_section *s, unsigned int offset)
{
	return board_word_at(s->data, offset);
}
