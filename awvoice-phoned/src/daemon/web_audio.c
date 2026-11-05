/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
/* The page's browser audio (web_audio.h): the TLS listener's session, its frames and its one-owner grant. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>

#include "web_audio.h"
#include "web_proto.h"
#include "web_tls.h"

#define WEB_TOKEN_MS		30000u		/* a minted token's life: long enough to connect, short enough to be dead if leaked */
#define WEB_HEAD_MS		5000u		/* the request after the handshake, and the HELLO after the 101 */
#define WEB_PAGE_MS		10000u		/* a note page that cannot be written in this time is a client that left */
#define WEB_IDLE_MS		30000u		/* no frame from the page for this long: the stream is gone */
#define WEB_PING_MS		10000u		/* a ping while the page is quiet */
#define WEB_OUT_MAX		8192		/* the outgoing queue: an info page, or a few PCM frames */
#define WEB_PLAY_FRAMES		8		/* the page's uplink queue: 160 ms of speech */
#define WEB_TOKEN_HEX		(32 * 2)

enum web_phase {
	WEB_PHASE_CLOSED,
	WEB_PHASE_HELLO,		/* TLS is ready: the HTTP request travels in */
	WEB_PHASE_PAGE,			/* the note page was asked for: the answer is on its way, then the connection ends */
	WEB_PHASE_WS,			/* the stream is up: the token is waited for */
	WEB_PHASE_GRANTED,
};

struct web_audio {
	struct web_tls *tls;
	struct web_audio_ops ops;
	int listen_up;
	enum web_phase phase;
	unsigned long long phase_ms;		/* when the phase began: the timeouts count from it */
	unsigned long long last_rx_ms;
	unsigned long long last_ping_ms;
	int page_close;				/* the note page or an error page is out: end after the flush */

	uint8_t in[WEB_HEADER_MAX + 64];
	size_t in_len;
	uint8_t out[WEB_OUT_MAX];
	size_t out_len, out_off;
	int out_stuck_logged;

	char token[WEB_TOKEN_HEX + 1];
	uint64_t token_rev;
	unsigned long long token_ms;
	int token_live;

	unsigned int frames_out, frames_in, dropped_out, dropped_in;

	int16_t play[WEB_PLAY_FRAMES][WEB_SAMPLES];
	unsigned int play_head, play_tail;	/* the queue, tail = the next to take */
};

static unsigned long long now_ms_of(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return 0;
	return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000;
}

static void page_put(struct web_audio *w, const char *text, size_t len)
{
	if (w->out_len + len > sizeof(w->out)) {
		w->dropped_out++;
		return;
	}
	memcpy(w->out + w->out_len, text, len);
	w->out_len += len;
}

/* queue one whole WebSocket frame; a queue that cannot take it drops the frame (a page that stops reading) */
static void ws_put(struct web_audio *w, uint8_t opcode, const uint8_t *payload, size_t len)
{
	uint8_t frame[WEB_HDR_LEN + WEB_PCM_BYTES + 8];
	size_t n = ws_frame_build(frame, sizeof(frame), opcode, payload, len);

	if (!n || w->out_len + n > sizeof(w->out)) {
		w->dropped_out++;
		return;
	}
	memcpy(w->out + w->out_len, frame, n);
	w->out_len += n;
}

/* a control word on the stream: the protocol's header, then the word, in one binary frame (the page reads the type
 * from the header, never from the word's bytes) */
static void ws_text(struct web_audio *w, uint8_t type, const char *word)
{
	uint8_t frame[WEB_HDR_LEN + 64];
	size_t n = web_ctrl_build(frame, sizeof(frame), type, word);

	if (n)
		ws_put(w, WS_BINARY, frame, n);
}

static const char note_page[] =
	"<!doctype html><html><head><meta charset=\"utf-8\"><title>AWvoice secure audio</title>"
	"<style>body{font:15px/1.5 system-ui,sans-serif;margin:3em auto;max-width:34em;padding:0 1em;color:#222}"
	"</style></head><body>"
	"<h1>AWvoice secure audio</h1>"
	"<p>This address carries the phone page's encrypted audio stream. It has no page of its own: the audio is "
	"offered on the Overview tab of the phone page while a call is up.</p>"
	"<p>Seeing this page means the certificate of this router is trusted for this address; the phone page can "
	"now use it. A replaced certificate asks once more.</p></body></html>";

