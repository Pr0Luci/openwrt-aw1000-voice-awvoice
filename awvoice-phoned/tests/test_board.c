// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The shipped board file (the package's own default) parsed with the daemon's own reader: make -f Makefile.host test */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "board.h"

static const struct { const char *id; unsigned int length; } need[] = {
	{ "general", 224 }, { "dcfeed", 384 }, { "fsk", 56 }, { "gpio", 7 },
	{ "imped", 256 }, { "pcm", 20 }, { "pulse", 16 }, { "ring", 528 },
	{ "tone", 324 }, { "gain", 48 }, { "init_nopatch", 1464 },
	{ "converter", 28 }, { "iir", 104 },
};

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "files/board.bin";
	static unsigned char buf[16 * 1024];
	FILE *f = fopen(path, "rb");
	assert(f && "the shipped board file is in the package");
	size_t n = fread(buf, 1, sizeof buf, f);
	assert(n > 44 && n < sizeof buf && !ferror(f));
	fclose(f);

	struct board b;
	assert(board_parse(buf, n, &b) == 0);
	for (size_t k = 0; k < sizeof need / sizeof *need; k++) {
		const struct board_section *s = board_find(&b, need[k].id);
		assert(s && "every section the daemon reads is present");
		assert(s->length == need[k].length);
	}
	/* the chip maker's microcode and the stock's full init are not part of the shipped file */
	assert(!board_find(&b, "patch"));
	assert(!board_find(&b, "init_full"));
	printf("board: all tests passed\n");
	return 0;
}
