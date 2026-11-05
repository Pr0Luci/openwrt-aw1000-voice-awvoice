/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_QMSK_SERVER_H
#define AW_PHONED_QMSK_SERVER_H

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "front_end.h"

/*
 * QMSK: the media socket between the daemon and a SIP front-end. One message per frame on an AF_UNIX SOCK_SEQPACKET,
 * non-blocking throughout, no threads and no clock of its own (every call that ages a binding is handed the time). A
 * kernel that refuses seqpacket for unix sockets is given a stream at the same path, where a frame is one send. The
 * daemon serves, the front-end connects, and the interface carries 8000 Hz only, so a media frame is always 160
 * samples over 20 ms and any other sample count is a wire fault.
 *
 * The frame on the wire: twelve header bytes, little-endian - u32 magic, u16 version, u16 type, u32 payload length -
 * then the payload of its type. The magic is the number 0x514d534b, so its little-endian bytes are 4b 53 4d 51; the
 * name qmsk is those four letters read the other way. A bad magic, another version or a payload length that is not
 * its type's is refused with an ERROR frame and the peer is closed. A heartbeat has no payload, sends nothing back
 * and refreshes the binding.
 *
 * A binding is one call as one front-end sees it: the daemon mints an id for a call revision and gives the id to the
 * front-end through the call interface, so an attach only proves that the id is live and that the revision is the
 * one it was minted for. An attach carries the id padded out to the payload length a C struct of these fields has:
 * eight bytes of revision, the 65 bytes of id, then the three bytes the alignment adds. Eight bindings and eight peer
 * sockets exist, a binding lives 30 s from its last frame, and one id belongs to one socket. Each peer's playback
 * queue holds 25 frames: a full queue refuses the next frame with dropped-slow and keeps the frames already waiting.
 */

#define QMSK_MAGIC		0x514d534bu	/* the wire's four bytes 4b 53 4d 51 read as one little-endian number */
#define QMSK_VERSION		1u
#define QMSK_HEADER_BYTES	12u

/* the frame types; a front-end sends attach, detach, heartbeat and playback, the daemon sends capture, state and error */
#define QMSK_ATTACH		0x01u
#define QMSK_DETACH		0x02u
#define QMSK_HEARTBEAT		0x03u
#define QMSK_PCM_CAPTURE	0x10u	/* the far end toward the front-end */
#define QMSK_PCM_PLAYBACK	0x11u	/* the front-end's audio toward the line */
#define QMSK_STATE_CHANGE	0x12u
#define QMSK_ERROR		0xffu

#define QMSK_ID_MAX		64u
#define QMSK_PCM_SAMPLES	160u	/* 20 ms at 8000 Hz */
#define QMSK_PCM_MS		20u
#define QMSK_ATTACH_FIELDS	(8u + QMSK_ID_MAX + 1u)		/* u64 revision, char id[65] */
#define QMSK_ATTACH_PAYLOAD	((QMSK_ATTACH_FIELDS + 7u) & ~7u)	/* the fields, padded to the u64's alignment: a C sender's struct is 80 */
#define QMSK_PCM_PAYLOAD	(8u + 2u * QMSK_PCM_SAMPLES)	/* u32 sequence, u32 timestamp, samples */
#define QMSK_STATE_PAYLOAD	8u					/* u32 state, u32 revision */
#define QMSK_ERROR_PAYLOAD	4u					/* u32 code */
#define QMSK_QUEUE_FRAMES	25u					/* one second of playback waiting per peer */
#define QMSK_SLOTS		8u
#define QMSK_PEERS		8u
#define QMSK_TTL_MS		30000u
#define QMSK_FRAME_MAX		(QMSK_HEADER_BYTES + QMSK_PCM_PAYLOAD)	/* the longest frame of any type */

/* one media frame: `sequence` counts frames from the call's start and the timestamp is sequence * QMSK_PCM_MS */
struct qmsk_pcm {
	u32 sequence;
	u32 timestamp_ms;
	int16_t sample[QMSK_PCM_SAMPLES];
};

/* one decoded frame; only the fields of its own type carry anything, and payload_length is that type's own length */
struct qmsk_msg {
	u16 type;
	u32 payload_length;
	u64 call_revision;			/* attach */
	char binding_id[QMSK_ID_MAX + 1];	/* attach */
	struct qmsk_pcm pcm;			/* both pcm types */
	enum fe_state state;			/* state change */
	u32 revision;				/* state change */
	enum fe_refusal code;			/* error */
};

/* one call attached by one front-end */
struct qmsk_binding {
	int used;				/* this slot holds a binding */
	u64 revision;				/* the call revision the id was minted for */
	u64 expires_ms;				/* the binding dies at this time; every frame from its peer moves it on */
	char id[QMSK_ID_MAX + 1];
	u32 head;				/* the oldest frame waiting */
	u32 count;				/* frames waiting, at most QMSK_QUEUE_FRAMES */
	struct qmsk_pcm play[QMSK_QUEUE_FRAMES];
};

/* the binding table; `minted` is the id counter */
struct qmsk_bindings {
	struct qmsk_binding slot[QMSK_SLOTS];
	u64 minted;
};

struct qmsk_peer {
	int fd;					/* the accepted socket, -1 when the slot is free */
	char id[QMSK_ID_MAX + 1];		/* the binding it attached, "" until an attach */
};

/* The queues make this object about 70 kb: the caller decides where it lives, and a stack is the wrong place. */
struct qmsk_server {
	int fd;					/* the listening socket, -1 when closed */
	char path[PATH_MAX];			/* where it is bound, "" when closed */
	struct qmsk_bindings bindings;
	struct qmsk_peer peer[QMSK_PEERS];
};

