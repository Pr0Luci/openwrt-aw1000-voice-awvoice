/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_DAEMON_WEB_PROTO_H
#define AW_DAEMON_WEB_PROTO_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>		/* strcasecmp/strncasecmp: the handshake's header names are case-insensitive (RFC 6455 §4.1) */

/*
 * The protocol of the browser-audio listener: one HTTP/1.1 request that becomes a WebSocket (RFC 6455), and then the
 * frames of this daemon's own audio stream. Host-tested (tests/test_web_proto.c); nothing here opens a socket or knows
 * about TLS.
 *
 * A frame on the WebSocket is binary and carries a 12-byte header, big-endian on the wire:
 *   [0] 'A' (0x41)  [1] version 1  [2] type  [3] 0
 *   [4..7] sequence  [8..11] timestamp_ms
 * followed, for a PCM frame, by 160 samples of 16-bit linear audio in network byte order (S16BE, the RTP L16 order),
 * 20 ms at 8000 Hz. The daemon sends PCM downlink; the page sends PCM uplink; HELLO carries the audio token as ASCII,
 * GRANT answers one verdict word, MUTE suspends the uplink only, and BYE closes the stream with a reason word.
 */

#define WEB_MAGIC		0x41		/* 'A' */
#define WEB_VERSION		1
#define WEB_HDR_LEN		12
#define WEB_RATE		8000
#define WEB_FRAME_MS		20
#define WEB_SAMPLES		160
#define WEB_PCM_BYTES		(WEB_SAMPLES * 2)

enum web_type {
	WEB_T_HELLO = 1,	/* page -> daemon: the token, ASCII */
	WEB_T_GRANT = 2,	/* daemon -> page: "granted" or one refusal word, ASCII */
	WEB_T_PCM = 3,
	WEB_T_MUTE = 4,		/* page -> daemon: one byte, 1 = stop the uplink, 0 = resume */
	WEB_T_BYE = 5,		/* either way: one reason word, ASCII, best effort */
	WEB_T_ERROR = 6,	/* daemon -> page: one reason word, ASCII, then the stream closes */
};

/* the largest frame this build accepts from a page: a HELLO, a PCM frame, a short control word */
#define WEB_FRAME_MAX		512
#define WEB_TOKEN_MAX		80
#define WEB_REASON_MAX		40

static inline void web_put_u32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v >> 24);
	b[1] = (uint8_t)(v >> 16);
	b[2] = (uint8_t)(v >> 8);
	b[3] = (uint8_t)v;
}

