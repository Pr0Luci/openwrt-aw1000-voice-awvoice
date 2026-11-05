/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
/* The qmsk frames byte by byte against the builders, every refusal, the attach gate, the ttl with the time handed in,
 * the heartbeat, the playback queue and the peer count, over an AF_UNIX socket. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "qmsk_server.h"

static struct qmsk_server srv;
static char sock_path[128];
static u64 clock_ms;		/* the time every call that ages a binding is handed */
static int sock_type;		/* the wire's socket type, or the stream a host without it is given */

/* no peer may be left from an earlier test when the table is cleared */
static void reset_bindings(u64 now)
{
	unsigned int k;

	for (k = 0; k < QMSK_PEERS; k++)
		assert(srv.peer[k].fd < 0);
	clock_ms = now;
	qmsk_bindings_init(&srv.bindings);
}

static void mint_here(u64 revision, u64 now, char id[QMSK_ID_MAX + 1])
{
	assert(qmsk_binding_mint(&srv.bindings, revision, now, id) == 0);
}

/* the lowest free slot, which is the one an accept takes */
static unsigned int free_slot(void)
{
	unsigned int k;

	for (k = 0; k < QMSK_PEERS && srv.peer[k].fd >= 0; k++)
		;
	return k;
}

/* a connected, non-blocking client on the server, and the peer slot it took */
static int connect_peer(unsigned int *idx)
{
	struct sockaddr_un addr;
	int fd, flags;

	fd = socket(AF_UNIX, sock_type, 0);
	assert(fd >= 0);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, sock_path);
	assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(qmsk_server_accept(&srv, idx) == 1);
	assert(*idx < QMSK_PEERS && srv.peer[*idx].fd >= 0);
	flags = fcntl(fd, F_GETFL);
	assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
	return fd;
}

static void send_attach(int fd, u64 revision, const char *id)
{
	unsigned char buf[QMSK_FRAME_MAX];
	u32 n = qmsk_build_attach(buf, revision, id);

	assert(n);
	assert(send(fd, buf, n, 0) == (ssize_t)n);
}

static void send_pcm(int fd, u16 type, const struct qmsk_pcm *f)
{
	unsigned char buf[QMSK_FRAME_MAX];
	u32 n = qmsk_build_pcm(buf, type, f);

	assert(n);
	assert(send(fd, buf, n, 0) == (ssize_t)n);
}

static void send_simple(int fd, u32 n, const unsigned char *buf)
{
	assert(send(fd, buf, n, 0) == (ssize_t)n);
}

/* the one frame waiting, decoded, or a failure assertion */
static u32 recv_frame(int fd, struct qmsk_msg *m)
{
	unsigned char buf[QMSK_FRAME_MAX];
	ssize_t n = recv(fd, buf, sizeof(buf), 0);

	assert(n > 0);
	assert(qmsk_parse(buf, (size_t)n, m) == 0);
	return (u32)n;
}

/* the peer sent nothing back */
static void expect_silence(int fd)
{
	unsigned char buf[QMSK_FRAME_MAX];

	assert(recv(fd, buf, sizeof(buf), 0) == -1);
	assert(errno == EAGAIN || errno == EWOULDBLOCK);
}

/* one error frame, and then the end: the peer was closed with the reason queued ahead of it */
static void expect_error(int fd, enum fe_refusal code)
{
	unsigned char buf[QMSK_FRAME_MAX];
	struct qmsk_msg m;

	recv_frame(fd, &m);
	assert(m.type == QMSK_ERROR && m.code == code);
	assert(recv(fd, buf, sizeof(buf), 0) == 0);
}

