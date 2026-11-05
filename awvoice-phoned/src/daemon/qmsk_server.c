/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "qmsk_server.h"

static void put16(unsigned char *p, u32 v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, u32 v)
{
	put16(p, v);
	put16(p + 2, v >> 16);
}

static void put64(unsigned char *p, u64 v)
{
	put32(p, (u32)v);
	put32(p + 4, (u32)(v >> 32));
}

static u32 get16(const unsigned char *p)
{
	return (u32)p[0] | (u32)p[1] << 8;
}

static u32 get32(const unsigned char *p)
{
	return get16(p) | get16(p + 2) << 16;
}

static u64 get64(const unsigned char *p)
{
	return (u64)get32(p) | (u64)get32(p + 4) << 32;
}

/* the payload length each type carries, -1 for a type this wire does not define */
static int payload_size(u16 type)
{
	switch (type) {
	case QMSK_ATTACH:
		return (int)QMSK_ATTACH_PAYLOAD;
	case QMSK_DETACH:
	case QMSK_HEARTBEAT:
		return 0;
	case QMSK_PCM_CAPTURE:
	case QMSK_PCM_PLAYBACK:
		return (int)QMSK_PCM_PAYLOAD;
	case QMSK_STATE_CHANGE:
		return (int)QMSK_STATE_PAYLOAD;
	case QMSK_ERROR:
		return (int)QMSK_ERROR_PAYLOAD;
	}
	return -1;
}

u32 qmsk_build(void *out, u16 type, const void *payload, u32 payload_length)
{
	unsigned char *p = out;
	int size = payload_size(type);

	if (size < 0 || (u32)size != payload_length || (payload_length && !payload))
		return 0;
	put32(p, QMSK_MAGIC);
	put16(p + 4, QMSK_VERSION);
	put16(p + 6, type);
	put32(p + 8, payload_length);
	if (payload_length)
		memcpy(p + QMSK_HEADER_BYTES, payload, payload_length);
	return QMSK_HEADER_BYTES + payload_length;
}

u32 qmsk_build_attach(void *out, u64 revision, const char *binding_id)
{
	unsigned char payload[QMSK_ATTACH_PAYLOAD];
	size_t n;

	if (!binding_id || !(n = strlen(binding_id)) || n > QMSK_ID_MAX)
		return 0;
	memset(payload, 0, sizeof(payload));
	put64(payload, revision);
	memcpy(payload + 8, binding_id, n);
	return qmsk_build(out, QMSK_ATTACH, payload, sizeof(payload));
}

u32 qmsk_build_detach(void *out)
{
	return qmsk_build(out, QMSK_DETACH, NULL, 0);
}

u32 qmsk_build_heartbeat(void *out)
{
	return qmsk_build(out, QMSK_HEARTBEAT, NULL, 0);
}

u32 qmsk_build_pcm(void *out, u16 type, const struct qmsk_pcm *f)
{
	unsigned char payload[QMSK_PCM_PAYLOAD];
	u32 k;

	if (!f || (type != QMSK_PCM_CAPTURE && type != QMSK_PCM_PLAYBACK))
		return 0;
	put32(payload, f->sequence);
	put32(payload + 4, f->timestamp_ms);
	for (k = 0; k < QMSK_PCM_SAMPLES; k++)
		put16(payload + 8 + 2 * k, (u16)f->sample[k]);
	return qmsk_build(out, type, payload, sizeof(payload));
}

u32 qmsk_build_state(void *out, enum fe_state state, u32 revision)
{
	unsigned char payload[QMSK_STATE_PAYLOAD];

	put32(payload, (u32)state);
	put32(payload + 4, revision);
	return qmsk_build(out, QMSK_STATE_CHANGE, payload, sizeof(payload));
}

u32 qmsk_build_error(void *out, enum fe_refusal code)
{
	unsigned char payload[QMSK_ERROR_PAYLOAD];

	put32(payload, (u32)code);
	return qmsk_build(out, QMSK_ERROR, payload, sizeof(payload));
}