/* Framing. `out` takes QMSK_FRAME_MAX bytes; each builder returns the bytes written, or 0 when its arguments are not
 * something this wire carries. The builders write every field byte by byte, so the order is the wire's on any host. */

/* a frame of `type` around a payload of exactly that type's length */
u32 qmsk_build(void *out, u16 type, const void *payload, u32 payload_length);

/* attach: the call revision and the binding id, which is 1 to QMSK_ID_MAX characters and NUL padded */
u32 qmsk_build_attach(void *out, u64 revision, const char *binding_id);

u32 qmsk_build_detach(void *out);
u32 qmsk_build_heartbeat(void *out);

/* a media frame of either pcm type; the type must be QMSK_PCM_CAPTURE or QMSK_PCM_PLAYBACK */
u32 qmsk_build_pcm(void *out, u16 type, const struct qmsk_pcm *f);

u32 qmsk_build_state(void *out, enum fe_state state, u32 revision);
u32 qmsk_build_error(void *out, enum fe_refusal code);

/* Decode one frame: 0 and *m filled, or -EPROTO for a frame that must be refused - shorter than a header, another
 * magic or version, a type this wire does not define, or a length that is not that type's. An attach id must end
 * inside its 65 bytes; what follows the terminator is not read. */
int qmsk_parse(const void *buf, size_t len, struct qmsk_msg *m);

/* The binding table, with the time handed in. */

void qmsk_bindings_init(struct qmsk_bindings *t);

/* 1 while the binding holds a slot and has not reached its ttl at now_ms */
int qmsk_binding_live(const struct qmsk_binding *b, u64 now_ms);

/* the binding an id names, its ttl unchecked; NULL when no slot holds that id */
struct qmsk_binding *qmsk_binding_find(struct qmsk_bindings *t, const char *id);

/* the binding an id names, its ttl moved to now_ms + QMSK_TTL_MS; NULL when the id is unknown or its ttl has run out,
 * and an expired binding is dropped here */
struct qmsk_binding *qmsk_binding_get(struct qmsk_bindings *t, const char *id, u64 now_ms);

/* drop every binding whose ttl has run out; how many were dropped */
u32 qmsk_bindings_expire(struct qmsk_bindings *t, u64 now_ms);

/* free the slot and clear the queue */
void qmsk_binding_drop(struct qmsk_binding *b);

/* Mint an id for a call revision and start its ttl: 0 and the id written, -EINVAL for revision 0, -EBUSY when all
 * eight slots are live. The id is sixteen lowercase hex digits of a counter that starts at the clock on its first
 * mint and only climbs, so a restart of the daemon does not hand out an id it handed out before. */
int qmsk_binding_mint(struct qmsk_bindings *t, u64 revision, u64 now_ms, char id[QMSK_ID_MAX + 1]);

/* The binding an attach may bind: the id and the revision must name the same live binding, whose ttl then moves on.
 * NULL when they do not, with *why the refusal to send: FE_REFUSE_NOCALL for an unknown or expired id, or
 * FE_REFUSE_REVISION for the current call being another one. */
struct qmsk_binding *qmsk_binding_attach(struct qmsk_bindings *t, const char *id, u64 revision, u64 now_ms,
	enum fe_refusal *why);

/* queue one playback frame: 0, -ENOSPC when the 25 are waiting, -EINVAL for a dropped binding */
int qmsk_binding_push(struct qmsk_binding *b, const struct qmsk_pcm *f);

/* the oldest waiting frame, removed: 0, or -ENOENT when nothing waits */
int qmsk_binding_pop(struct qmsk_binding *b, struct qmsk_pcm *out);

/* The server. Every call is non-blocking and returns 0 on success, a positive count or type where one is documented,
 * or -errno. Nothing is printed and nothing is logged. */

void qmsk_server_init(struct qmsk_server *s);

/* bind and listen at `path`, 0600, replacing a socket file a killed daemon left behind: 0 or -errno */
int qmsk_server_open(struct qmsk_server *s, const char *path);

/* close every peer, then the listening socket, and unlink the path */
void qmsk_server_close(struct qmsk_server *s);

/* one connection: 1 accepted with its peer slot in *slot (NULL to drop it), 0 when none waits, -ENOSPC when the eight
 * slots are taken (the socket is closed at once), -errno */
int qmsk_server_accept(struct qmsk_server *s, unsigned int *slot);

/* Take one frame from peer `i` at time now_ms: the frame's type when it was taken, 0 when nothing waits,
 * -ECONNRESET when the peer closed (its slot is free), -EPROTO when the frame was refused - the ERROR went out and
 * the peer is closed - or -ENOSPC when the playback queue was full and the frame was not queued (the peer stays). */
int qmsk_server_read(struct qmsk_server *s, unsigned int i, u64 now_ms);

/* the binding a peer attached, or NULL; the ttl is not touched, so a frame from the peer is what keeps it alive */
struct qmsk_binding *qmsk_server_peer_binding(struct qmsk_server *s, unsigned int i);

/* send one media frame, one state change or one error to peer `i`: 0, -EAGAIN when the peer is not reading, -errno */
int qmsk_server_send_capture(struct qmsk_server *s, unsigned int i, const struct qmsk_pcm *f);
int qmsk_server_send_state(struct qmsk_server *s, unsigned int i, enum fe_state state, u32 revision);
int qmsk_server_send_error(struct qmsk_server *s, unsigned int i, enum fe_refusal code);

/* close peer `i` and free its slot; the binding it attached stays until its ttl, so a reconnect can attach again */
void qmsk_server_drop_peer(struct qmsk_server *s, unsigned int i);

#endif /* AW_PHONED_QMSK_SERVER_H */