/* the same frame built and read back carries every field, and a length that is not its type's is never built */
static void test_framing(void)
{
	unsigned char buf[QMSK_FRAME_MAX];
	struct qmsk_msg m;
	struct qmsk_pcm f;
	char id[QMSK_ID_MAX + 2];
	u64 rev = 0x0123456789abcdefull;
	u32 k, n;

	memset(id, 'a', QMSK_ID_MAX);
	id[QMSK_ID_MAX] = 0;
	n = qmsk_build_attach(buf, rev, id);
	assert(n == QMSK_HEADER_BYTES + QMSK_ATTACH_PAYLOAD && n == 92);
	assert(qmsk_parse(buf, n, &m) == 0);
	assert(m.type == QMSK_ATTACH && m.payload_length == QMSK_ATTACH_PAYLOAD && m.call_revision == rev);
	assert(!strcmp(m.binding_id, id));
	buf[8] = 76;				/* neither the padded length nor another valid one */
	assert(qmsk_parse(buf, QMSK_HEADER_BYTES + 76, &m) == -EPROTO);
	buf[8] = QMSK_ATTACH_PAYLOAD;
	id[QMSK_ID_MAX] = 'a';		/* 65 characters do not fit the field */
	id[QMSK_ID_MAX + 1] = 0;
	assert(qmsk_build_attach(buf, rev, id) == 0);
	assert(qmsk_build_attach(buf, rev, "") == 0);
	n = qmsk_build_detach(buf);
	assert(n == 12 && qmsk_parse(buf, n, &m) == 0 && m.type == QMSK_DETACH && m.payload_length == 0);
	n = qmsk_build_heartbeat(buf);
	assert(n == 12 && qmsk_parse(buf, n, &m) == 0 && m.type == QMSK_HEARTBEAT && m.payload_length == 0);
	memset(&f, 0, sizeof(f));
	f.sequence = 7;
	f.timestamp_ms = 7 * QMSK_PCM_MS;
	f.sample[0] = -32768;
	f.sample[QMSK_PCM_SAMPLES - 1] = 32767;
	for (k = 1; k + 1 < QMSK_PCM_SAMPLES; k++)
		f.sample[k] = (int16_t)(1000 - (int)k * 13);
	n = qmsk_build_pcm(buf, QMSK_PCM_PLAYBACK, &f);
	assert(n == QMSK_HEADER_BYTES + QMSK_PCM_PAYLOAD && n == 340);
	assert(qmsk_parse(buf, n, &m) == 0 && m.type == QMSK_PCM_PLAYBACK);
	assert(m.pcm.sequence == 7 && m.pcm.timestamp_ms == 140);
	for (k = 0; k < QMSK_PCM_SAMPLES; k++)
		assert(m.pcm.sample[k] == f.sample[k]);
	n = qmsk_build_pcm(buf, QMSK_PCM_CAPTURE, &f);
	assert(n == 340 && qmsk_parse(buf, n, &m) == 0 && m.type == QMSK_PCM_CAPTURE);
	n = qmsk_build_state(buf, FE_STATE_INCOMING_RINGING, 9);
	assert(n == QMSK_HEADER_BYTES + QMSK_STATE_PAYLOAD && n == 20);
	assert(qmsk_parse(buf, n, &m) == 0 && m.type == QMSK_STATE_CHANGE);
	assert(m.state == FE_STATE_INCOMING_RINGING && m.revision == 9);
	n = qmsk_build_error(buf, FE_REFUSE_REVISION);
	assert(n == QMSK_HEADER_BYTES + QMSK_ERROR_PAYLOAD && n == 16);
	assert(qmsk_parse(buf, n, &m) == 0 && m.type == QMSK_ERROR && m.code == FE_REFUSE_REVISION);
	assert(qmsk_build(buf, 0x20, NULL, 0) == 0);
	assert(qmsk_build(buf, QMSK_DETACH, NULL, 4) == 0);
	assert(qmsk_build_pcm(buf, QMSK_ATTACH, &f) == 0);
	assert(qmsk_build_pcm(buf, QMSK_PCM_PLAYBACK, NULL) == 0);
}