int qmsk_parse(const void *buf, size_t len, struct qmsk_msg *m)
{
	const unsigned char *p = buf;
	u32 payload_length, k;
	int size, type;

	memset(m, 0, sizeof(*m));
	if (len < QMSK_HEADER_BYTES)
		return -EPROTO;		/* a message shorter than the header was cut on the way */
	if (get32(p) != QMSK_MAGIC || get16(p + 4) != QMSK_VERSION)
		return -EPROTO;
	type = (int)get16(p + 6);
	payload_length = get32(p + 8);
	size = payload_size((u16)type);
	if (size < 0 || payload_length != (u32)size || len != QMSK_HEADER_BYTES + payload_length)
		return -EPROTO;		/* a type this wire does not define, or a length that is not its own */
	m->type = (u16)type;
	m->payload_length = payload_length;
	p += QMSK_HEADER_BYTES;
	switch (m->type) {
	case QMSK_ATTACH:
		m->call_revision = get64(p);
		memcpy(m->binding_id, p + 8, sizeof(m->binding_id));
		if (m->binding_id[QMSK_ID_MAX] || !m->binding_id[0])
			return -EPROTO;	/* an id must end inside the 65 bytes and hold at least one character */
		break;
	case QMSK_PCM_CAPTURE:
	case QMSK_PCM_PLAYBACK:
		m->pcm.sequence = get32(p);
		m->pcm.timestamp_ms = get32(p + 4);
		for (k = 0; k < QMSK_PCM_SAMPLES; k++)
			m->pcm.sample[k] = (int16_t)(u16)get16(p + 8 + 2 * k);
		break;
	case QMSK_STATE_CHANGE:
		m->state = (enum fe_state)get32(p);
		m->revision = get32(p + 4);
		break;
	case QMSK_ERROR:
		m->code = (enum fe_refusal)get32(p);
		break;
	}
	return 0;
}

void qmsk_bindings_init(struct qmsk_bindings *t)
{
	memset(t, 0, sizeof(*t));
}

int qmsk_binding_live(const struct qmsk_binding *b, u64 now_ms)
{
	return b->used && now_ms < b->expires_ms;
}

struct qmsk_binding *qmsk_binding_find(struct qmsk_bindings *t, const char *id)
{
	u32 k;

	if (!id || !*id)
		return NULL;
	for (k = 0; k < QMSK_SLOTS; k++)
		if (t->slot[k].used && !strcmp(t->slot[k].id, id))
			return &t->slot[k];
	return NULL;
}

struct qmsk_binding *qmsk_binding_get(struct qmsk_bindings *t, const char *id, u64 now_ms)
{
	struct qmsk_binding *b = qmsk_binding_find(t, id);

	if (!b)
		return NULL;
	if (!qmsk_binding_live(b, now_ms)) {
		qmsk_binding_drop(b);	/* the ttl ran out: the slot is free from here */
		return NULL;
	}
	b->expires_ms = now_ms + QMSK_TTL_MS;
	return b;
}

u32 qmsk_bindings_expire(struct qmsk_bindings *t, u64 now_ms)
{
	u32 k, n = 0;

	for (k = 0; k < QMSK_SLOTS; k++) {
		if (t->slot[k].used && !qmsk_binding_live(&t->slot[k], now_ms)) {
			qmsk_binding_drop(&t->slot[k]);
			n++;
		}
	}
	return n;
}

void qmsk_binding_drop(struct qmsk_binding *b)
{
	memset(b, 0, sizeof(*b));	/* the queued call audio goes with the binding */
}

int qmsk_binding_mint(struct qmsk_bindings *t, u64 revision, u64 now_ms, char id[QMSK_ID_MAX + 1])
{
	struct qmsk_binding *b = NULL;
	u32 k;

	if (!revision)
		return -EINVAL;		/* revision 0 names no call */
	qmsk_bindings_expire(t, now_ms);
	for (k = 0; k < QMSK_SLOTS; k++) {
		if (!t->slot[k].used) {
			b = &t->slot[k];
			break;
		}
	}
	if (!b)
		return -EBUSY;
	if (!t->minted)
		t->minted = now_ms;	/* the first id sits just past the clock, so a restart does not repeat one */
	t->minted++;
	qmsk_binding_drop(b);
	b->used = 1;
	b->revision = revision;
	b->expires_ms = now_ms + QMSK_TTL_MS;
	snprintf(b->id, sizeof(b->id), "%016llx", (unsigned long long)t->minted);
	memcpy(id, b->id, sizeof(b->id));
	return 0;
}

struct qmsk_binding *qmsk_binding_attach(struct qmsk_bindings *t, const char *id, u64 revision, u64 now_ms,
	enum fe_refusal *why)
{
	struct qmsk_binding *b = qmsk_binding_find(t, id);

