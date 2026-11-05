/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
/*
 * The browser-audio wire: the frame header, the WebSocket framing of RFC 6455, the HTTP upgrade and its SHA-1 accept
 * value. The vectors are the protocol's own (RFC 6455 §1.3 and §5.7, FIPS 180-4). make -f Makefile.host test
 */
#include <assert.h>
#include <stdio.h>

#include "web_proto.h"

static void test_sha1(void)
{
	struct web_sha1 s;
	uint8_t out[20];
	char hex[41];
	int k;

	/* FIPS 180-4's own vectors */
	web_sha1_init(&s);
	web_sha1_update(&s, (const uint8_t *)"abc", 3);
	web_sha1_final(&s, out);
	for (k = 0; k < 20; k++)
		snprintf(hex + 2 * k, 3, "%02x", out[k]);
	assert(!strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d"));

	web_sha1_init(&s);
	web_sha1_update(&s, (const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
	web_sha1_final(&s, out);
	for (k = 0; k < 20; k++)
		snprintf(hex + 2 * k, 3, "%02x", out[k]);
	assert(!strcmp(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));

	/* a length that pads across a block boundary (55, 56, 64 and 119 bytes) */
	{
		const char *msg = "0123456789012345678901234567890123456789012345678901234567890123456789"
			"0123456789012345678901234567890123456789012345678";

		web_sha1_init(&s);
		web_sha1_update(&s, (const uint8_t *)msg, 55);
		web_sha1_final(&s, out);
		web_sha1_init(&s);
		web_sha1_update(&s, (const uint8_t *)msg, 56);
		web_sha1_final(&s, out);
		web_sha1_init(&s);
		web_sha1_update(&s, (const uint8_t *)msg, 64);
		web_sha1_final(&s, out);
		web_sha1_init(&s);
		web_sha1_update(&s, (const uint8_t *)msg, 119);
		web_sha1_final(&s, out);
	}
}

static void test_accept(void)
{
	char out[29];

	/* RFC 6455 §1.3: the worked example */
	assert(web_accept("dGhlIHNhbXBsZSBub25jZQ==", out));
	assert(!strcmp(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
	assert(!web_accept("dGhlIHNhbXBsZSBub25jZQ==", out) || strlen(out) == 28);
}

static void test_frames(void)
{
	uint8_t b[16];
	uint8_t buf[WEB_FRAME_MAX + 8];
	struct ws_frame f;
	size_t n;
	uint32_t seq, ts;
	uint8_t type;

	/* the 12-byte header travels big-endian and comes back */
	web_hdr_put(b, WEB_T_PCM, 0x01020304u, 0x0a0b0c0du);
	assert(b[0] == 'A' && b[1] == 1 && b[2] == WEB_T_PCM && b[3] == 0);
	assert(b[4] == 1 && b[5] == 2 && b[6] == 3 && b[7] == 4);
	assert(b[8] == 0x0a && b[11] == 0x0d);
	assert(web_hdr_get(b, &type, &seq, &ts) && type == WEB_T_PCM && seq == 0x01020304u && ts == 0x0a0b0c0du);
	b[1] = 2;
	assert(!web_hdr_get(b, &type, &seq, &ts));
	b[1] = 1;
	b[0] = 'B';
	assert(!web_hdr_get(b, &type, &seq, &ts));

	/* 16-bit samples in network order */
	web_s16be_put(b, -2);
	assert(b[0] == 0xff && b[1] == 0xfe && web_s16be_get(b) == -2);
	web_s16be_put(b, 0x1234);
	assert(b[0] == 0x12 && b[1] == 0x34 && web_s16be_get(b) == 0x1234);

	/* a masked client binary frame of three bytes: RFC 6455 §5.7's "Hello" is five, this is the shape */
	buf[0] = 0x82; buf[1] = 0x83; buf[2] = 1; buf[3] = 2; buf[4] = 3; buf[5] = 4;
	buf[6] = 'a' ^ 1; buf[7] = 'b' ^ 2; buf[8] = 'c' ^ 3;
	assert(ws_frame_parse(buf, 2, &f) == 0);
	assert(ws_frame_parse(buf, 5, &f) == 0);
	assert(ws_frame_parse(buf, 9, &f) == 1);
	assert(f.fin && f.opcode == WS_BINARY && f.masked && f.len == 3 && f.header == 6);
	ws_unmask(buf + 6, 3, f.mask);
	assert(buf[6] == 'a' && buf[7] == 'b' && buf[8] == 'c');

	/* a client data frame may not be unmasked (RFC 6455 §5.1, the daemon refuses it later); the parser still reads it */
	buf[1] = 0x03;
	assert(ws_frame_parse(buf, 5, &f) == 1 && !f.masked && f.len == 3);

	/* a fragmented or oversized control frame, a reserved opcode and an oversized data frame are refusals */
	buf[0] = 0x09; buf[1] = 0x80;			/* ping, not final */
	assert(ws_frame_parse(buf, 2, &f) == -EBADMSG);
	buf[0] = 0x89; buf[1] = 0x80 | 126; buf[2] = 0; buf[3] = 126;
	assert(ws_frame_parse(buf, 4, &f) == -EBADMSG);
	buf[0] = 0x8f; buf[1] = 0x80;
	assert(ws_frame_parse(buf, 2, &f) == -EBADMSG);	/* opcode 15 is reserved */
	buf[0] = 0x82; buf[1] = 0x80 | 126; buf[2] = 0x10; buf[3] = 0x00;	/* 4096 bytes */
	assert(ws_frame_parse(buf, 4, &f) == -EMSGSIZE);
	/* 64-bit lengths can arrive in a header this build reads before the length check */
	buf[0] = 0x82; buf[1] = 0x80 | 127;
	for (n = 0; n < 8; n++)
		buf[2 + n] = 0;
	assert(ws_frame_parse(buf, 10, &f) == 0);	/* mask bytes still missing */
	buf[10] = 1; buf[11] = 2; buf[12] = 3; buf[13] = 4;
	assert(ws_frame_parse(buf, 14, &f) == 1 && f.len == 0 && f.masked);

	/* the server's own frame: unmasked, binary, a short payload and a long one */
	n = ws_frame_build(buf, sizeof(buf), WS_BINARY, (const uint8_t *)"hi", 2);
	assert(n == 4 && buf[0] == 0x82 && buf[1] == 2 && buf[2] == 'h' && buf[3] == 'i');
	{
		uint8_t pcm[WEB_PCM_BYTES + WEB_HDR_LEN];

		memset(pcm, 0x5a, sizeof(pcm));
		n = ws_frame_build(buf, sizeof(buf), WS_BINARY, pcm, sizeof(pcm));
		assert(n == 4 + sizeof(pcm) && buf[1] == 126 && buf[2] == (uint8_t)(sizeof(pcm) >> 8) && buf[3] == (uint8_t)sizeof(pcm));
	}
	/* a payload too large for the buffer writes nothing */
	assert(ws_frame_build(buf, 8, WS_BINARY, (const uint8_t *)"0123456789", 10) == 0);
}

static void test_http(void)
{
	static const char up[] =
		"GET /audio HTTP/1.1\r\n"
		"Host: router.local:7443\r\n"
		"Upgrade: WebSocket\r\n"
		"Connection: keep-alive, Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"\r\n";
	struct web_http r;

	assert(web_http_parse(up, 20, &r) == 0);
	assert(web_http_parse(up, sizeof(up) - 1, &r) == 1);
	assert(r.upgrade == 1 && !strcmp(r.key, "dGhlIHNhbXBsZSBub25jZQ=="));
	assert(r.len == sizeof(up) - 1);

	/* a plain GET is the note page, not an upgrade */
	{
		static const char plain[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";

		assert(web_http_parse(plain, sizeof(plain) - 1, &r) == -EINVAL);
	}
	/* a missing version, a wrong version, a missing key: refusals of the protocol, not a note page */
	{
		static const char nover[] = "GET /audio HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Key: 0123456789abcdef\r\n\r\n";
		static const char badver[] = "GET /audio HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 8\r\n"
			"Sec-WebSocket-Key: 0123456789abcdef\r\n\r\n";
		static const char nokey[] = "GET /audio HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n\r\n";
		static const char shortkey[] = "GET /audio HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
			"Sec-WebSocket-Key: short\r\n\r\n";
		static const char post[] = "POST /audio HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
			"Sec-WebSocket-Key: 0123456789abcdef\r\n\r\n";

		assert(web_http_parse(nover, sizeof(nover) - 1, &r) == -EPROTO);
		assert(web_http_parse(badver, sizeof(badver) - 1, &r) == -EPROTO);
		assert(web_http_parse(nokey, sizeof(nokey) - 1, &r) == -EPROTO);
		assert(web_http_parse(shortkey, sizeof(shortkey) - 1, &r) == -EPROTO);
		assert(web_http_parse(post, sizeof(post) - 1, &r) == -EINVAL);
	}
	/* a header beyond the limit is refused, not read */
	{
		char big[WEB_HEADER_MAX + 2];

		memset(big, 'A', sizeof(big));
		assert(web_http_parse(big, sizeof(big), &r) == -EMSGSIZE);
	}

	/* header names are case-insensitive and values may be padded (RFC 6455 §4.1) */
	{
		static const char odd[] = "GET /audio HTTP/1.1\r\nupgrade:   WEBSOCKET  \r\n"
			"sec-websocket-version:13\r\nSEC-WEBSOCKET-KEY:\t0123456789abcdef\r\n\r\n";

		assert(web_http_parse(odd, sizeof(odd) - 1, &r) == 1 && !strcmp(r.key, "0123456789abcdef"));
	}

	/* tokens compare whole */
	assert(web_token_eq("abcdef", "abcdef"));
	assert(!web_token_eq("abcdef", "abcde"));
	assert(!web_token_eq("abcdef", "abcdeg"));
	assert(!web_token_eq("abcdef", "abcdefg"));
}

/* a phase times out only after the limit, and never because its start is newer than the caller's clock reading */
static void test_ms_past(void)
{
	assert(!web_ms_past(1000, 1000, 5000));		/* just begun */
	assert(!web_ms_past(1000, 4000, 5000));		/* the phase began after the reading: nothing has run */
	assert(!web_ms_past(6000, 1000, 5000));		/* exactly the limit is not past it */
	assert(web_ms_past(6001, 1000, 5000));		/* one millisecond past it */
	assert(web_ms_past(90000, 1000, 5000));
	assert(!web_ms_past(0, 0, 0));
	assert(web_ms_past(1, 0, 0));
}

/* a control word travels with the twelve-byte header, exactly as the page reads it: the type is the header's, never
 * the word's bytes, and a word that travels bare is a frame the page drops */
static void test_ctrl(void)
{
	uint8_t b[64], type = 0;
	uint32_t seq = 9, ts = 9;
	size_t n;

	n = web_ctrl_build(b, sizeof(b), WEB_T_GRANT, "granted");
	assert(n == WEB_HDR_LEN + 7);
	assert(web_hdr_get(b, &type, &seq, &ts) && type == WEB_T_GRANT);
	assert(seq == 0 && ts == 0);
	assert(!memcmp(b + WEB_HDR_LEN, "granted", 7));

	n = web_ctrl_build(b, sizeof(b), WEB_T_GRANT, "refused-revision");
	assert(n == WEB_HDR_LEN + 16);
	assert(web_hdr_get(b, &type, &seq, &ts) && type == WEB_T_GRANT);
	assert(!memcmp(b + WEB_HDR_LEN, "refused-revision", 16));

	n = web_ctrl_build(b, sizeof(b), WEB_T_BYE, "");
	assert(n == WEB_HDR_LEN);
	assert(web_hdr_get(b, &type, &seq, &ts) && type == WEB_T_BYE);

	assert(web_ctrl_build(b, WEB_HDR_LEN - 1, WEB_T_GRANT, "granted") == 0);
	assert(web_ctrl_build(b, WEB_HDR_LEN + 3, WEB_T_GRANT, "granted") == 0);
	assert(web_ctrl_build(b, sizeof(b), WEB_T_GRANT, "granted") > WEB_HDR_LEN);
}

int main(void)
{
	test_sha1();
	test_accept();
	test_frames();
	test_http();
	test_ctrl();
	test_ms_past();
	printf("web_proto: all tests passed\n");
	return 0;
}