/* the header and every payload field in wire order, byte by byte */
static void test_wire_bytes(void)
{
	unsigned char buf[QMSK_FRAME_MAX];
	struct qmsk_pcm f;
	u32 k;

	qmsk_build_attach(buf, 0x1122334455667788ull, "s1");
	assert(buf[0] == 0x4b && buf[1] == 0x53 && buf[2] == 0x4d && buf[3] == 0x51);	/* 0x514d534b little-endian */
	assert(buf[4] == 1 && buf[5] == 0);		/* version */
	assert(buf[6] == QMSK_ATTACH && buf[7] == 0);	/* type */
	assert(buf[8] == 80 && buf[9] == 0 && buf[10] == 0 && buf[11] == 0);	/* payload length: the fields padded to eight */
	assert(buf[12] == 0x88 && buf[13] == 0x77 && buf[14] == 0x66 && buf[15] == 0x55);
	assert(buf[16] == 0x44 && buf[17] == 0x33 && buf[18] == 0x22 && buf[19] == 0x11);
	assert(buf[20] == 's' && buf[21] == '1');
	for (k = 22; k < QMSK_HEADER_BYTES + QMSK_ATTACH_PAYLOAD; k++)
		assert(buf[k] == 0);			/* the rest of the id field is padding */
	memset(&f, 0, sizeof(f));
	f.sequence = 0x10203040u;
	f.timestamp_ms = 5;
	f.sample[0] = 0x1234;
	f.sample[1] = -2;
	assert(qmsk_build_pcm(buf, QMSK_PCM_PLAYBACK, &f) == 340);
	assert(buf[6] == QMSK_PCM_PLAYBACK && buf[8] == 0x48 && buf[9] == 1);	/* 328 */
	assert(buf[12] == 0x40 && buf[13] == 0x30 && buf[14] == 0x20 && buf[15] == 0x10);
	assert(buf[16] == 5 && buf[17] == 0 && buf[18] == 0 && buf[19] == 0);
	assert(buf[20] == 0x34 && buf[21] == 0x12);
	assert(buf[22] == 0xfe && buf[23] == 0xff);
	assert(qmsk_build_state(buf, FE_STATE_ACTIVE, 0x0a) == 20);
	assert(buf[6] == QMSK_STATE_CHANGE && buf[8] == 8 && buf[9] == 0);
	assert(buf[12] == 5 && buf[13] == 0 && buf[14] == 0 && buf[15] == 0);
	assert(buf[16] == 0x0a && buf[17] == 0 && buf[18] == 0 && buf[19] == 0);
	assert(qmsk_build_error(buf, FE_REFUSE_BUSY) == 16);
	assert(buf[8] == 4 && buf[9] == 0 && buf[12] == 1 && buf[13] == 0 && buf[14] == 0 && buf[15] == 0);
}

/* every frame the wire must refuse is refused by the parser alone, before any socket is involved */
static void test_parse_refusals(void)
{
	unsigned char buf[QMSK_FRAME_MAX];
	struct qmsk_msg m;
	u32 n, k;

	n = qmsk_build_heartbeat(buf);
	buf[0] ^= 0xff;
	assert(qmsk_parse(buf, n, &m) == -EPROTO);
	buf[0] ^= 0xff;
	buf[4] = 2;
	assert(qmsk_parse(buf, n, &m) == -EPROTO);
	buf[4] = 1;
	buf[8] = 4;
	assert(qmsk_parse(buf, n, &m) == -EPROTO);	/* a heartbeat carries nothing */
	buf[8] = 0;
	assert(qmsk_parse(buf, n - 1, &m) == -EPROTO);	/* cut inside the frame */
	assert(qmsk_parse(buf, 0, &m) == -EPROTO);
	buf[6] = 0x42;
	assert(qmsk_parse(buf, n, &m) == -EPROTO);	/* a type this wire does not define */
	buf[6] = QMSK_ATTACH;
	buf[8] = 73;
	assert(qmsk_parse(buf, n, &m) == -EPROTO);	/* the length says attach, the frame is shorter */
	n = qmsk_build_attach(buf, 3, "abc");
	for (k = 0; k < QMSK_ID_MAX + 1; k++)
		buf[QMSK_HEADER_BYTES + 8 + k] = 'a';	/* no end inside the id field */
	assert(qmsk_parse(buf, n, &m) == -EPROTO);
}

/* a bad magic, another version, a length that is not the type's, a truncated frame, an overlong one and an unknown
 * type are each refused with an error and the peer is closed */