static inline uint32_t web_get_u32(const uint8_t *b)
{
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static inline void web_s16be_put(uint8_t *b, int16_t s)
{
	uint16_t u = (uint16_t)s;

	b[0] = (uint8_t)(u >> 8);
	b[1] = (uint8_t)u;
}

static inline int16_t web_s16be_get(const uint8_t *b)
{
	return (int16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
}

/* the 12-byte header of one audio frame */
static inline void web_hdr_put(uint8_t *b, uint8_t type, uint32_t seq, uint32_t ts_ms)
{
	b[0] = WEB_MAGIC;
	b[1] = WEB_VERSION;
	b[2] = type;
	b[3] = 0;
	web_put_u32(b + 4, seq);
	web_put_u32(b + 8, ts_ms);
}

/* 1 when the twelve bytes are one of this build's headers, 0 when they are not (magic or version) */
static inline int web_hdr_get(const uint8_t *b, uint8_t *type, uint32_t *seq, uint32_t *ts_ms)
{
	if (b[0] != WEB_MAGIC || b[1] != WEB_VERSION)
		return 0;
	*type = b[2];
	*seq = web_get_u32(b + 4);
	*ts_ms = web_get_u32(b + 8);
	return 1;
}

/* a control word on the stream carries the same twelve-byte header as a PCM frame, then the word. both ends read the
 * type from the header, so a word that travels bare is a frame neither end can place: it is not the word's bytes that
 * say what the frame is */
static inline size_t web_ctrl_build(uint8_t *out, size_t cap, uint8_t type, const char *word)
{
	size_t n = strlen(word);

	if (cap < WEB_HDR_LEN || n > cap - WEB_HDR_LEN)
		return 0;
	web_hdr_put(out, type, 0, 0);
	memcpy(out + WEB_HDR_LEN, word, n);
	return WEB_HDR_LEN + n;
}

/* ---- WebSocket framing (RFC 6455 §5) ----------------------------------------------------------------------------- */

/* opcodes this build reads and writes; a text frame is not this protocol's */
#define WS_CONT		0x0
#define WS_TEXT		0x1
#define WS_BINARY	0x2
#define WS_CLOSE	0x8
#define WS_PING		0x9
#define WS_PONG		0xa

struct ws_frame {
	int fin;
	uint8_t opcode;
	int masked;
	uint64_t len;
	size_t header;		/* the bytes of the frame's header, mask included */
	uint8_t mask[4];
};

/* Parse one frame's header. 1 = complete header in f, 0 = more bytes needed, -EBADMSG = not this protocol,
 * -EMSGSIZE = a length this build will not take. A client's data frame must be masked (RFC 6455 §5.1); a control frame may not
 * be fragmented and may not carry more than 125 bytes (RFC 6455 §5.5). */
static inline int ws_frame_parse(const uint8_t *b, size_t n, struct ws_frame *f)
{
	uint8_t b0, b1;
	uint64_t len;
	size_t need;

	if (n < 2)
		return 0;
	b0 = b[0];
	b1 = b[1];
	f->fin = (b0 & 0x80) != 0;
	f->opcode = b0 & 0x0f;
	f->masked = (b1 & 0x80) != 0;
	len = b1 & 0x7f;
	need = 2;
	if (len == 126) {
		if (n < 4)
			return 0;
		len = ((uint64_t)b[2] << 8) | b[3];
		need = 4;
	} else if (len == 127) {
		int k;

		if (n < 10)
			return 0;
		len = 0;
		for (k = 0; k < 8; k++)
			len = (len << 8) | b[2 + k];
		need = 10;
	}
	f->len = len;
	/* only the four data/control opcodes of RFC 6455 §5.2 and §5.5 are taken, and never a reserved opcode */
	if (f->opcode != WS_CONT && f->opcode != WS_TEXT && f->opcode != WS_BINARY &&
	    f->opcode != WS_CLOSE && f->opcode != WS_PING && f->opcode != WS_PONG)
		return -EBADMSG;
	if (f->opcode >= 0x8 && (!f->fin || len > 125))
		return -EBADMSG;		/* a control frame is whole and short */
	if (len > WEB_FRAME_MAX)
		return -EMSGSIZE;
	if (f->masked) {
		if (n < need + 4)
			return 0;
		memcpy(f->mask, b + need, 4);
		need += 4;
	}
	f->header = need;
	return 1;
}

static inline void ws_unmask(uint8_t *payload, size_t len, const uint8_t mask[4])
{
	size_t k;

	for (k = 0; k < len; k++)
		payload[k] ^= mask[k & 3];
}

/* One whole, unmasked, binary frame into out (the server never masks, RFC 6455 §5.1): the header plus the payload.
 * Returns the bytes written, 0 when the buffer is too small. */
static inline size_t ws_frame_build(uint8_t *out, size_t cap, uint8_t opcode, const uint8_t *payload, size_t len)
{
	size_t need = 2 + len, k;

	if (len > 125)
		need += 2;
	if (cap < need)
		return 0;
	out[0] = (uint8_t)(0x80 | opcode);	/* FIN, no extension: this build never fragments */
	if (len <= 125) {
		out[1] = (uint8_t)len;
		k = 2;
	} else {
		out[1] = 126;
		out[2] = (uint8_t)(len >> 8);
		out[3] = (uint8_t)len;
		k = 4;
	}
	if (payload && len)
		memcpy(out + k, payload, len);
	return k + len;
}

/* ---- the HTTP upgrade (RFC 6455 §4.1-4.2) ------------------------------------------------------------------------ */

#define WEB_HEADER_MAX		2048	/* a request header larger than this is not one this build wrote */

struct web_http {
	int upgrade;			/* 1 = a WebSocket upgrade request */
	char key[32];			/* Sec-WebSocket-Key's value, as sent */
	size_t len;			/* the header's bytes, the blank line included */
};

/* A case-insensitive "name: value" lookup inside one request header; the value is trimmed. Returns 1 when present. */
static inline int http_header(const char *head, size_t len, const char *name, char *out, size_t cap)
{
	size_t nlen = strlen(name), k = 0;

	while (k + nlen + 1 < len) {
		size_t start, end;
		const char *p = head + k;

		/* a header line begins at the start of the request or after a CRLF */
		if (k && !(head[k - 1] == '\n'))
			goto next;
		if (strncasecmp(p, name, nlen) || p[nlen] != ':')
			goto next;
		start = k + nlen + 1;
		while (start < len && (head[start] == ' ' || head[start] == '\t'))
			start++;
		end = start;
		while (end < len && head[end] != '\r' && head[end] != '\n')
			end++;
		while (end > start && (head[end - 1] == ' ' || head[end - 1] == '\t'))
			end--;			/* optional whitespace around a value (RFC 7230 §3.2.4) */
		if (end - start >= cap)
			return 0;
		memcpy(out, head + start, end - start);
		out[end - start] = 0;
		return 1;
next:
		while (k < len && head[k] != '\n')
			k++;
		if (k < len)
			k++;
	}
	return 0;
}

/* Parse a request head that ends with a blank line. 1 = a WebSocket upgrade for this build, 0 = more bytes needed,
 * -EINVAL = a plain request (the listener answers its note page), -EPROTO = malformed or a version this build does
 * not speak, -EMSGSIZE = a header beyond WEB_HEADER_MAX. */
static inline int web_http_parse(const char *head, size_t n, struct web_http *r)
{
	size_t end = 0, k;
	char v[64];

	memset(r, 0, sizeof(*r));
	if (n > WEB_HEADER_MAX)
		return -EMSGSIZE;
	for (k = 0; k + 3 < n; k++) {
		if (head[k] == '\r' && head[k + 1] == '\n' && head[k + 2] == '\r' && head[k + 3] == '\n') {
			end = k + 4;
			break;
		}
	}
	if (!end)
		return 0;
	r->len = end;
	if (end < 4 || strncmp(head, "GET ", 4))
		return -EINVAL;
	if (!http_header(head, end, "Upgrade", v, sizeof(v)) || strcasecmp(v, "websocket"))
		return -EINVAL;
	if (!http_header(head, end, "Sec-WebSocket-Version", v, sizeof(v)) || strcmp(v, "13"))
		return -EPROTO;
	if (!http_header(head, end, "Sec-WebSocket-Key", r->key, sizeof(r->key)))
		return -EPROTO;
	if (strlen(r->key) < 16 || strlen(r->key) > 24)		/* base64 of 16 bytes, as RFC 6455 §4.1 asks */
		return -EPROTO;
	r->upgrade = 1;
	return 1;
}

/* ---- SHA-1 and base64 for Sec-WebSocket-Accept (RFC 6455 §4.2.2) -------------------------------------------------- */

/* SHA-1 (FIPS 180-4), written from the standard: the handshake's accept value is the protocol's, not a security
 * choice of this implementation (RFC 6455 §4.2.2 mandates it). */
struct web_sha1 {
	uint32_t h[5];
	uint64_t bits;
	uint8_t block[64];
	size_t used;
};

static inline uint32_t web_rol(uint32_t v, unsigned n)
{
	return (v << n) | (v >> (32 - n));
}

static inline void web_sha1_init(struct web_sha1 *s)
{
	s->h[0] = 0x67452301u;
	s->h[1] = 0xefcdab89u;
	s->h[2] = 0x98badcfeu;
	s->h[3] = 0x10325476u;
	s->h[4] = 0xc3d2e1f0u;
	s->bits = 0;
	s->used = 0;
}

static inline void web_sha1_block(struct web_sha1 *s, const uint8_t *p)
{
	uint32_t w[80], a, b, c, d, e;
	int k;

	for (k = 0; k < 16; k++)
		w[k] = ((uint32_t)p[4 * k] << 24) | ((uint32_t)p[4 * k + 1] << 16) |
			((uint32_t)p[4 * k + 2] << 8) | (uint32_t)p[4 * k + 3];
	for (k = 16; k < 80; k++)
		w[k] = web_rol(w[k - 3] ^ w[k - 8] ^ w[k - 14] ^ w[k - 16], 1);
	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
	for (k = 0; k < 80; k++) {
		uint32_t f, kk, t;

		if (k < 20) {
			f = (b & c) | (~b & d);
			kk = 0x5a827999u;
		} else if (k < 40) {
			f = b ^ c ^ d;
			kk = 0x6ed9eba1u;
		} else if (k < 60) {
			f = (b & c) | (b & d) | (c & d);
			kk = 0x8f1bbcdcu;
		} else {
			f = b ^ c ^ d;
			kk = 0xca62c1d6u;
		}
		t = web_rol(a, 5) + f + e + kk + w[k];
		e = d; d = c; c = web_rol(b, 30); b = a; a = t;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static inline void web_sha1_update(struct web_sha1 *s, const uint8_t *p, size_t n)
{
	s->bits += (uint64_t)n * 8u;
	while (n) {
		size_t take = 64 - s->used;

		if (take > n)
			take = n;
		memcpy(s->block + s->used, p, take);
		s->used += take;
		p += take;
		n -= take;
		if (s->used == 64) {
			web_sha1_block(s, s->block);
			s->used = 0;
		}
	}
}

static inline void web_sha1_final(struct web_sha1 *s, uint8_t out[20])
{
	uint64_t bits = s->bits;
	uint8_t pad = 0x80;
	int k;

	web_sha1_update(s, &pad, 1);
	pad = 0;
	while (s->used != 56)
		web_sha1_update(s, &pad, 1);
	for (k = 7; k >= 0; k--) {
		uint8_t b = (uint8_t)(bits >> (8 * k));

		web_sha1_update(s, &b, 1);
	}
	for (k = 0; k < 5; k++) {
		out[4 * k] = (uint8_t)(s->h[k] >> 24);
		out[4 * k + 1] = (uint8_t)(s->h[k] >> 16);
		out[4 * k + 2] = (uint8_t)(s->h[k] >> 8);
		out[4 * k + 3] = (uint8_t)s->h[k];
	}
}

/* standard base64 (§4 of RFC 4648), with padding; returns the characters written, 0 when cap is too small */
static inline size_t web_b64(const uint8_t *in, size_t n, char *out, size_t cap)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t k, o = 0;

	if (cap < (n + 2) / 3 * 4 + 1)
		return 0;
	for (k = 0; k + 2 < n; k += 3) {
		uint32_t v = ((uint32_t)in[k] << 16) | ((uint32_t)in[k + 1] << 8) | in[k + 2];

		out[o++] = alphabet[(v >> 18) & 63];
		out[o++] = alphabet[(v >> 12) & 63];
		out[o++] = alphabet[(v >> 6) & 63];
		out[o++] = alphabet[v & 63];
	}
	if (k < n) {
		uint32_t v = (uint32_t)in[k] << 16;

		if (k + 1 < n)
			v |= (uint32_t)in[k + 1] << 8;
		out[o++] = alphabet[(v >> 18) & 63];
		out[o++] = alphabet[(v >> 12) & 63];
		out[o++] = k + 1 < n ? alphabet[(v >> 6) & 63] : '=';
		out[o++] = '=';
	}
	out[o] = 0;
	return o;
}

/* Sec-WebSocket-Accept: base64(SHA-1(key + the protocol's GUID)), RFC 6455 §4.2.2. 0 when out is too small. */
static inline int web_accept(const char *key, char out[29])
{
	static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	struct web_sha1 s;
	uint8_t digest[20];

	web_sha1_init(&s);
	web_sha1_update(&s, (const uint8_t *)key, strlen(key));
	web_sha1_update(&s, (const uint8_t *)guid, sizeof(guid) - 1);
	web_sha1_final(&s, digest);
	return web_b64(digest, sizeof(digest), out, 29) == 28;
}

/* the token a page presents, compared over its whole length */
static inline int web_token_eq(const char *expected, const char *got)
{
	size_t n = strlen(expected);

	return strlen(got) == n && !memcmp(expected, got, n);
}

/* whether a phase that began at `since` has run past `limit`, read on one clock. the guard is the point: a phase can
 * begin after the reading the caller holds (the request that opens it reads the clock a moment later, and the loop's
 * own reading is older), and a plain unsigned subtraction would then wrap to a huge value and end the phase at once */
static inline int web_ms_past(unsigned long long now, unsigned long long since, unsigned long long limit)
{
	return now > since && now - since > limit;
}

#endif /* AW_DAEMON_WEB_PROTO_H */