	if (why)
		*why = FE_REFUSE_NOCALL;
	if (b && !qmsk_binding_live(b, now_ms)) {
		qmsk_binding_drop(b);
		b = NULL;
	}
	if (!b)
		return NULL;
	if (b->revision != revision) {
		if (why)
			*why = FE_REFUSE_REVISION;
		return NULL;		/* a refused attach does not hold the binding open */
	}
	b->expires_ms = now_ms + QMSK_TTL_MS;
	return b;
}

int qmsk_binding_push(struct qmsk_binding *b, const struct qmsk_pcm *f)
{
	if (!b->used)
		return -EINVAL;
	if (b->count >= QMSK_QUEUE_FRAMES)
		return -ENOSPC;
	b->play[(b->head + b->count) % QMSK_QUEUE_FRAMES] = *f;
	b->count++;
	return 0;
}

int qmsk_binding_pop(struct qmsk_binding *b, struct qmsk_pcm *out)
{
	if (!b->used || !b->count)
		return -ENOENT;
	*out = b->play[b->head];
	b->head = (b->head + 1) % QMSK_QUEUE_FRAMES;
	b->count--;
	return 0;
}

void qmsk_server_init(struct qmsk_server *s)
{
	u32 k;

	memset(s, 0, sizeof(*s));
	s->fd = -1;
	qmsk_bindings_init(&s->bindings);
	for (k = 0; k < QMSK_PEERS; k++) {
		s->peer[k].fd = -1;
		s->peer[k].id[0] = 0;
	}
}

int qmsk_server_open(struct qmsk_server *s, const char *path)
{
	struct sockaddr_un addr;
	socklen_t len;
	int fd, flags, err;

	if (!path || !*path)
		return -EINVAL;
	if (strlen(path) >= sizeof(addr.sun_path))
		return -ENAMETOOLONG;
	if (s->fd >= 0)
		return -EALREADY;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, path, strlen(path));
	len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
	fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0 && (errno == EPROTONOSUPPORT || errno == ESOCKTNOSUPPORT))
		fd = socket(AF_UNIX, SOCK_STREAM, 0);	/* a kernel without unix seqpacket is served over a stream, frame for frame */
	if (fd < 0)
		return -errno;
	flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	unlink(path);	/* the socket file of a killed daemon is replaced; one daemon serves this path */
	if (bind(fd, (struct sockaddr *)&addr, len) || listen(fd, (int)QMSK_PEERS)) {
		err = -errno;
		close(fd);
		return err;
	}
	(void)chmod(path, 0600);	/* the audio is the call's: only the daemon's own user connects */
	s->fd = fd;
	snprintf(s->path, sizeof(s->path), "%s", path);
	return 0;
}

void qmsk_server_drop_peer(struct qmsk_server *s, unsigned int i)
{
	if (i >= QMSK_PEERS)
		return;
	if (s->peer[i].fd >= 0)
		close(s->peer[i].fd);
	s->peer[i].fd = -1;
	s->peer[i].id[0] = 0;
}

void qmsk_server_close(struct qmsk_server *s)
{
	unsigned int k;

	for (k = 0; k < QMSK_PEERS; k++)
		qmsk_server_drop_peer(s, k);
	if (s->fd >= 0) {
		close(s->fd);
		s->fd = -1;
	}
	if (s->path[0]) {
		unlink(s->path);
		s->path[0] = 0;
	}
	qmsk_bindings_init(&s->bindings);
}

int qmsk_server_accept(struct qmsk_server *s, unsigned int *slot)
{
	unsigned int i;
	int fd, flags;

	if (s->fd < 0)
		return -EBADF;
	for (i = 0; i < QMSK_PEERS && s->peer[i].fd >= 0; i++)
		;
	fd = accept(s->fd, NULL, NULL);
	if (fd < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		return -errno;
	}
	if (i == QMSK_PEERS) {
		close(fd);		/* the ninth socket is closed at once: eight peers exist */
		return -ENOSPC;
	}
	flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	s->peer[i].fd = fd;
	s->peer[i].id[0] = 0;
	if (slot)
		*slot = i;
	return 1;
}

/* the peer holding a binding's id, -1 when no peer does */
static int binding_peer(const struct qmsk_server *s, const struct qmsk_binding *b)
{
	unsigned int k;

	for (k = 0; k < QMSK_PEERS; k++)
		if (s->peer[k].fd >= 0 && !strcmp(s->peer[k].id, b->id))
			return (int)k;
	return -1;
}