static void test_refusals(void)
{
	unsigned char buf[512];
	unsigned int i;
	int fd;
	u32 n, k;

	reset_bindings(5000);
	fd = connect_peer(&i);
	n = qmsk_build_heartbeat(buf);
	buf[0] ^= 0xff;
	send_simple(fd, n, buf);
	assert(qmsk_server_read(&srv, i, clock_ms) == -EPROTO);
	assert(srv.peer[i].fd < 0);
	expect_error(fd, FE_INTERNAL);
	assert(qmsk_server_read(&srv, i, clock_ms) == -EINVAL);
	close(fd);
	fd = connect_peer(&i);
	n = qmsk_build_heartbeat(buf);
	buf[4] = 2;				/* another version */
	send_simple(fd, n, buf);
	assert(qmsk_server_read(&srv, i, clock_ms) == -EPROTO);
	expect_error(fd, FE_INTERNAL);
	close(fd);
	fd = connect_peer(&i);
	n = qmsk_build_heartbeat(buf);
	buf[8] = 4;				/* a heartbeat carries nothing */
	send_simple(fd, n, buf);
	assert(qmsk_server_read(&srv, i, clock_ms) == -EPROTO);
	expect_error(fd, FE_INTERNAL);
	close(fd);
	fd = connect_peer(&i);
	n = qmsk_build_state(buf, FE_STATE_ACTIVE, 1);
	buf[8] = 4;				/* a state change carries two numbers */
	send_simple(fd, n, buf);
	assert(qmsk_server_read(&srv, i, clock_ms) == -EPROTO);
	expect_error(fd, FE_INTERNAL);
	close(fd);
	fd = connect_peer(&i);
	n = qmsk_build_heartbeat(buf);
	send_simple(fd, 5, buf);		/* cut before the header ends */
	assert(qmsk_server_read(&srv, i, clock_ms) == -EPROTO);
	expect_error(fd, FE_INTERNAL);
	close(fd);
	fd = connect_peer(&i);
	n = qmsk_build_heartbeat(buf);
	memset(buf + n, 0, sizeof(buf) - n);
	send_simple(fd, 400, buf);		/* longer than the longest frame of any type */
	assert(qmsk_server_read(&srv, i, clock_ms) == -EPROTO);
	expect_error(fd, FE_INTERNAL);
	close(fd);
	fd = connect_peer(&i);
	n = qmsk_build_heartbeat(buf);
	buf[6] = 0x42;				/* a type this wire does not define */
	send_simple(fd, n, buf);
	assert(qmsk_server_read(&srv, i, clock_ms) == -EPROTO);
	expect_error(fd, FE_INTERNAL);
	close(fd);
	fd = connect_peer(&i);
	n = qmsk_build_attach(buf, 3, "abc");
	for (k = 0; k < QMSK_ID_MAX + 1; k++)	/* every byte of the id field is a character: no end, no id */
		buf[QMSK_HEADER_BYTES + 8 + k] = 'a';
	send_simple(fd, n, buf);
	assert(qmsk_server_read(&srv, i, clock_ms) == -EPROTO);
	expect_error(fd, FE_INTERNAL);
	close(fd);
}

/* an attach is taken for its own revision only, one socket holds one binding, and the binding outlives a close */
static void test_attach(void)
{
	char id[QMSK_ID_MAX + 1], other[QMSK_ID_MAX + 1];
	unsigned int i, j;
	int fd;

	reset_bindings(1000);
	mint_here(7, clock_ms, id);
	mint_here(7, clock_ms, other);
	fd = connect_peer(&i);
	send_attach(fd, 7, id);
	assert(qmsk_server_read(&srv, i, clock_ms) == QMSK_ATTACH);
	assert(qmsk_server_peer_binding(&srv, i) == qmsk_binding_find(&srv.bindings, id));
	assert(qmsk_server_peer_binding(&srv, i)->revision == 7);
	{
		int bad = connect_peer(&j);

		send_attach(bad, 8, other);	/* the id is minted for another revision */
		assert(qmsk_server_read(&srv, j, clock_ms) == -EPROTO);
		expect_error(bad, FE_REFUSE_REVISION);
		close(bad);
	}
	{
		int unknown = connect_peer(&j);

		send_attach(unknown, 7, "0000000000000000");
		assert(qmsk_server_read(&srv, j, clock_ms) == -EPROTO);
		expect_error(unknown, FE_REFUSE_NOCALL);
		close(unknown);
	}
	{
		int second = connect_peer(&j);

		send_attach(second, 7, id);
		assert(qmsk_server_read(&srv, j, clock_ms) == -EPROTO);
		expect_error(second, FE_REFUSE_BUSY);
		close(second);
	}
	close(fd);				/* the binding waits for a reconnect, it does not die with the socket */
	assert(qmsk_server_read(&srv, i, clock_ms) == -ECONNRESET);
	assert(qmsk_binding_find(&srv.bindings, id) != NULL);
	fd = connect_peer(&i);
	send_attach(fd, 7, id);
	assert(qmsk_server_read(&srv, i, clock_ms) == QMSK_ATTACH);
	close(fd);
	assert(qmsk_server_read(&srv, i, clock_ms) == -ECONNRESET);
}