/* the connection is over: the queue and the transport go; a granted session is told exactly once */
static void release_session(struct web_audio *w)
{
	int granted = w->phase == WEB_PHASE_GRANTED;

	w->phase = WEB_PHASE_CLOSED;
	w->page_close = 0;
	w->in_len = 0;
	w->out_len = w->out_off = 0;
	w->play_head = w->play_tail = 0;
	web_tls_drop(w->tls);
	if (granted && w->ops.release)
		w->ops.release(w->ops.ctx);
}

static const char *refusal_for(int err)
{
	switch (err) {
	case -EBUSY:
		return "refused-busy";
	case -ENOENT:
		return "refused-nocall";
	case -ESTALE:
		return "refused-revision";
	case -EPERM:
		return "refused-mode";
	}
	return "NOT_AVAILABLE";
}

/* a plain request, or a request this build will not upgrade: one small page, then the connection ends */
static void serve_page(struct web_audio *w, const char *status, const char *extra)
{
	char head[256];
	int n = snprintf(head, sizeof(head),
		"HTTP/1.1 %s\r\n%sContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n",
		status, extra ? extra : "", sizeof(note_page) - 1);

	if (n > 0 && (size_t)n < sizeof(head))
		page_put(w, head, (size_t)n);
	page_put(w, note_page, sizeof(note_page) - 1);
	w->phase = WEB_PHASE_PAGE;
	w->phase_ms = now_ms_of();
	w->page_close = 1;
	w->in_len = 0;
}

static void ws_upgrade(struct web_audio *w, const struct web_http *req)
{
	char accept[29], answer[256];

	if (!web_accept(req->key, accept)) {
		serve_page(w, "400 Bad Request", NULL);
		return;
	}
	snprintf(answer, sizeof(answer),
		"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Accept: %s\r\n\r\n", accept);
	page_put(w, answer, strlen(answer));
	w->phase = WEB_PHASE_WS;
	w->phase_ms = now_ms_of();
	w->in_len = 0;
}

void web_audio_drop(struct web_audio *w)
{
	if (w && w->phase != WEB_PHASE_CLOSED)
		release_session(w);
}

/* one frame off the stream */
static void ws_frame(struct web_audio *w, uint8_t opcode, const uint8_t *payload, size_t len)
{
	uint8_t type;
	uint32_t seq, ts;

	switch (opcode) {
	case WS_PING:
		ws_put(w, WS_PONG, payload, len);
		return;
	case WS_PONG:
		return;
	case WS_CLOSE:
		ws_put(w, WS_CLOSE, NULL, 0);
		w->page_close = 1;
		return;
	case WS_TEXT:
	case WS_CONT:
		release_session(w);		/* this protocol carries no text and never fragments */
		return;
	case WS_BINARY:
		break;
	default:
		release_session(w);
		return;
	}
	if (!web_hdr_get(payload, &type, &seq, &ts)) {
		release_session(w);
		return;
	}
	if (w->phase == WEB_PHASE_WS) {
		char got[WEB_TOKEN_MAX + 1];
		size_t n;

		if (type != WEB_T_HELLO || len <= WEB_HDR_LEN) {
			ws_text(w, WEB_T_ERROR, "hello-first");
			w->page_close = 1;
			return;
		}
		n = len - WEB_HDR_LEN;
		if (n >= sizeof(got)) {
			ws_text(w, WEB_T_ERROR, "hello-long");
			w->page_close = 1;
			return;
		}
		memcpy(got, payload + WEB_HDR_LEN, n);
		got[n] = 0;
		/* a stale or unknown token is the same answer to a page that lost its place: it asks for another */
		if (!w->token_live || now_ms_of() > w->token_ms + WEB_TOKEN_MS || !web_token_eq(w->token, got)) {
			w->token_live = 0;
			ws_text(w, WEB_T_GRANT, "refused-revision");
			w->page_close = 1;
			return;
		}
		{
			int err = w->ops.grant ? w->ops.grant(w->ops.ctx, w->token_rev) : -EPERM;

			w->token_live = 0;	/* one stream, one token */
			if (err) {
				ws_text(w, WEB_T_GRANT, refusal_for(err));
				w->page_close = 1;
				return;
			}
		}
		ws_text(w, WEB_T_GRANT, "granted");
		w->phase = WEB_PHASE_GRANTED;
		w->phase_ms = now_ms_of();
		w->last_rx_ms = w->phase_ms;
		w->last_ping_ms = w->phase_ms;
		w->play_head = w->play_tail = 0;
		w->frames_in = w->frames_out = 0;
		return;
	}
	if (w->phase != WEB_PHASE_GRANTED)
		return;
	if (type == WEB_T_BYE) {
		ws_put(w, WS_CLOSE, NULL, 0);
		w->page_close = 1;
		return;
	}
	if (type == WEB_T_MUTE)
		return;				/* the page stopped sending; the downlink goes on */
	if (type != WEB_T_PCM || len != WEB_HDR_LEN + WEB_PCM_BYTES) {
		w->dropped_in++;
		return;
	}
	if (w->play_tail - w->play_head >= WEB_PLAY_FRAMES)
		w->play_head++;			/* the page outran the line: the oldest frame goes */
	{
		unsigned int slot = w->play_tail % WEB_PLAY_FRAMES;
		unsigned int k;

		for (k = 0; k < WEB_SAMPLES; k++)
			w->play[slot][k] = web_s16be_get(payload + WEB_HDR_LEN + 2 * k);
		w->play_tail++;
		w->frames_in++;
	}
	(void)seq;
	(void)ts;
}