/* one frame out; a seqpacket message is whole or not sent at all */
static int send_frame(struct qmsk_server *s, unsigned int i, const unsigned char *buf, u32 len)
{
	ssize_t n;

	if (i >= QMSK_PEERS || s->peer[i].fd < 0)
		return -EINVAL;
	n = send(s->peer[i].fd, buf, len, MSG_NOSIGNAL);
	if (n < 0)
		return errno == EAGAIN || errno == EWOULDBLOCK ? -EAGAIN : -errno;
	return (u32)n == len ? 0 : -EIO;
}

int qmsk_server_send_capture(struct qmsk_server *s, unsigned int i, const struct qmsk_pcm *f)
{
	unsigned char buf[QMSK_FRAME_MAX];
	u32 n = qmsk_build_pcm(buf, QMSK_PCM_CAPTURE, f);

	return n ? send_frame(s, i, buf, n) : -EINVAL;
}

int qmsk_server_send_state(struct qmsk_server *s, unsigned int i, enum fe_state state, u32 revision)
{
	unsigned char buf[QMSK_FRAME_MAX];
	u32 n = qmsk_build_state(buf, state, revision);

	return n ? send_frame(s, i, buf, n) : -EINVAL;
}

int qmsk_server_send_error(struct qmsk_server *s, unsigned int i, enum fe_refusal code)
{
	unsigned char buf[QMSK_FRAME_MAX];
	u32 n = qmsk_build_error(buf, code);

	return n ? send_frame(s, i, buf, n) : -EINVAL;
}

/* the reason goes out first and the socket closes after it: a closing unix socket still delivers what it holds */
static void refuse(struct qmsk_server *s, unsigned int i, enum fe_refusal code)
{
	(void)qmsk_server_send_error(s, i, code);
	qmsk_server_drop_peer(s, i);
}

struct qmsk_binding *qmsk_server_peer_binding(struct qmsk_server *s, unsigned int i)
{
	if (i >= QMSK_PEERS || s->peer[i].fd < 0 || !s->peer[i].id[0])
		return NULL;
	return qmsk_binding_find(&s->bindings, s->peer[i].id);
}

int qmsk_server_read(struct qmsk_server *s, unsigned int i, u64 now_ms)
{
	unsigned char buf[QMSK_FRAME_MAX];
	struct qmsk_msg m;
	struct qmsk_peer *p;
	struct qmsk_binding *b;
	ssize_t n;

	if (i >= QMSK_PEERS || s->peer[i].fd < 0)
		return -EINVAL;
	p = &s->peer[i];
	n = recv(p->fd, buf, sizeof(buf), MSG_TRUNC);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return 0;
		qmsk_server_drop_peer(s, i);
		return -ECONNRESET;
	}
	if (n == 0) {
		qmsk_server_drop_peer(s, i);	/* the binding stays until its ttl, so a reconnect can attach again */
		return -ECONNRESET;
	}
	if ((size_t)n > sizeof(buf) || qmsk_parse(buf, (size_t)n, &m)) {
		refuse(s, i, FE_INTERNAL);	/* the wire faults have no refusal word of their own */
		return -EPROTO;
	}
	b = p->id[0] ? qmsk_binding_get(&s->bindings, p->id, now_ms) : NULL;
	if (!b)
		p->id[0] = 0;			/* the binding is gone: the peer holds none */
	switch (m.type) {
	case QMSK_ATTACH: {
		enum fe_refusal why;
		int holder;

		b = qmsk_binding_attach(&s->bindings, m.binding_id, m.call_revision, now_ms, &why);
		if (!b) {
			refuse(s, i, why);
			return -EPROTO;
		}
		holder = binding_peer(s, b);
		if (holder >= 0 && holder != (int)i) {
			refuse(s, i, FE_REFUSE_BUSY);	/* one binding belongs to one socket */
			return -EPROTO;
		}
		memcpy(p->id, b->id, sizeof(p->id));
		return QMSK_ATTACH;
	}
	case QMSK_DETACH:
		if (b)
			qmsk_binding_drop(b);	/* the front-end is through with the call: the id dies with the frame */
		p->id[0] = 0;
		break;
	case QMSK_PCM_PLAYBACK:
		if (!b) {
			refuse(s, i, FE_REFUSE_NOCALL);	/* playback belongs to a call */
			return -EPROTO;
		}
		if (qmsk_binding_push(b, &m.pcm)) {
			(void)qmsk_server_send_error(s, i, FE_DROP_SLOW);
			return -ENOSPC;		/* the peer stays: it may stop overfilling and the queue drains */
		}
		break;
	}
	return m.type;
}