/* a detach ends the binding there and then: the id is dead and playback has no call */
static void test_detach(void)
{
	unsigned char buf[QMSK_FRAME_MAX];
	struct qmsk_pcm f;
	char id[QMSK_ID_MAX + 1];
	unsigned int i;
	u32 n;
	int fd;

	reset_bindings(2000);
	mint_here(5, clock_ms, id);
	fd = connect_peer(&i);
	send_attach(fd, 5, id);
	assert(qmsk_server_read(&srv, i, clock_ms) == QMSK_ATTACH);
	n = qmsk_build_detach(buf);
	send_simple(fd, n, buf);
	assert(qmsk_server_read(&srv, i, clock_ms) == QMSK_DETACH);
	assert(qmsk_server_peer_binding(&srv, i) == NULL);
	assert(qmsk_binding_find(&srv.bindings, id) == NULL);
	memset(&f, 0, sizeof(f));
	send_pcm(fd, QMSK_PCM_PLAYBACK, &f);
	assert(qmsk_server_read(&srv, i, clock_ms) == -EPROTO);
	expect_error(fd, FE_REFUSE_NOCALL);
	close(fd);
}

/* a heartbeat sends nothing back and a live binding is carried by it; a dead one is not brought back */
static void test_heartbeat(void)
{
	unsigned char buf[QMSK_FRAME_MAX];
	struct qmsk_pcm f;
	char id[QMSK_ID_MAX + 1];
	unsigned int i, j;
	u32 n;
	int fd, other;

	reset_bindings(0);
	mint_here(3, clock_ms, id);
	fd = connect_peer(&i);
	send_attach(fd, 3, id);
	assert(qmsk_server_read(&srv, i, clock_ms) == QMSK_ATTACH);
	other = connect_peer(&j);
	n = qmsk_build_heartbeat(buf);
	send_simple(other, n, buf);		/* a peer with no binding is answered with nothing too */
	assert(qmsk_server_read(&srv, j, clock_ms) == QMSK_HEARTBEAT);
	expect_silence(other);
	assert(srv.peer[j].fd >= 0);
	close(other);
	assert(qmsk_server_read(&srv, j, clock_ms) == -ECONNRESET);
	clock_ms = 29000;
	send_simple(fd, n, buf);
	assert(qmsk_server_read(&srv, i, clock_ms) == QMSK_HEARTBEAT);
	expect_silence(fd);
	assert(qmsk_binding_find(&srv.bindings, id)->expires_ms == 29000 + QMSK_TTL_MS);
	clock_ms = 58000;			/* past 30000: only the heartbeat keeps it alive */
	memset(&f, 0, sizeof(f));
	f.sequence = 1;
	send_pcm(fd, QMSK_PCM_PLAYBACK, &f);
	assert(qmsk_server_read(&srv, i, clock_ms) == QMSK_PCM_PLAYBACK);
	clock_ms = 58000 + QMSK_TTL_MS;		/* and nothing keeps it past its ttl */
	send_simple(fd, n, buf);
	assert(qmsk_server_read(&srv, i, clock_ms) == QMSK_HEARTBEAT);
	assert(qmsk_server_peer_binding(&srv, i) == NULL);
	expect_silence(fd);
	close(fd);
	assert(qmsk_server_read(&srv, i, clock_ms) == -ECONNRESET);
}

