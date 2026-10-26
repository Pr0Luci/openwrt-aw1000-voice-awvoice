/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_WIDEBAND_H
#define AW_PHONED_WIDEBAND_H

#include <stdio.h>

#include "chip.h"

/*
 * HD voice on the line chip (the wideband mode), applied per channel.
 *
 * What it does: the chip's codec switches between the standard voiceband (3.4 kHz) and the wideband
 * (50 Hz - 7 kHz) mode (the Si32260/61 product brief p.30, the private reference collection); in the
 * wideband mode the PCM path carries a second sample half a frame (128 bits) after the first, with FSYNC
 * kept at 8 kHz, so the bridge (AWFXS_BRIDGE_WIDEBAND) and the modem's 16 kHz PCM line up on the same frame.
 *
 * The registers and the blocks (the sources are in the private reference collection, REFERENCES.md):
 *   DIGCON = 0x2C, bits 2 and 3: the transmit/receive double-rate selects (the API's register map);
 *   ENHANCE = 0x2F, bit 0: the wideband enhance select (the API's register map; the independent second
 *   source is the reference implementation's documentation);
 *   TXACIIR = RAM 518-530 and RXACIIR = RAM 550-562: the audio filters of the double-rate path, three
 *   sections each (a first-order section, then two biquads), coefficients as 32-bit two's complement with
 *   1.0 = 0x0800000 (the format follows from the API's own preset values).
 *
 * The coefficients are OUR OWN design (no vendor data ships): a bilinear transform at 16 kHz of the
 * G.712 wideband band (50 Hz high-pass, two 7.5 kHz low-pass biquads per direction), scaled into the
 * format above. Every write is read back; the narrowband coefficients the line had before the
 * first apply are snapshotted and restored on disable, so nothing of another party's is needed to go back.
 */
struct wideband {
	int applied;				/* the wideband coefficients are on the chip */
	int snapshotted;			/* the narrowband coefficients of this line are saved */
	uint32_t narrow_tx[13], narrow_rx[13];	/* the snapshots (TXACIIR 13 words, RXACIIR 13 words) */
};

#define WIDEBAND_NCOEFF		13

/* apply or remove the wideband mode with the staged read-backs; 0, or -errno (the caller closes the line) */
int wideband_apply(struct chip *c, unsigned int ch, struct wideband *w, int on, FILE *log);

/* 1 while the wideband coefficients are on the chip */
int wideband_active(const struct wideband *w);

#endif