/* frames out of the input buffer; a partial frame stays for the next read */
static void ws_consume(struct web_audio *w)
{
	size_t off = 0;

	while (off < w->in_len) {
		struct ws_frame f;
		int r = ws_frame_parse(w->in + off, w->in_len - off, &f);

		if (r == 0)
			break;
		if (r < 0 || !f.masked) {
			release_session(w);	/* a client's data frames are masked (RFC 6455 §5.1) */
			return;
		}
		if (w->in_len - off < f.header + f.len)
			break;
		ws_unmask(w->in + off + f.header, (size_t)f.len, f.mask);
		ws_frame(w, f.opcode, w->in + off + f.header, (size_t)f.len);
		if (w->phase == WEB_PHASE_CLOSED)
			return;
		off += f.header + (size_t)f.len;
	}
	if (off) {
		memmove(w->in, w->in + off, w->in_len - off);
		w->in_len -= off;
	}
}

static void flush_out(struct web_audio *w)
{
	while (w->out_off < w->out_len) {
		ssize_t n = web_tls_write(w->tls, w->out + w->out_off, w->out_len - w->out_off);

		if (n < 0) {
			release_session(w);
			return;
		}
		if (!n) {
			if (!w->out_stuck_logged)
				w->dropped_out++;
			w->out_stuck_logged = 1;
			return;
		}
		w->out_stuck_logged = 0;
		w->out_off += (size_t)n;
	}
	w->out_len = w->out_off = 0;
	if (w->page_close)
		release_session(w);
}

int web_audio_open(struct web_audio **out, const struct web_audio_ops *ops, const char *addr, unsigned port,
	const char *cert, const char *key, FILE *log)
{
	struct web_audio *w;
	int err;

	*out = NULL;
	w = calloc(1, sizeof(*w));
	if (!w)
		return -ENOMEM;
	w->ops = *ops;
	err = web_tls_listen(&w->tls, addr, port, cert, key, log);
	if (err) {
		free(w);
		return err;
	}
	w->listen_up = 1;
	w->phase = WEB_PHASE_CLOSED;
	*out = w;
	return 0;
}

void web_audio_close(struct web_audio **out)
{
	if (!out || !*out)
		return;
	release_session(*out);
	web_tls_free(&(*out)->tls);
	free(*out);
	*out = NULL;
}

int web_audio_fd(const struct web_audio *w)
{
	return w ? web_tls_fd(w->tls) : -1;
}

int web_audio_accept(struct web_audio *w, unsigned long long now_ms)
{
	int r;

	if (!w)
		return 0;
	r = web_tls_accept(w->tls, now_ms);
	if (r == 1) {
		w->phase = WEB_PHASE_HELLO;
		w->phase_ms = now_ms;
		w->in_len = 0;
		w->out_len = w->out_off = 0;
		w->page_close = 0;
	}
	return r;
}

int web_audio_granted(const struct web_audio *w)
{
	return w && w->phase == WEB_PHASE_GRANTED;
}

int web_audio_mint(struct web_audio *w, uint64_t revision, unsigned long long now_ms, char *token, size_t cap)
{
	static const char hex[] = "0123456789abcdef";
	unsigned char raw[32];
	size_t k;

	if (!w || !w->listen_up || cap < WEB_TOKEN_HEX + 1)
		return -ENODEV;
	if (getentropy(raw, sizeof(raw)))
		return -EIO;
	for (k = 0; k < sizeof(raw); k++) {
		token[2 * k] = hex[raw[k] >> 4];
		token[2 * k + 1] = hex[raw[k] & 0x0f];
	}
	token[WEB_TOKEN_HEX] = 0;
	snprintf(w->token, sizeof(w->token), "%s", token);
	w->token_rev = revision;
	w->token_ms = now_ms ? now_ms : now_ms_of();
	w->token_live = 1;
	return 0;
}