/* 160 samples a frame, 25 frames waiting, the twenty-sixth refused with dropped-slow while the peer stays */
static void test_pcm_queue(void)
{
	struct qmsk_pcm f, out;
	char id[QMSK_ID_MAX + 1];
	unsigned int i, k;
	int fd;

	reset_bindings(3000);
	mint_here(2, clock_ms, id);
	fd = connect_peer(&i);
	send_attach(fd, 2, id);
	assert(qmsk_server_read(&srv, i, clock_ms) == QMSK_ATTACH);
	for (k = 0; k < QMSK_QUEUE_FRAMES; k++) {
		memset(&f, 0, sizeof(f));
		f.sequence = k;
		f.timestamp_ms = k * QMSK_PCM_MS;
		f.sample[0] = (int16_t)k;
		f.sample[QMSK_PCM_SAMPLES - 1] = (int16_t)-k;
		send_pcm(fd, QMSK_PCM_PLAYBACK, &f);
		assert(qmsk_server_read(&srv, i, clock_ms) == QMSK_PCM_PLAYBACK);
	}
	assert(qmsk_server_peer_binding(&srv, i)->count == QMSK_QUEUE_FRAMES);
	memset(&f, 0, sizeof(f));
	f.sequence = QMSK_QUEUE_FRAMES;
	f.timestamp_ms = QMSK_QUEUE_FRAMES * QMSK_PCM_MS;
	f.sample[0] = (int16_t)QMSK_QUEUE_FRAMES;
	send_pcm(fd, QMSK_PCM_PLAYBACK, &f);
	assert(qmsk_server_read(&srv, i, clock_ms) == -ENOSPC);
	{
		struct qmsk_msg m;

		recv_frame(fd, &m);
		assert(m.type == QMSK_ERROR && m.code == FE_DROP_SLOW);	/* the queue kept every frame it held */
	}
	assert(qmsk_server_peer_binding(&srv, i)->count == QMSK_QUEUE_FRAMES);
	assert(qmsk_binding_pop(qmsk_server_peer_binding(&srv, i), &out) == 0);
	assert(out.sequence == 0 && out.sample[0] == 0 && out.sample[QMSK_PCM_SAMPLES - 1] == 0);
	send_pcm(fd, QMSK_PCM_PLAYBACK, &f);	/* one out, one in */
	assert(qmsk_server_read(&srv, i, clock_ms) == QMSK_PCM_PLAYBACK);
	for (k = 0; k < QMSK_QUEUE_FRAMES; k++) {
		assert(qmsk_binding_pop(qmsk_server_peer_binding(&srv, i), &out) == 0);
		assert(out.sequence == (k + 1 < QMSK_QUEUE_FRAMES ? k + 1 : QMSK_QUEUE_FRAMES));
		assert(out.timestamp_ms == out.sequence * QMSK_PCM_MS);
		assert(out.sample[0] == (int16_t)out.sequence);
	}
	assert(qmsk_binding_pop(qmsk_server_peer_binding(&srv, i), &out) == -ENOENT);
	close(fd);
	assert(qmsk_server_read(&srv, i, clock_ms) == -ECONNRESET);
}

/* a state change and an error go out with their own payloads, and a capture frame is a playback frame's mirror */
static void test_state_error(void)
{
	unsigned char buf[QMSK_FRAME_MAX];
	struct qmsk_msg m;
	struct qmsk_pcm f, back;
	unsigned int i, k;
	int fd;

	reset_bindings(4000);
	fd = connect_peer(&i);
	assert(qmsk_server_send_state(&srv, i, FE_STATE_INCOMING_RINGING, 9) == 0);
	recv_frame(fd, &m);
	assert(m.type == QMSK_STATE_CHANGE && m.state == FE_STATE_INCOMING_RINGING && m.revision == 9);
	assert(qmsk_server_send_error(&srv, i, FE_REFUSE_BUSY) == 0);
	recv_frame(fd, &m);
	assert(m.type == QMSK_ERROR && m.code == FE_REFUSE_BUSY);
	memset(&f, 0, sizeof(f));
	f.sequence = 12;
	f.timestamp_ms = 240;
	for (k = 0; k < QMSK_PCM_SAMPLES; k++)
		f.sample[k] = (int16_t)(k - 80);
	assert(qmsk_server_send_capture(&srv, i, &f) == 0);
	assert(recv_frame(fd, &m) == 340);
	assert(m.type == QMSK_PCM_CAPTURE && m.pcm.sequence == 12 && m.pcm.timestamp_ms == 240);
	memcpy(&back, &m.pcm, sizeof(back));
	assert(!memcmp(&back, &f, sizeof(f)));
	assert(qmsk_build_state(buf, FE_STATE_FAULT, 1) == 20);
	close(fd);
	assert(qmsk_server_read(&srv, i, clock_ms) == -ECONNRESET);
}

