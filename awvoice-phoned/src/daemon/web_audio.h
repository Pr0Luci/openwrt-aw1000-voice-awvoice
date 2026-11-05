/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_DAEMON_WEB_AUDIO_H
#define AW_DAEMON_WEB_AUDIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * The page's browser audio: a TLS listener on the LAN address, one page at a time, WebSocket (web_proto.h) carrying
 * 20 ms / 8 kHz frames. The module owns the transport and the session; the line stays the daemon's: it asks to become
 * the playback ring's owner when a page presents a token minted for the call (grant), and it is told when the session
 * ends (release), so the daemon's one-owner ladder stays the only authority over the ring.
 *
 * The token is minted through the daemon's own ubus method for the call revision the page saw; it is single-use, lives
 * 30 s, and travels only in the first frame of an encrypted stream, never in a URL or a log. A page that is refused
 * hears the fixed word (refused-busy, refused-nocall, refused-revision, refused-mode) and the stream closes.
 *
 * Host-tested pieces: web_proto.h. This module is the daemon's integration and is checked on the device.
 */

struct web_audio;

struct web_audio_ops {
	void *ctx;
	/* the page may have the ring for this call revision: 0, or -EBUSY / -ENOENT / -ESTALE / -EPERM */
	int (*grant)(void *ctx, uint64_t revision);
	/* the session is gone: the ring and the tap sink are the daemon's to release (idempotent) */
	void (*release)(void *ctx);
};

/* Listen on addr:port. 0 and *out; -errno and *out NULL on a certificate, key, address or socket failure (one line
 * to log). */
int web_audio_open(struct web_audio **out, const struct web_audio_ops *ops, const char *addr, unsigned port,
	const char *cert, const char *key, FILE *log);

/* end the session and the listener (release is told once when a session was granted) */
void web_audio_close(struct web_audio **out);

/* the listening socket, for the event loop's read watch */
int web_audio_fd(const struct web_audio *w);

/* take the connection that waits: 1 taken, 0 none, -errno failed (one line logged) */
int web_audio_accept(struct web_audio *w, unsigned long long now_ms);

/* the step: the TLS handshake, the request, the page's frames, the timeouts and the outgoing queue */
void web_audio_tick(struct web_audio *w, unsigned long long now_ms);

/* 1 while a page holds the ring */
int web_audio_granted(const struct web_audio *w);

/* mint a token for a call revision: 0 with token filled, -ENODEV when the listener is not up */
int web_audio_mint(struct web_audio *w, uint64_t revision, unsigned long long now_ms, char *token, size_t cap);

/* one 20 ms far-end frame toward the page; the daemon calls this only while granted (160 samples at 8 kHz) */
void web_audio_capture(struct web_audio *w, const int16_t *samples, unsigned count, uint32_t sequence);

/* the page's voice for the line: 1 when a whole frame was copied to out (160 samples) */
int web_audio_take_playback(struct web_audio *w, int16_t *out, unsigned count);

/* end the session, keep the listener (a granted session tells release) */
void web_audio_drop(struct web_audio *w);

/* the health word: off, listening, connected, or why it is not listening */
const char *web_audio_state(const struct web_audio *w);

#endif /* AW_DAEMON_WEB_AUDIO_H */
