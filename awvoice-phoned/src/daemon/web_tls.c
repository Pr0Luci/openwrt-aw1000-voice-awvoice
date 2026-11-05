/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
/* The browser-audio listener's TLS transport (web_tls.h), on mbedTLS. */
#define _GNU_SOURCE		/* accept4, MSG_NOSIGNAL */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "web_tls.h"

#define WEB_HANDSHAKE_MS	10000u	/* a client that cannot finish a handshake in this time is dropped */

struct web_tls {
	int listen_fd;
	int fd;			/* the accepted connection, -1 while none */
	unsigned long long accepted_ms;

	mbedtls_ssl_config conf;
	mbedtls_ssl_context ssl;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt cert;
	mbedtls_pk_context key;
	int conf_up, ssl_up, cert_up, key_up, drbg_up, entropy_up;
};

static void tls_err(FILE *log, const char *what, int err)
{
	char text[128];

	mbedtls_strerror(err, text, sizeof(text));
	fprintf(log, "browser audio: %s: %s (%d)\n", what, text[0] ? text : "error", err);
}

/* the transport callbacks: a non-blocking socket that is not ready is the library's wait, never an error */
static int tls_send(void *ctx, const unsigned char *buf, size_t len)
{
	struct web_tls *t = ctx;
	ssize_t n = send(t->fd, buf, len, MSG_NOSIGNAL);

	if (n >= 0)
		return (int)n;
	if (errno == EAGAIN || errno == EWOULDBLOCK)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int tls_recv(void *ctx, unsigned char *buf, size_t len)
{
	struct web_tls *t = ctx;
	ssize_t n = recv(t->fd, buf, len, 0);

	if (n >= 0)
		return (int)n;
	if (errno == EAGAIN || errno == EWOULDBLOCK)
		return MBEDTLS_ERR_SSL_WANT_READ;
	return MBEDTLS_ERR_NET_RECV_FAILED;
}

void web_tls_drop(struct web_tls *t)
{
	if (!t)
		return;
	if (t->ssl_up) {
		(void)mbedtls_ssl_close_notify(&t->ssl);
		mbedtls_ssl_session_reset(&t->ssl);
	}
	if (t->fd >= 0) {
		close(t->fd);
		t->fd = -1;
	}
}

void web_tls_free(struct web_tls **tp)
{
	struct web_tls *t = tp ? *tp : NULL;

	if (!t)
		return;
	web_tls_drop(t);
	if (t->listen_fd >= 0)
		close(t->listen_fd);
	if (t->conf_up)
		mbedtls_ssl_config_free(&t->conf);
	if (t->cert_up)
		mbedtls_x509_crt_free(&t->cert);
	if (t->key_up)
		mbedtls_pk_free(&t->key);
	if (t->drbg_up)
		mbedtls_ctr_drbg_free(&t->drbg);
	if (t->entropy_up)
		mbedtls_entropy_free(&t->entropy);
	free(t);
	*tp = NULL;
}

int web_tls_fd(const struct web_tls *t)
{
	return t ? t->listen_fd : -1;
}

int web_tls_busy(const struct web_tls *t)
{
	return t && t->fd >= 0;
}

int web_tls_listen(struct web_tls **out, const char *addr, unsigned port, const char *cert, const char *key, FILE *log)
{
	static const char personal[] = "awvoice browser audio";
	struct sockaddr_in sa;
	struct web_tls *t;
	int err, one = 1;
	struct stat st;

	*out = NULL;
	t = calloc(1, sizeof(*t));
	if (!t)
		return -ENOMEM;
	t->listen_fd = t->fd = -1;
	mbedtls_ssl_config_init(&t->conf);
	mbedtls_ssl_init(&t->ssl);
	mbedtls_entropy_init(&t->entropy);
	mbedtls_ctr_drbg_init(&t->drbg);
	mbedtls_x509_crt_init(&t->cert);
	mbedtls_pk_init(&t->key);
	t->entropy_up = 1;
	t->drbg_up = 1;

	if (stat(cert, &st) || stat(key, &st)) {
		fprintf(log, "browser audio: the certificate or its key is missing (%s, %s)\n", cert, key);
		err = -ENOENT;
		goto fail;
	}
	err = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy,
		(const unsigned char *)personal, sizeof(personal) - 1);
	if (err) {
		tls_err(log, "the random source did not seed", err);
		goto fail;
	}
	err = mbedtls_x509_crt_parse_file(&t->cert, cert);
	if (err) {
		tls_err(log, "the certificate could not be read", err);
		goto fail;
	}
	t->cert_up = 1;
	err = mbedtls_pk_parse_keyfile(&t->key, key, NULL, mbedtls_ctr_drbg_random, &t->drbg);
	if (err) {
		tls_err(log, "the private key could not be read", err);
		goto fail;
	}
	t->key_up = 1;
	err = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
		MBEDTLS_SSL_PRESET_DEFAULT);
	if (err) {
		tls_err(log, "the TLS configuration was refused", err);
		goto fail;
	}
	t->conf_up = 1;
	mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);	/* the page authenticates itself with its token */
	mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
	err = mbedtls_ssl_conf_own_cert(&t->conf, &t->cert, &t->key);
	if (err) {
		tls_err(log, "the certificate and key do not pair", err);
		goto fail;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
		fprintf(log, "browser audio: %s is not an IPv4 address to listen on\n", addr);
		err = -EINVAL;
		goto fail;
	}
	t->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (t->listen_fd < 0) {
		err = -errno;
		goto fail;
	}
	(void)setsockopt(t->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(t->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) || listen(t->listen_fd, 4)) {
		err = -errno;
		fprintf(log, "browser audio: %s:%u cannot be listened on: %s\n", addr, port, strerror(errno));
		goto fail;
	}
	*out = t;
	return 0;