/* eight peers, the ninth socket closed at once, and a peer slot freed by a drop */
static void test_peers(void)
{
	int fd[QMSK_PEERS];
	unsigned int i, k;

	reset_bindings(6000);
	for (k = 0; k < QMSK_PEERS; k++)
		fd[k] = connect_peer(&i);
	assert(free_slot() == QMSK_PEERS);
	{
		struct sockaddr_un addr;
		char c;
		int over = socket(AF_UNIX, sock_type, 0);

		assert(over >= 0);
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strcpy(addr.sun_path, sock_path);
		assert(connect(over, (struct sockaddr *)&addr, sizeof(addr)) == 0);
		assert(qmsk_server_accept(&srv, NULL) == -ENOSPC);
		assert(recv(over, &c, 1, 0) == 0);	/* the ninth is closed, not served */
		close(over);
	}
	for (k = 0; k < QMSK_PEERS; k++) {
		qmsk_server_drop_peer(&srv, k);
		assert(srv.peer[k].fd < 0);
		close(fd[k]);
	}
	assert(free_slot() == 0);
}

/* the socket path: bind, the second open on it, an old file replaced, and a path the kernel cannot take */
static void test_open(const char *dir)
{
	struct qmsk_server one;
	char path[300], deep[200];
	FILE *f;

	qmsk_server_init(&one);
	snprintf(path, sizeof(path), "%s/one.sock", dir);
	assert(qmsk_server_open(&one, path) == 0);
	assert(one.fd >= 0 && !strcmp(one.path, path));
	assert(qmsk_server_open(&one, path) == -EALREADY);
	qmsk_server_close(&one);
	assert(one.fd < 0 && !one.path[0]);
	assert(access(path, F_OK) == -1);
	f = fopen(path, "w");		/* a file where the socket was left is replaced */
	assert(f != NULL);
	fclose(f);
	assert(qmsk_server_open(&one, path) == 0);
	qmsk_server_close(&one);
	assert(access(path, F_OK) == -1);
	memset(deep, 'a', sizeof(deep) - 1);
	deep[0] = '/';
	deep[sizeof(deep) - 1] = 0;
	assert(qmsk_server_open(&one, deep) == -ENAMETOOLONG);
	assert(qmsk_server_open(&one, "") == -EINVAL);
}

/* the table on its own: the ttl to the millisecond, the eight slots, the revision gate and the ids */
static void test_bindings(void)
{
	struct qmsk_bindings t;
	enum fe_refusal why;
	struct qmsk_binding *b;
	char id[QMSK_ID_MAX + 1], again[QMSK_ID_MAX + 1];
	u32 k;

	qmsk_bindings_init(&t);
	assert(qmsk_binding_mint(&t, 4, 0, id) == 0);
	assert(strlen(id) == 16);
	assert(qmsk_binding_live(qmsk_binding_find(&t, id), QMSK_TTL_MS - 1));
	assert(!qmsk_binding_live(qmsk_binding_find(&t, id), QMSK_TTL_MS));
	b = qmsk_binding_attach(&t, id, 4, 1000, &why);
	assert(b && b->expires_ms == 1000 + QMSK_TTL_MS);
	assert(qmsk_binding_live(b, 30999));
	b = qmsk_binding_attach(&t, id, 4, 31000, &why);
	assert(!b && why == FE_REFUSE_NOCALL);
	assert(qmsk_binding_find(&t, id) == NULL);
	qmsk_bindings_init(&t);
	assert(qmsk_binding_mint(&t, 4, 0, id) == 0);
	assert(qmsk_binding_attach(&t, id, 5, 1000, &why) == NULL);
	assert(why == FE_REFUSE_REVISION);
	assert(qmsk_binding_attach(&t, id, 4, 29999, &why) != NULL);	/* a refused attach did not hold it open */
	assert(qmsk_binding_mint(&t, 0, 0, id) == -EINVAL);
	qmsk_bindings_init(&t);
	for (k = 0; k < QMSK_SLOTS; k++)
		assert(qmsk_binding_mint(&t, 1, 0, id) == 0);
	assert(qmsk_binding_mint(&t, 1, 0, id) == -EBUSY);
	assert(qmsk_bindings_expire(&t, QMSK_TTL_MS) == QMSK_SLOTS);
	assert(qmsk_binding_mint(&t, 1, QMSK_TTL_MS, id) == 0);
	assert(qmsk_binding_mint(&t, 1, QMSK_TTL_MS, again) == 0);
	assert(strcmp(id, again) != 0);
	qmsk_bindings_init(&t);
	assert(qmsk_binding_mint(&t, 9, 0, id) == 0);
	b = qmsk_binding_get(&t, id, 20000);		/* a frame from the peer, a heartbeat among them */
	assert(b && b->expires_ms == 20000 + QMSK_TTL_MS);
	assert(qmsk_binding_live(b, 49999));
	assert(qmsk_binding_get(&t, id, 50000) == NULL);	/* nothing arrived in time */
}

