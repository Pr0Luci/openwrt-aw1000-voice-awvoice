/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_DAEMON_WEB_TLS_H
#define AW_DAEMON_WEB_TLS_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

/*
 * The browser-audio listener's transport: one TLS server socket, one connection at a time, never blocking.
 * mbedTLS is the library (dual-licensed Apache-2.0 / GPL-2.0-or-later, taken here under GPL-2.0-or-later like this
 * daemon). A connection is accepted after web_tls_accept and advanced with web_tls_shake from the daemon's step; reads
 * and writes report 0 while the transport wants to wait, so the 5 ms step never stalls. Nothing here knows the
 * WebSocket protocol (web_audio.c) or the certificates' policy (web_audio.c hands the paths over).
 */

struct web_tls;

/* listen on addr:port with the PEM certificate and key; 0 and *out, or -errno and *out NULL (one line to log) */
int web_tls_listen(struct web_tls **out, const char *addr, unsigned port, const char *cert, const char *key, FILE *log);

/* the listening socket, for the event loop's read watch */
int web_tls_fd(const struct web_tls *t);

/* take the connection that waits: 1 = one is now being handshaken, 0 = none waits, -errno = it could not be taken */
int web_tls_accept(struct web_tls *t, unsigned long long now_ms);

/* drive the handshake: 1 = ready, 0 = in progress, -errno = refused or gone (the connection is closed for you) */
int web_tls_shake(struct web_tls *t, unsigned long long now_ms);

/* 1 while a connection is accepted (handshaking or ready) */
int web_tls_busy(const struct web_tls *t);

/* >0 bytes moved, 0 = the transport wants to wait, -errno = gone (the connection is closed) */
ssize_t web_tls_read(struct web_tls *t, void *buf, size_t cap);
ssize_t web_tls_write(struct web_tls *t, const void *buf, size_t len);

/* end the connection, keep the listener */
void web_tls_drop(struct web_tls *t);

/* the listener and everything on it */
void web_tls_free(struct web_tls **t);

#endif /* AW_DAEMON_WEB_TLS_H */