void web_audio_capture(struct web_audio *w, const int16_t *samples, unsigned count, uint32_t sequence)
{
	uint8_t payload[WEB_HDR_LEN + WEB_PCM_BYTES];
	unsigned k;

	if (!web_audio_granted(w) || count != WEB_SAMPLES)
		return;
	web_hdr_put(payload, WEB_T_PCM, sequence, sequence * WEB_FRAME_MS);
	for (k = 0; k < WEB_SAMPLES; k++)
		web_s16be_put(payload + WEB_HDR_LEN + 2 * k, samples[k]);
	ws_put(w, WS_BINARY, payload, sizeof(payload));
	w->frames_out++;
}

int web_audio_take_playback(struct web_audio *w, int16_t *out, unsigned count)
{
	unsigned int slot;

	if (!web_audio_granted(w) || count != WEB_SAMPLES || w->play_tail == w->play_head)
		return 0;
	slot = w->play_head % WEB_PLAY_FRAMES;
	memcpy(out, w->play[slot], sizeof(w->play[slot]));
	w->play_head++;
	return 1;
}

const char *web_audio_state(const struct web_audio *w)
{
	if (!w || !w->listen_up)
		return "off";
	if (w->phase == WEB_PHASE_GRANTED)
		return "connected";
	return "listening";
}

void web_audio_tick(struct web_audio *w, unsigned long long now_ms)
{
	ssize_t n;
	int r;

	if (!w || !w->listen_up)
		return;
	if (!web_tls_busy(w->tls)) {
		/* a connection that waited behind the last one is taken now, without waiting for a new event */
		(void)web_audio_accept(w, now_ms);
		return;
	}
	if (w->phase == WEB_PHASE_HELLO) {
		r = web_tls_shake(w->tls, now_ms);
		if (r == 0)
			return;
		if (r < 0) {
			w->phase = WEB_PHASE_CLOSED;
			w->in_len = 0;
			w->out_len = w->out_off = 0;
			return;
		}
	}
	/* the request, then the frames */
	for (r = 0; r < 8; r++) {
		if (w->phase == WEB_PHASE_CLOSED || w->phase == WEB_PHASE_PAGE || w->page_close)
			break;
		if (w->in_len >= sizeof(w->in)) {
			release_session(w);
			return;
		}
		n = web_tls_read(w->tls, w->in + w->in_len, sizeof(w->in) - w->in_len);
		if (n < 0) {
			release_session(w);
			return;
		}
		if (!n)
			break;
		w->in_len += (size_t)n;
		w->last_rx_ms = now_ms;
		if (w->phase == WEB_PHASE_HELLO) {
			struct web_http req;
			int p = web_http_parse((const char *)w->in, w->in_len, &req);

			if (p == 0)
				continue;
			if (p == 1)
				ws_upgrade(w, &req);
			else if (p == -EPROTO)
				serve_page(w, "426 Upgrade Required", "Sec-WebSocket-Version: 13\r\n");
			else if (p == -EMSGSIZE)
				serve_page(w, "431 Request Header Fields Too Large", NULL);
			else
				serve_page(w, "200 OK", NULL);
			if (w->phase == WEB_PHASE_CLOSED)
				return;
			break;		/* the request's bytes are not frames */
		}
		if (w->phase == WEB_PHASE_WS || w->phase == WEB_PHASE_GRANTED)
			ws_consume(w);
		if (w->phase == WEB_PHASE_CLOSED)
			return;
	}
	if (w->phase == WEB_PHASE_HELLO && web_ms_past(now_ms, w->phase_ms, WEB_HEAD_MS)) {
		web_tls_drop(w->tls);
		w->phase = WEB_PHASE_CLOSED;
		return;
	}
	if (w->phase == WEB_PHASE_WS && web_ms_past(now_ms, w->phase_ms, WEB_HEAD_MS)) {
		ws_text(w, WEB_T_GRANT, "refused-nocall");
		w->page_close = 1;
	}
	if (w->phase == WEB_PHASE_PAGE && w->page_close && web_ms_past(now_ms, w->phase_ms, WEB_PAGE_MS)) {
		release_session(w);
		return;
	}
	if (w->phase == WEB_PHASE_GRANTED) {
		if (web_ms_past(now_ms, w->last_rx_ms, WEB_IDLE_MS)) {
			release_session(w);
			return;
		}
		if (web_ms_past(now_ms, w->last_ping_ms, WEB_PING_MS)) {
			ws_put(w, WS_PING, NULL, 0);
			w->last_ping_ms = now_ms;
		}
	}
	flush_out(w);
}