/* the playback queue: 25 frames, the twenty-sixth refused, and the head wrapping */
static void test_queue(void)
{
	struct qmsk_bindings t;
	struct qmsk_binding *b;
	struct qmsk_pcm f, out;
	char id[QMSK_ID_MAX + 1];
	u32 k;

	qmsk_bindings_init(&t);
	assert(qmsk_binding_mint(&t, 1, 0, id) == 0);
	b = qmsk_binding_find(&t, id);
	for (k = 0; k < QMSK_QUEUE_FRAMES; k++) {
		memset(&f, 0, sizeof(f));
		f.sequence = k;
		f.timestamp_ms = k * QMSK_PCM_MS;
		f.sample[0] = (int16_t)k;
		f.sample[QMSK_PCM_SAMPLES - 1] = (int16_t)-k;
		assert(qmsk_binding_push(b, &f) == 0);
	}
	assert(b->count == QMSK_QUEUE_FRAMES);
	memset(&f, 0, sizeof(f));
	f.sequence = QMSK_QUEUE_FRAMES;
	assert(qmsk_binding_push(b, &f) == -ENOSPC);
	assert(b->count == QMSK_QUEUE_FRAMES);		/* the refusal kept what waited */
	for (k = 0; k < QMSK_QUEUE_FRAMES; k++) {
		assert(qmsk_binding_pop(b, &out) == 0);
		assert(out.sequence == k && out.timestamp_ms == k * QMSK_PCM_MS);
		assert(out.sample[0] == (int16_t)k && out.sample[QMSK_PCM_SAMPLES - 1] == (int16_t)-k);
	}
	assert(qmsk_binding_pop(b, &out) == -ENOENT && b->count == 0);
	assert(qmsk_binding_push(b, &f) == 0);		/* the head and the tail wrap past the end */
	assert(qmsk_binding_pop(b, &out) == 0 && out.sequence == QMSK_QUEUE_FRAMES);
	qmsk_binding_drop(b);
	assert(qmsk_binding_push(b, &f) == -EINVAL);
	assert(qmsk_binding_pop(b, &out) == -ENOENT);
}

/* af_unix seqpacket is what the wire is. A host whose kernel has no af_unix seqpacket runs the same server over a
 * stream: every frame here is one send, answered before the next, so a receive is one frame either way. */
static int pair_works(int type)
{
	char c = 'x';
	int sv[2], ok;

	if (socketpair(AF_UNIX, type, 0, sv))
		return 0;
	ok = send(sv[0], &c, 1, 0) == 1 && recv(sv[1], &c, 1, 0) == 1;
	close(sv[0]);
	close(sv[1]);
	return ok;
}

int main(void)
{
	char dir[128];

	test_framing();
	test_wire_bytes();
	test_parse_refusals();
	test_bindings();
	test_queue();
	if (pair_works(SOCK_SEQPACKET)) {
		sock_type = SOCK_SEQPACKET;
	} else {
		assert(pair_works(SOCK_STREAM));
		sock_type = SOCK_STREAM;
		fprintf(stderr, "qmsk: this host has no af_unix seqpacket: the socket half runs over a stream\n");
	}
	snprintf(dir, sizeof(dir), "/tmp/awvoice-qmsk-test.%ld", (long)getpid());
	assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
	snprintf(sock_path, sizeof(sock_path), "%s/qmsk.sock", dir);
	qmsk_server_init(&srv);
	test_open(dir);
	assert(qmsk_server_open(&srv, sock_path) == 0);
	test_refusals();
	test_attach();
	test_detach();
	test_heartbeat();
	test_pcm_queue();
	test_state_error();
	test_peers();
	qmsk_server_close(&srv);
	assert(access(sock_path, F_OK) == -1);
	rmdir(dir);
	printf("qmsk: all tests passed\n");
	return 0;
}