fail:
	web_tls_free(&t);
	return err;
}

int web_tls_accept(struct web_tls *t, unsigned long long now_ms)
{
	int fd, err;

	if (!t || t->listen_fd < 0 || t->fd >= 0)
		return 0;
	fd = accept4(t->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -errno;
	t->fd = fd;
	t->accepted_ms = now_ms;
	if (!t->ssl_up) {
		err = mbedtls_ssl_setup(&t->ssl, &t->conf);
		if (err) {
			tls_err(stdout, "a connection's TLS context could not be made", err);
			web_tls_drop(t);
			return -EIO;
		}
		t->ssl_up = 1;
	} else {
		mbedtls_ssl_session_reset(&t->ssl);
	}
	mbedtls_ssl_set_bio(&t->ssl, t, tls_send, tls_recv, NULL);
	return 1;
}

int web_tls_shake(struct web_tls *t, unsigned long long now_ms)
{
	int err;

	if (!t || t->fd < 0)
		return 0;
	err = mbedtls_ssl_handshake(&t->ssl);
	if (err == 0)
		return 1;
	if (err == MBEDTLS_ERR_SSL_WANT_READ || err == MBEDTLS_ERR_SSL_WANT_WRITE)
		return now_ms - t->accepted_ms < WEB_HANDSHAKE_MS ? 0 : -ETIMEDOUT;
	tls_err(stdout, "the TLS handshake was refused", err);
	web_tls_drop(t);
	return -EPROTO;
}

ssize_t web_tls_read(struct web_tls *t, void *buf, size_t cap)
{
	int n;

	if (!t || t->fd < 0)
		return -ENOTCONN;
	n = mbedtls_ssl_read(&t->ssl, buf, cap);
	if (n > 0)
		return n;
	if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF) {
		web_tls_drop(t);
		return -ECONNRESET;
	}
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
	if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
		return 0;		/* the ticket is the library's; the next read carries the page's bytes */
#endif
	if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
		return 0;
	tls_err(stdout, "the TLS stream failed", n);
	web_tls_drop(t);
	return -EPROTO;
}

ssize_t web_tls_write(struct web_tls *t, const void *buf, size_t len)
{
	int n;

	if (!t || t->fd < 0)
		return -ENOTCONN;
	n = mbedtls_ssl_write(&t->ssl, buf, len);
	if (n > 0)
		return n;
	if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
		return 0;
	tls_err(stdout, "the TLS stream failed on a write", n);
	web_tls_drop(t);
	return -EPROTO;
}
