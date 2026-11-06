// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * The ubus object awvoice: the method handlers and the event sender around the reply shapes of reply.c. The
 * handlers run inside the daemon's event loop, between two steps of the line; they only read the core's status or
 * make the one short call the method means. The core's events are sent from a request that is not waited for.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>

#include <linux/awvoice-fxs.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "api.h"
#include "indicator.h"
#include "machine.h"
#include "recording.h"
#include "reply.h"
#include "call_arbiter.h"
#include "sip_guard.h"
#include "slic_map.h"
#include "storage.h"
#include "web_audio.h"
#include "web_proto.h"

static struct api_ctx *the_api;		/* the object's handlers have no user pointer: one object, one daemon */
static struct blob_buf reply_buf, event_buf;

static unsigned long long mono_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return now.tv_sec * 1000ull + now.tv_nsec / 1000000;
}

/* ---- the blobmsg sink ------------------------------------------------------------------------------------------------- */

#define NEST_MAX	4

struct blob_out {
	struct blob_buf *buf;
	void *cookie[NEST_MAX];
	int depth;
};

static void bo_open(void *ctx, const char *name, int array)
{
	struct blob_out *o = ctx;

	if (o->depth >= NEST_MAX)
		return;
	o->cookie[o->depth++] = array ? blobmsg_open_array(o->buf, name ? name : "") : blobmsg_open_table(o->buf, name ? name : "");
}

static void bo_close(void *ctx)
{
	struct blob_out *o = ctx;

	if (o->depth)
		blobmsg_close_table(o->buf, o->cookie[--o->depth]);
}

static void bo_text(void *ctx, const char *name, const char *value)
{
	blobmsg_add_string(((struct blob_out *)ctx)->buf, name, value);
}

static void bo_number(void *ctx, const char *name, long long value)
{
	struct blob_buf *b = ((struct blob_out *)ctx)->buf;

	if (value >= INT32_MIN && value <= INT32_MAX)
		blobmsg_add_u32(b, name, (uint32_t)(int32_t)value);	/* read back as a signed 32-bit number */
	else
		blobmsg_add_u64(b, name, (uint64_t)value);
}

static void bo_flag(void *ctx, const char *name, int value)
{
	blobmsg_add_u8(((struct blob_out *)ctx)->buf, name, value != 0);
}

static void bo_null(void *ctx, const char *name)
{
	blobmsg_add_field(((struct blob_out *)ctx)->buf, BLOBMSG_TYPE_UNSPEC, name, NULL, 0);
}

static void sink_for(struct reply_sink *sink, struct blob_out *out, struct blob_buf *buf)
{
	*out = (struct blob_out){ .buf = buf };
	*sink = (struct reply_sink){ .ctx = out, .open = bo_open, .close = bo_close, .text = bo_text,
		.number = bo_number, .flag = bo_flag, .null = bo_null };
}

/* ---- what the replies need ----------------------------------------------------------------------------------------------- */

/* who holds the qmodem_voip name: this daemon while the object is registered, the owner the guard found otherwise, or
 * nobody. the guard's word is the name's alone: with the SIP feature off nothing is registered and nobody is named */
static const char *object_owner_word(const struct api_ctx *a)
{
	if (a->sip_registered)
		return "awvoice";
	if (a->guard && a->guard->name_taken)
		return "voipd";
	return "none";
}

static void info_of(const struct api_ctx *a, struct reply_info *info)
{
	*info = (struct reply_info){
		.line_in_service = a->phone != NULL,
		.my_number = a->my_number,
		.channel = a->channel,
		.at_interface = a->at_interface,
		.port = a->port ? a->port : "",
		.module_loaded = access("/sys/module/awvoice_fxs", F_OK) == 0,
		.ind = a->ind,
		.dnd = a->dnd,
		.hd = a->hd,
		.layout = a->layout,
		.region = a->region,
		.machine_delay_s = a->machine_delay_s,
		.record_path = a->record_path,
		.message_path = a->message_path,
		.record_store = a->record_store,
		.message_store = a->message_store,
		.sip_mode = a->sip_mode,
		.sip_object_owner = object_owner_word(a),
		.sip_port_owner = a->guard ? a->guard->port_owner : NULL,
		.sip_last_refusal = a->sip_refusal,
		.sip_consumer_pid = a->sip_consumer_pid,
		.sip_web_audio = a->web ? web_audio_state(a->web) : a->sip_web_audio,
		.sip_web_port = a->web_port,
		.sip_web_addr = a->web_addr,
	};
	if (a->machine) {
		/* what the daemon holds, whatever the settings ask for: the load's own finding, so the reply and the audio
		 * cannot drift apart, and the word for a file it could not use */
		info->machine_greeting = a->machine->greeting_loaded;
		info->machine_greeting_ms = a->machine->greeting_ms;
		info->machine_greeting_err = reply_greeting_word(a->machine->greeting_loaded ? 0 : a->machine->greeting_err);
	}
	if (a->phone) {
		struct phone_audio_view av;

		phone_audio_view_of(a->phone, &av);
		info->audio_ok = 1;
		info->audio_up = av.up;
		info->audio_wanted = av.wanted;
		info->audio_reason = av.reason;
		info->audio_claims = av.claims;
		info->audio_starts = av.starts;
		info->audio_stops = av.stops;
		info->audio_up_ms = av.since_ms;
		info->audio_error = av.last_error;
	}
	if (a->bridge_stats) {
		struct awfxs_bridge_stats bs;

		memset(&bs, 0, sizeof(bs));
		if (!a->bridge_stats(a->ctx, &bs)) {
			info->bridge_ok = 1;
			info->bridge_frames = bs.frames;
			info->bridge_running = bs.running;
			info->bridge_resyncs = bs.resyncs;
			info->bridge_tx_skipped = bs.tx_skipped;
			info->bridge_max_gap_us = bs.max_gap_us;
		}
	}
}

static void status_of(const struct api_ctx *a, struct phone_status *st)
{
	if (a->phone)
		phone_status(a->phone, st);
	else
		memset(st, 0, sizeof(*st));
}

/* ---- the methods ------------------------------------------------------------------------------------------------------------ */

static int send_reply(struct ubus_context *ctx, struct ubus_request_data *req)
{
	return ubus_send_reply(ctx, req, reply_buf.head);
}

static int m_status(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct phone_status st;
	struct reply_info info;
	struct reply_sink sink;
	struct blob_out out;

	(void)obj; (void)method; (void)msg;
	status_of(the_api, &st);
	info_of(the_api, &info);
	blob_buf_init(&reply_buf, 0);
	sink_for(&sink, &out, &reply_buf);
	reply_status(&sink, &st, &info, mono_ms());
	return send_reply(ctx, req);
}

static int m_health(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct phone_status st;
	struct reply_info info;
	struct reply_sink sink;
	struct blob_out out;

	(void)obj; (void)method; (void)msg;
	status_of(the_api, &st);
	info_of(the_api, &info);
	info.nleds = indicator_scan_leds(the_api->leds_buf, sizeof(the_api->leds_buf), the_api->leds,
		(unsigned int)(sizeof(the_api->leds) / sizeof(the_api->leds[0])));
	info.leds = the_api->leds;
	blob_buf_init(&reply_buf, 0);
	sink_for(&sink, &out, &reply_buf);
	reply_health(&sink, &st, &info, mono_ms());
	return send_reply(ctx, req);
}

/* the page's own view of the line's call: the same fields the qmodem_voip status carries, read from the call's own
 * arbiter (call_arbiter.h) so the page shows the call, the number and its audio counters in every mode, with no object */
static int m_call_status(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct reply_sink sink;
	struct blob_out out;

	(void)obj; (void)method; (void)msg;
	if (!the_api->arbiter)
		return UBUS_STATUS_NOT_FOUND;
	blob_buf_init(&reply_buf, 0);
	sink_for(&sink, &out, &reply_buf);
	arbiter_status(the_api->arbiter, &sink);
	return send_reply(ctx, req);
}

/* { ok, refused } for a method that acted (err 0) or was refused */
static int action_reply(struct ubus_context *ctx, struct ubus_request_data *req, int err, int off_hook)
{
	const char *refused = reply_refusal(err, off_hook);

	blob_buf_init(&reply_buf, 0);
	blobmsg_add_u8(&reply_buf, "ok", refused == NULL);
	if (refused)
		blobmsg_add_string(&reply_buf, "refused", refused);
	else
		blobmsg_add_field(&reply_buf, BLOBMSG_TYPE_UNSPEC, "refused", NULL, 0);
	return send_reply(ctx, req);
}

static int m_hangup(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	return action_reply(ctx, req, the_api->phone ? phone_hangup(the_api->phone) : -ENODEV, 0);
}

enum { RT_SECONDS, RT_MAX };
static const struct blobmsg_policy ring_policy[RT_MAX] = { [RT_SECONDS] = { .name = "seconds", .type = BLOBMSG_TYPE_INT32 } };

static int m_ring_test(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[RT_MAX];
	struct phone_status st;
	int err;
	uint32_t seconds;

	(void)obj; (void)method;
	blobmsg_parse(ring_policy, RT_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[RT_SECONDS])
		return UBUS_STATUS_INVALID_ARGUMENT;
	seconds = blobmsg_get_u32(tb[RT_SECONDS]);
	if (seconds < 1 || seconds > RING_TEST_MAX_SECONDS)
		return UBUS_STATUS_INVALID_ARGUMENT;
	status_of(the_api, &st);
	err = the_api->phone ? phone_ring_test(the_api->phone, seconds) : -ENODEV;
	if (err == -EINVAL)
		err = -EOPNOTSUPP;		/* the ring generator is not loaded: not available */
	return action_reply(ctx, req, err, st.off_hook);
}

static int m_reload(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	blob_buf_init(&reply_buf, 0);
	blobmsg_add_u8(&reply_buf, "ok", the_api->reload ? the_api->reload(the_api->ctx) == 0 : 0);
	return send_reply(ctx, req);
}

static int m_forwarding_get(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct phone_status st;
	struct reply_sink sink;
	struct blob_out out;

	(void)obj; (void)method; (void)msg;
	status_of(the_api, &st);
	if (!the_api->phone)
		st.fwd.network = PHONE_UNKNOWN;
	blob_buf_init(&reply_buf, 0);
	sink_for(&sink, &out, &reply_buf);
	reply_forwarding(&sink, &st.fwd);
	return send_reply(ctx, req);
}

static int m_calls(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct reply_sink sink;
	struct blob_out out;

	(void)obj; (void)method; (void)msg;
	blob_buf_init(&reply_buf, 0);
	sink_for(&sink, &out, &reply_buf);
	if (the_api->calls)
		reply_calls(&sink, the_api->calls, the_api->book);
	else
		blobmsg_close_array(&reply_buf, blobmsg_open_array(&reply_buf, "calls"));
	return send_reply(ctx, req);
}

enum { CD_TYPE, CD_MAX };
static const struct blobmsg_policy calls_delete_policy[CD_MAX] = { [CD_TYPE] = { .name = "type", .type = BLOBMSG_TYPE_STRING } };

static int m_calls_delete(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[CD_MAX];
	enum calllog_type type;
	unsigned int removed;

	(void)obj; (void)method;
	if (!the_api->calls)
		return UBUS_STATUS_NOT_SUPPORTED;
	blobmsg_parse(calls_delete_policy, CD_MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[CD_TYPE] && calllog_type_parse(blobmsg_get_string(tb[CD_TYPE]), &type))
		return UBUS_STATUS_INVALID_ARGUMENT;
	removed = calllog_delete(the_api->calls, tb[CD_TYPE] ? &type : NULL);
	(void)calllog_persist(the_api->calls, mono_ms(), 0);
	blob_buf_init(&reply_buf, 0);
	blobmsg_add_u8(&reply_buf, "ok", 1);
	blobmsg_add_u32(&reply_buf, "removed", removed);
	return send_reply(ctx, req);
}

/* ---- the recordings and the machine's messages: the files are named <epoch>-<call id>.wav, nothing else is served --- */

#define RECORDING_CHUNK	24576		/* raw bytes per reply: 32768 base64 characters, inside a ubus reply */

/* only the daemon's own file names pass, by the same rule the folder's prune and its tmp clean use (storage.h):
 * <epoch>-<call id>.wav. A path, a dot or another program's file can never get through */
static int recording_name_ok(const char *name)
{
	return storage_name_ok(name);
}

static void recording_path_of(const char *dir, const char *name, char *out, size_t cap)
{
	snprintf(out, cap, "%s/%s", dir ? dir : "", name);
}

/* the epoch a name begins with: the call's answer, by the recorder's naming rule; -1 when it does not carry one
 * (the reply carries it as a 32-bit number) */
static long long name_epoch(const char *name)
{
	unsigned long long epoch, id;

	if (storage_name_ids(name, &epoch, &id) || epoch > 0xffffffffull)
		return -1;
	return (long long)epoch;
}

/* one directory of the daemon's finished WAV files, as the array `field`: a file whose header does not probe (one
 * still being written, or not one this program wrote) is never named. `with_ts` adds the epoch in the name, for the message list. */
static void list_wavs(const char *dir, const char *field, int with_ts)
{
	char path[256];
	struct dirent *e;
	DIR *d = dir ? opendir(dir) : NULL;
	void *array = blobmsg_open_array(&reply_buf, field);

	while (d && (e = readdir(d)) != NULL) {
		unsigned char hdr[44];
		long long ts = with_ts ? name_epoch(e->d_name) : -1;
		u32 rate = 0, channels = 0, data = 0;
		void *entry;
		FILE *f;

		if (!recording_name_ok(e->d_name))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path))
			continue;
		f = fopen(path, "r");
		if (!f || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || recording_wav_probe(hdr, &rate, &channels, &data)) {
			if (f)
				fclose(f);
			continue;		/* not a finished recording of this program's: never named, never listed */
		}
		fclose(f);
		entry = blobmsg_open_table(&reply_buf, NULL);
		blobmsg_add_string(&reply_buf, "name", e->d_name);
		blobmsg_add_u32(&reply_buf, "bytes", data + 44);
		blobmsg_add_u32(&reply_buf, "rate", rate);
		blobmsg_add_u32(&reply_buf, "channels", channels);
		blobmsg_add_u32(&reply_buf, "seconds", rate ? data / rate / channels / 2 : 0);	/* two bytes a sample */
		if (ts >= 0)
			blobmsg_add_u32(&reply_buf, "ts", (uint32_t)ts);
		blobmsg_close_table(&reply_buf, entry);
	}
	if (d)
		closedir(d);
	blobmsg_close_array(&reply_buf, array);
}

static int m_recordings(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	blob_buf_init(&reply_buf, 0);
	list_wavs(the_api->record_path, "recordings", 0);
	return send_reply(ctx, req);
}

static int m_messages(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	blob_buf_init(&reply_buf, 0);
	list_wavs(the_api->message_path, "messages", 1);
	return send_reply(ctx, req);
}

enum { RG_NAME, RG_OFFSET, RG_MAX };
static const struct blobmsg_policy recording_get_policy[RG_MAX] = {
	[RG_NAME] = { .name = "name", .type = BLOBMSG_TYPE_STRING },
	[RG_OFFSET] = { .name = "offset", .type = BLOBMSG_TYPE_INT32 }
};

/*
 * One chunk of one file the daemon serves whole, base64, from `offset`: the reply names the file's total, the offset it
 * starts at and whether more follows, which is how the page walks a file to its end. A ubus reply is small, so the raw
 * piece is RECORDING_CHUNK. `path` is the daemon's own - a folder listing's name or the greeting's one constant - never
 * something the caller put together, and `name` is the handle the page asked with (NULL for a file with no name to
 * echo, which is the greeting: there is only ever one).
 */
static int file_chunk_at(struct ubus_context *ctx, struct ubus_request_data *req, const char *path,
	const char *name, u32 offset)
{
	unsigned char *raw;
	char *b64;
	FILE *f;
	struct stat st;
	u32 take;

	f = fopen(path, "r");
	if (!f)
		return UBUS_STATUS_NOT_FOUND;
	/* offset is a plain file offset: 0 carries the header, so the page assembles the exact file */
	if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || offset > (u32)st.st_size) {
		fclose(f);
		return UBUS_STATUS_INVALID_ARGUMENT;
	}
	take = (u32)st.st_size - offset;
	if (take > RECORDING_CHUNK)
		take = RECORDING_CHUNK;
	if (offset)
		fseek(f, offset, SEEK_SET);
	raw = malloc(take ? take : 1);
	b64 = malloc(take ? take * 2 : 2);
	if (!raw || !b64) {
		free(raw); free(b64); fclose(f);
		return UBUS_STATUS_UNKNOWN_ERROR;
	}
	if (take && fread(raw, 1, take, f) != take) {
		free(raw); free(b64); fclose(f);
		return UBUS_STATUS_UNKNOWN_ERROR;
	}
	fclose(f);
	recording_base64(raw, take, b64);
	blob_buf_init(&reply_buf, 0);
	if (name)
		blobmsg_add_string(&reply_buf, "name", name);
	blobmsg_add_u32(&reply_buf, "total", (u32)st.st_size);
	blobmsg_add_u32(&reply_buf, "offset", offset);
	blobmsg_add_string(&reply_buf, "b64", b64);
	blobmsg_add_u8(&reply_buf, "more", offset + take < (u32)st.st_size);
	free(raw);
	free(b64);
	return send_reply(ctx, req);
}

/* one chunk of a file of `dir`, base64 (a ubus reply is small; the page walks the offsets until "more" is 0) */
static int file_chunk(struct ubus_context *ctx, struct ubus_request_data *req, struct blob_attr *msg, const char *dir)
{
	char path[256];
	struct blob_attr *tb[RG_MAX];
	const char *name;
	u32 offset;

	blobmsg_parse(recording_get_policy, RG_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[RG_NAME] || !tb[RG_OFFSET])
		return UBUS_STATUS_INVALID_ARGUMENT;
	name = blobmsg_get_string(tb[RG_NAME]);
	offset = blobmsg_get_u32(tb[RG_OFFSET]);
	if (!recording_name_ok(name) || !dir)
		return UBUS_STATUS_INVALID_ARGUMENT;
	recording_path_of(dir, name, path, sizeof(path));
	return file_chunk_at(ctx, req, path, name, offset);
}

static int m_recording_get(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method;
	return file_chunk(ctx, req, msg, the_api->record_path);
}

static int m_message_get(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method;
	return file_chunk(ctx, req, msg, the_api->message_path);
}

enum { RD_NAME, RD_MAX };
static const struct blobmsg_policy recording_delete_policy[RD_MAX] = { [RD_NAME] = { .name = "name", .type = BLOBMSG_TYPE_STRING } };

static int file_delete(struct ubus_context *ctx, struct ubus_request_data *req, struct blob_attr *msg, const char *dir)
{
	char path[256];
	struct blob_attr *tb[RD_MAX];
	const char *name;
	int removed;

	blobmsg_parse(recording_delete_policy, RD_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[RD_NAME])
		return UBUS_STATUS_INVALID_ARGUMENT;
	name = blobmsg_get_string(tb[RD_NAME]);
	if (!recording_name_ok(name) || !dir)
		return UBUS_STATUS_INVALID_ARGUMENT;
	recording_path_of(dir, name, path, sizeof(path));
	removed = unlink(path) == 0;
	blob_buf_init(&reply_buf, 0);
	blobmsg_add_u8(&reply_buf, "ok", removed);
	blobmsg_add_u8(&reply_buf, "removed", removed);
	return send_reply(ctx, req);
}

static int m_recording_delete(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method;
	return file_delete(ctx, req, msg, the_api->record_path);
}

static int m_message_delete(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method;
	return file_delete(ctx, req, msg, the_api->message_path);
}

/* ---- the greeting: the one managed file, read whole in chunks and replaced only by a commit that proves it -------- */

enum { GRG_OFFSET, GRG_MAX };
static const struct blobmsg_policy greeting_get_policy[GRG_MAX] = {
	[GRG_OFFSET] = { .name = "offset", .type = BLOBMSG_TYPE_INT32 }
};

enum {GRP_OFFSET, GRP_B64, GRP_MAX};
static const struct blobmsg_policy greeting_put_policy[GRP_MAX] = {
	[GRP_OFFSET] = { .name = "offset", .type = BLOBMSG_TYPE_INT32 },
	[GRP_B64] = { .name = "b64", .type = BLOBMSG_TYPE_STRING }
};

enum { GRC_POLISH, GRC_MAX };
/* INT32 like every other number this object takes: a JSON integer reaches ubus as INT32 (or INT64), so a narrower
 * policy type would never match and the argument would be dropped without a word about it */
static const struct blobmsg_policy greeting_commit_policy[GRC_MAX] = {
	[GRC_POLISH] = { .name = "polish", .type = BLOBMSG_TYPE_INT32 }
};

/* { ok: 0, refused: word }: a refusal the page reads as a body, the way the call actions already do, so rpcd hands it
 * on instead of turning it into an error the tab cannot name */
static int greeting_refused(struct ubus_context *ctx, struct ubus_request_data *req, int err)
{
	blob_buf_init(&reply_buf, 0);
	blobmsg_add_u8(&reply_buf, "ok", 0);
	blobmsg_add_string(&reply_buf, "refused", reply_greeting_word(err));
	return send_reply(ctx, req);
}

/* the machine and its staging, or NULL when this daemon has no answering machine to manage */
static int greeting_targets(struct machine **m, struct machine_upload **up)
{
	if (!the_api->machine || !the_api->greeting_up)
		return 0;
	*m = the_api->machine;
	*up = the_api->greeting_up;
	return 1;
}

static int m_greeting_get(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[GRG_MAX];
	u32 offset;

	(void)obj; (void)method;
	blobmsg_parse(greeting_get_policy, GRG_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[GRG_OFFSET])
		return UBUS_STATUS_INVALID_ARGUMENT;
	offset = blobmsg_get_u32(tb[GRG_OFFSET]);
	/* no name travels: the greeting is one constant file, so a caller can only ever read what the machine plays */
	return file_chunk_at(ctx, req, MACHINE_GREETING_PATH, NULL, offset);
}

static int m_greeting_begin(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct machine *m;
	struct machine_upload *up;
	int err;

	(void)obj; (void)method; (void)msg;
	if (!greeting_targets(&m, &up))
		return UBUS_STATUS_NOT_FOUND;
	/* staging is allowed while a call is up: the pieces are not the greeting yet, the way a message is written
	 * during the call it belongs to */
	err = machine_upload_begin(up, m, MACHINE_GREETING_DIR);
	if (err)
		return greeting_refused(ctx, req, err);
	blob_buf_init(&reply_buf, 0);
	blobmsg_add_u8(&reply_buf, "ok", 1);
	return send_reply(ctx, req);
}

static int greeting_put_write(struct ubus_context *ctx, struct ubus_request_data *req, struct blob_attr *msg, struct machine *m, struct machine_upload *up)
{
	unsigned char *plain;
	struct blob_attr *tb[GRP_MAX];
	const char *b64;
	u32 encoded, want, len = 0, offset;
	int err;

	blobmsg_parse(greeting_put_policy, GRP_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[GRP_OFFSET] || !tb[GRP_B64])
		return UBUS_STATUS_INVALID_ARGUMENT;
	offset = blobmsg_get_u32(tb[GRP_OFFSET]);
	b64 = blobmsg_get_string(tb[GRP_B64]);
	encoded = blobmsg_data_len(tb[GRP_B64]) - 1;	/* the string's own bytes, not the terminator blobmsg adds */
	if (recording_base64_plain_len(b64, encoded, &want) || !want)
		return UBUS_STATUS_INVALID_ARGUMENT;	/* empty, unquaddable: a call the daemon will not make */
	if (want > MACHINE_GREETING_PUT_MAX)
		return UBUS_STATUS_INVALID_ARGUMENT;	/* a piece bigger than the daemon ever takes, in bytes that are there */
	plain = malloc(want);
	if (!plain)
		return UBUS_STATUS_UNKNOWN_ERROR;
	err = recording_base64_decode(b64, encoded, plain, want, &len);
	if (!err && len != want)
		err = -EINVAL;		/* the counter and the decoder disagree: nothing this daemon produced goes on the line */
	if (err) {
		free(plain);
		/* a piece that is not base64 at all is the caller's mistake rather than the file's: the staging stays open
		 * so the page can abort it, and the answer is the plain ubus argument error */
		return UBUS_STATUS_INVALID_ARGUMENT;
	}
	err = machine_upload_put(up, m, MACHINE_GREETING_DIR, offset, plain, len);
	free(plain);
	if (err == -EINVAL)
		return UBUS_STATUS_INVALID_ARGUMENT;	/* nothing is staged: begin was never called, or it was dropped */
	if (err)
		return greeting_refused(ctx, req, err);
	blob_buf_init(&reply_buf, 0);
	blobmsg_add_u8(&reply_buf, "ok", 1);
	blobmsg_add_u32(&reply_buf, "size", up->staged);
	return send_reply(ctx, req);
}

static int m_greeting_put(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct machine *m;
	struct machine_upload *up;

	(void)obj; (void)method;
	if (!greeting_targets(&m, &up))
		return UBUS_STATUS_NOT_FOUND;
	return greeting_put_write(ctx, req, msg, m, up);
}

static int m_greeting_commit(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[GRC_MAX];
	struct machine *m;
	struct machine_upload *up;
	unsigned int ms = 0;
	int err;

	(void)obj; (void)method;
	if (!greeting_targets(&m, &up))
		return UBUS_STATUS_NOT_FOUND;
	blobmsg_parse(greeting_commit_policy, GRC_MAX, tb, blob_data(msg), blob_len(msg));
	/* this is the one call that replaces the file the machine plays, so it waits for the line: a call up, the handset
	 * off hook, the greeting under the fill or the playback ring armed for anyone at all is a refusal, never a deferral */
	if (!the_api->greeting_may_swap || !the_api->greeting_may_swap(the_api->ctx))
		return greeting_refused(ctx, req, -EBUSY);
	err = machine_upload_commit(up, m, MACHINE_GREETING_DIR,
		tb[GRC_POLISH] && blobmsg_get_u32(tb[GRC_POLISH]) ? 1 : 0, &ms);
	if (err)
		return greeting_refused(ctx, req, err);
	/* the core caches its answer flag: without this push a new greeting would not answer until the next settings save,
	 * and a greeting that stopped playing would keep answering */
	if (the_api->greeting_applied)
		the_api->greeting_applied(the_api->ctx);
	blob_buf_init(&reply_buf, 0);
	blobmsg_add_u8(&reply_buf, "ok", 1);
	blobmsg_add_u32(&reply_buf, "ms", ms);
	return send_reply(ctx, req);
}

static int m_greeting_abort(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct machine *m;
	struct machine_upload *up;

	(void)obj; (void)method; (void)msg;
	if (!greeting_targets(&m, &up))
		return UBUS_STATUS_NOT_FOUND;
	(void)machine_upload_abort(up, m, MACHINE_GREETING_DIR);	/* nothing staged is the result the page wants either way */
	blob_buf_init(&reply_buf, 0);
	blobmsg_add_u8(&reply_buf, "ok", 1);
	return send_reply(ctx, req);
}

/* the line test answers late: the request is held until the samples are in (api_tick) */
static struct ubus_request_data lt_req;
static struct ubus_context *lt_ctx;
static int lt_pending;

static int m_line_test(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct phone_status st;
	int err;

	(void)obj; (void)method; (void)msg;
	status_of(the_api, &st);
	err = the_api->phone ? phone_line_test(the_api->phone) : -ENODEV;
	if (err)
		return action_reply(ctx, req, err, st.off_hook);
	lt_pending = 1;
	lt_ctx = ctx;
	ubus_defer_request(ctx, req, &lt_req);
	return 0;
}

void api_tick(struct api_ctx *a)
{
	struct phone_line_test_result res;
	struct reply_sink sink;
	struct blob_out out;

	if (!lt_pending || !a->phone || !phone_line_test_poll(a->phone, &res))
		return;
	lt_pending = 0;
	blob_buf_init(&reply_buf, 0);
	sink_for(&sink, &out, &reply_buf);
	reply_line_test(&sink, &res);
	ubus_send_reply(lt_ctx, &lt_req, reply_buf.head);
	ubus_complete_deferred_request(lt_ctx, &lt_req, UBUS_STATUS_OK);
}

/* ---- qmodem_voip: the front end's object and its one topic -------------------------------------------------------------- */

/* The arbiter writes its reply through the same sink, so the body already says ok or refused; its return value is the
 * action's own errno and the ubus call itself succeeded. */
static int sip_reply(struct ubus_context *ctx, struct ubus_request_data *req, enum sip_method m, const struct sip_args *a)
{
	struct reply_sink sink;
	struct blob_out out;
	int ret;

	if (!the_api->arbiter)
		return UBUS_STATUS_NOT_FOUND;		/* no front-end is wired: the object is not registered either */
	blob_buf_init(&reply_buf, 0);
	sink_for(&sink, &out, &reply_buf);
	ret = arbiter_request(the_api->arbiter, m, a, &sink, stdout);
	(void)send_reply(ctx, req);
	/* the front end reads the call's status: a refusal must reach it as an error, not a body it does not parse */
	return ret < 0 ? UBUS_STATUS_UNKNOWN_ERROR : 0;
}

static int m_voip_status(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	return sip_reply(ctx, req, SIP_M_STATUS, NULL);
}

static int m_voip_capabilities(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	return sip_reply(ctx, req, SIP_M_CAPABILITIES, NULL);
}

enum { SO_ENDPOINT, SO_NUMBER, SO_ORIGIN, SO_USERNAME, SO_MAX };
static const struct blobmsg_policy voip_originate_policy[SO_MAX] = {
	[SO_ENDPOINT] = { .name = "endpoint", .type = BLOBMSG_TYPE_STRING },
	[SO_NUMBER] = { .name = "number", .type = BLOBMSG_TYPE_STRING },
	[SO_ORIGIN] = { .name = "origin", .type = BLOBMSG_TYPE_STRING },
	[SO_USERNAME] = { .name = "username", .type = BLOBMSG_TYPE_STRING }
};

static int m_voip_originate(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[SO_MAX] = { 0 };
	struct sip_args a;

	(void)obj; (void)method;
	memset(&a, 0, sizeof(a));
	if (msg)
		blobmsg_parse(voip_originate_policy, SO_MAX, tb, blob_data(msg), blob_len(msg));
	a.endpoint = tb[SO_ENDPOINT] ? blobmsg_get_string(tb[SO_ENDPOINT]) : NULL;
	a.number = tb[SO_NUMBER] ? blobmsg_get_string(tb[SO_NUMBER]) : NULL;
	a.origin = tb[SO_ORIGIN] ? blobmsg_get_string(tb[SO_ORIGIN]) : NULL;
	a.username = tb[SO_USERNAME] ? blobmsg_get_string(tb[SO_USERNAME]) : NULL;
	a.via_sip = 1;			/* the installed service's door: its leg is the one that dialled */
	return sip_reply(ctx, req, SIP_M_ORIGINATE, &a);
}

/* the front end answers a ringing call and attaches its audio at once: its status fetch must already say active, or
 * the attach fails and the front end hangs the call up. The modem takes a moment to show the call active, so the
 * reply is held until then (or the deadline, or the call ending). */
#define SIP_ANSWER_WAIT_MS	6000

static void voip_answer_done(int ok)
{
	struct api_ctx *a = the_api;

	if (!a || !a->voip_answer_held)
		return;
	a->voip_answer_held = 0;
	uloop_timeout_cancel(&a->voip_answer_timer);
	blob_buf_free(&reply_buf);	/* the accepted answer's first take is replaced by this reply */
	blob_buf_init(&reply_buf, 0);
	blobmsg_add_u8(&reply_buf, "ok", ok ? 1 : 0);
	if (ok)
		blobmsg_add_field(&reply_buf, BLOBMSG_TYPE_UNSPEC, "refused", NULL, 0);
	else
		blobmsg_add_string(&reply_buf, "refused", "no-answer");
	ubus_send_reply(a->ubus, &a->voip_answer_defer, reply_buf.head);
	ubus_complete_deferred_request(a->ubus, &a->voip_answer_defer, ok ? 0 : UBUS_STATUS_UNKNOWN_ERROR);
}

static void voip_answer_expired(struct uloop_timeout *t)
{
	(void)t;
	voip_answer_done(0);
}

void api_voip_answer_settle(struct api_ctx *a, int ok)
{
	if (a && a->voip_answer_held)
		voip_answer_done(ok);
}

static int m_voip_answer(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct reply_sink sink;
	struct blob_out out;
	int ret;

	struct sip_args a;

	(void)obj; (void)method; (void)msg;
	if (!the_api->arbiter)
		return UBUS_STATUS_NOT_FOUND;
	memset(&a, 0, sizeof(a));
	a.via_sip = 1;			/* the installed service's door: its leg is the one that answered */
	blob_buf_init(&reply_buf, 0);
	sink_for(&sink, &out, &reply_buf);
	ret = arbiter_request(the_api->arbiter, SIP_M_ANSWER, &a, &sink, stdout);
	if (ret == 0 && fe_call_view(the_api->arbiter->fe)->state != FE_STATE_ACTIVE) {
		ubus_defer_request(ctx, req, &the_api->voip_answer_defer);
		the_api->voip_answer_held = 1;
		uloop_timeout_set(&the_api->voip_answer_timer, SIP_ANSWER_WAIT_MS);
		return 0;
	}
	(void)send_reply(ctx, req);
	return ret < 0 ? UBUS_STATUS_UNKNOWN_ERROR : 0;
}

static int m_voip_reject(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	return sip_reply(ctx, req, SIP_M_REJECT, NULL);
}

static int m_voip_hangup(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	return sip_reply(ctx, req, SIP_M_HANGUP, NULL);
}

enum { SD_DIGIT, SD_MAX };
static const struct blobmsg_policy voip_dtmf_policy[SD_MAX] = { [SD_DIGIT] = { .name = "digit", .type = BLOBMSG_TYPE_STRING } };

static int m_voip_dtmf(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[SD_MAX] = { 0 };
	struct sip_args a;
	const char *digit = NULL;

	(void)obj; (void)method;
	memset(&a, 0, sizeof(a));
	if (msg)
		blobmsg_parse(voip_dtmf_policy, SD_MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[SD_DIGIT])
		digit = blobmsg_get_string(tb[SD_DIGIT]);
	a.digit = digit ? digit[0] : 0;		/* a longer string is cut to its first digit; the core judges the digit */
	return sip_reply(ctx, req, SIP_M_SEND_DTMF, &a);
}

/* ---- the page's own call control: the line's call arbiter, with the reply body always delivered ---- */

/* rpcd hands a caller the body only when the call itself succeeded, so an arbiter refusal would lose its word on the way
 * to the page. These five methods answer the body whatever the arbiter decided; the number stays inside this daemon. */
static int web_call(struct ubus_context *ctx, struct ubus_request_data *req, enum sip_method m, const struct sip_args *a)
{
	struct reply_sink sink;
	struct blob_out out;

	if (!the_api->arbiter)
		return UBUS_STATUS_NOT_FOUND;
	blob_buf_init(&reply_buf, 0);
	sink_for(&sink, &out, &reply_buf);
	(void)arbiter_request(the_api->arbiter, m, a, &sink, stdout);
	return send_reply(ctx, req);
}

enum { SW_NUMBER, SW_MAX };
static const struct blobmsg_policy web_originate_policy[SW_MAX] = {
	[SW_NUMBER] = { .name = "number", .type = BLOBMSG_TYPE_STRING }
};

/* the page is a control surface of this daemon: its endpoint is browser and its origin awvoice-web, both fixed here */
static int m_web_originate(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[SW_MAX] = { 0 };
	struct sip_args a;

	(void)obj; (void)method;
	memset(&a, 0, sizeof(a));
	if (msg)
		blobmsg_parse(web_originate_policy, SW_MAX, tb, blob_data(msg), blob_len(msg));
	a.endpoint = "browser";
	a.origin = "awvoice-web";
	a.number = tb[SW_NUMBER] ? blobmsg_get_string(tb[SW_NUMBER]) : "";
	return web_call(ctx, req, SIP_M_ORIGINATE, &a);
}

static int m_web_answer(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	return web_call(ctx, req, SIP_M_ANSWER, NULL);
}

static int m_web_reject(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	return web_call(ctx, req, SIP_M_REJECT, NULL);
}

static int m_web_hangup(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	return web_call(ctx, req, SIP_M_HANGUP, NULL);
}

enum { SD_WEB_DIGIT, SD_WEB_MAX };
static const struct blobmsg_policy web_dtmf_policy[SD_WEB_MAX] = {
	[SD_WEB_DIGIT] = { .name = "digit", .type = BLOBMSG_TYPE_STRING }
};

static int m_web_dtmf(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[SD_WEB_MAX] = { 0 };
	struct sip_args a;
	const char *digit = NULL;

	(void)obj; (void)method;
	memset(&a, 0, sizeof(a));
	if (msg)
		blobmsg_parse(web_dtmf_policy, SD_WEB_MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[SD_WEB_DIGIT])
		digit = blobmsg_get_string(tb[SD_WEB_DIGIT]);
	a.digit = digit ? digit[0] : 0;		/* a longer string is cut to its first digit; the core judges the digit */
	return web_call(ctx, req, SIP_M_SEND_DTMF, &a);
}

/* the page's flash: what the handset's flash key does in a call (hold, swap, take the waiting call, take the held
 * call back); a digit within the flash window is a code, as it is at the handset */
static int m_web_flash(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj; (void)method; (void)msg;
	return web_call(ctx, req, SIP_M_FLASH, NULL);
}

/* a count or a revision read from a blob attribute. the page's JSON-RPC call stores a small integer as a 32-bit one
 * while a native ubus caller sends the 64-bit field, and both name the same number, so a policy that asked for one
 * width refused the other and the caller saw a number it had sent itself come back as absent */
static int blob_u64(struct blob_attr *a, u64 *out)
{
	switch (a ? blobmsg_type(a) : 0) {
	case BLOBMSG_TYPE_INT32:
		*out = blobmsg_get_u32(a);
		return 1;
	case BLOBMSG_TYPE_INT64:
		*out = blobmsg_get_u64(a);
		return 1;
	}
	return 0;
}

enum { SS_REVISION, SS_MAX };
static const struct blobmsg_policy sip_session_policy[SS_MAX] = {
	[SS_REVISION] = { .name = "call_revision", .type = BLOBMSG_TYPE_UNSPEC }
};

/* ---- the page's browser audio: a token for the call the page saw ---------------------------------------------- */

enum { SA_REVISION, SA_MAX };
static const struct blobmsg_policy web_audio_policy[SA_MAX] = {
	[SA_REVISION] = { .name = "call_revision", .type = BLOBMSG_TYPE_UNSPEC }
};

static const char *audio_refusal(int err)
{
	switch (err) {
	case -EBUSY:
		return fe_refusal_name(FE_REFUSE_BUSY);
	case -ENOENT:
		return fe_refusal_name(FE_REFUSE_NOCALL);
	case -ESTALE:
		return fe_refusal_name(FE_REFUSE_REVISION);
	case -EPERM:
		return fe_refusal_name(FE_REFUSE_MODE);
	}
	return "NOT_AVAILABLE";
}

/* The page asks for the line's audio before it opens the encrypted stream; the token is minted for the call revision
 * it saw, and the stream presents it once. A call the page does not hold, a call the installed service is attached
 * to, or a listener that is not up are all refused with the arbiter's own words. */
static int m_web_audio_open(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[SA_MAX] = { 0 };
	const struct fe_call *v = NULL;
	char token[65];
	u64 rev = 0;
	int err = 0;

	(void)obj; (void)method;
	if (msg)
		blobmsg_parse(web_audio_policy, SA_MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[SA_REVISION])
		blob_u64(tb[SA_REVISION], &rev);
	if (!the_api->arbiter)
		err = -EPERM;
	else {
		v = fe_call_view(the_api->arbiter->fe);
		if (the_api->arbiter->off)
			err = -EPERM;
		else if (!v->id || v->state != FE_STATE_ACTIVE)
			err = -ENOENT;
		else if (!rev || rev != v->revision)
			err = -ESTALE;
		else if (v->owner != FE_ANSWER_WEB || the_api->sip_consumer_pid)
			err = -EBUSY;	/* the handset, the machine or the SIP service has this call */
		else if (!the_api->web)
			err = -EPERM;	/* the listener is not up: the health word says why */
		else if (fe_owners(the_api->arbiter->fe)->play != FE_PLAY_NONE)
			err = -EBUSY;
		else
			err = web_audio_mint(the_api->web, rev, mono_ms(), token, sizeof(token));
	}
	blob_buf_init(&reply_buf, 0);
	blobmsg_add_u8(&reply_buf, "ok", err == 0);
	if (err) {
		blobmsg_add_string(&reply_buf, "refused", audio_refusal(err));
	} else {
		blobmsg_add_field(&reply_buf, BLOBMSG_TYPE_UNSPEC, "refused", NULL, 0);
		blobmsg_add_string(&reply_buf, "token", token);
		blobmsg_add_u32(&reply_buf, "port", the_api->web_port);
		blobmsg_add_u32(&reply_buf, "rate", WEB_RATE);
		blobmsg_add_u32(&reply_buf, "frame_ms", WEB_FRAME_MS);
		blobmsg_add_u32(&reply_buf, "expires_ms", 30000);
	}
	return send_reply(ctx, req);
}

static int m_sip_issue_socket_session(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[SS_MAX] = { 0 };
	struct sip_args a;

	(void)obj; (void)method;
	memset(&a, 0, sizeof(a));
	if (msg)
		blobmsg_parse(sip_session_policy, SS_MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[SS_REVISION])
		blob_u64(tb[SS_REVISION], &a.call_revision);	/* a revision is never 0: an absent one is refused */
	return sip_reply(ctx, req, SIP_M_ISSUE_SOCKET_SESSION, &a);
}

static const struct ubus_method sip_methods[] = {
	UBUS_METHOD_NOARG("status", m_voip_status),
	UBUS_METHOD_NOARG("capabilities", m_voip_capabilities),
	UBUS_METHOD("originate", m_voip_originate, voip_originate_policy),
	UBUS_METHOD_NOARG("answer", m_voip_answer),
	UBUS_METHOD_NOARG("reject", m_voip_reject),
	UBUS_METHOD_NOARG("hangup", m_voip_hangup),
	UBUS_METHOD("send_dtmf", m_voip_dtmf, voip_dtmf_policy),
	UBUS_METHOD("issue_socket_session", m_sip_issue_socket_session, sip_session_policy),
};

static struct ubus_object_type sip_object_type = UBUS_OBJECT_TYPE(SIP_OBJECT, sip_methods);
static struct ubus_object sip_object = { .name = SIP_OBJECT, .type = &sip_object_type,
	.methods = sip_methods, .n_methods = ARRAY_SIZE(sip_methods) };

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("status", m_status),
	UBUS_METHOD_NOARG("health", m_health),
	UBUS_METHOD_NOARG("call_status", m_call_status),
	UBUS_METHOD_NOARG("hangup", m_hangup),
	UBUS_METHOD("ring_test", m_ring_test, ring_policy),
	UBUS_METHOD_NOARG("reload", m_reload),
	UBUS_METHOD_NOARG("line_test", m_line_test),
	UBUS_METHOD_NOARG("forwarding_get", m_forwarding_get),
	UBUS_METHOD("web_originate", m_web_originate, web_originate_policy),
	UBUS_METHOD_NOARG("web_answer", m_web_answer),
	UBUS_METHOD_NOARG("web_reject", m_web_reject),
	UBUS_METHOD_NOARG("web_hangup", m_web_hangup),
	UBUS_METHOD("web_dtmf", m_web_dtmf, web_dtmf_policy),
	UBUS_METHOD_NOARG("web_flash", m_web_flash),
	UBUS_METHOD("web_audio_open", m_web_audio_open, web_audio_policy),
	UBUS_METHOD_NOARG("calls", m_calls),
	UBUS_METHOD("calls_delete", m_calls_delete, calls_delete_policy),
	UBUS_METHOD_NOARG("recordings", m_recordings),
	UBUS_METHOD("recording_get", m_recording_get, recording_get_policy),
	UBUS_METHOD("recording_delete", m_recording_delete, recording_delete_policy),
	UBUS_METHOD_NOARG("messages", m_messages),
	UBUS_METHOD("message_get", m_message_get, recording_get_policy),
	UBUS_METHOD("message_delete", m_message_delete, recording_delete_policy),
	UBUS_METHOD("greeting_get", m_greeting_get, greeting_get_policy),
	UBUS_METHOD_NOARG("greeting_begin", m_greeting_begin),
	UBUS_METHOD("greeting_put", m_greeting_put, greeting_put_policy),
	UBUS_METHOD("greeting_commit", m_greeting_commit, greeting_commit_policy),
	UBUS_METHOD_NOARG("greeting_abort", m_greeting_abort),
};

static struct ubus_object_type object_type = UBUS_OBJECT_TYPE("awvoice", methods);
static struct ubus_object object = { .name = "awvoice", .type = &object_type, .methods = methods, .n_methods = ARRAY_SIZE(methods) };

int api_init(struct ubus_context *ubus, struct api_ctx *a)
{
	int err;

	the_api = a;
	a->ubus = ubus;
	a->voip_answer_timer.cb = voip_answer_expired;
	err = ubus_add_object(ubus, &object);
	a->connected = err == 0;
	/* the mode may have been on before the connection: the object follows it now, and a reconnect registers it again */
	a->sip_registered = 0;
	if (a->connected && a->sip_on && a->arbiter && ubus_add_object(ubus, &sip_object) == 0)
		a->sip_registered = 1;
	return err;
}

int api_sip_set(struct api_ctx *a, int on)
{
	int err = 0;

	a->sip_on = on != 0;
	if (!a->ubus || !a->connected)
		return 0;		/* the registration that follows a connection reads sip_on */
	if (a->sip_on && !a->sip_registered) {
		err = ubus_add_object(a->ubus, &sip_object);
		a->sip_registered = err == 0;
	} else if (!a->sip_on && a->sip_registered) {
		ubus_remove_object(a->ubus, &sip_object);
		a->sip_registered = 0;
	}
	return err;
}

/* ---- the events ------------------------------------------------------------------------------------------------------------- */

static void event_done(struct ubus_request *req, int ret)
{
	(void)ret;
	free(req);
}

/* one event, sent without waiting for ubusd's answer; dropped when there is no connection or no memory */
static void send_event(struct api_ctx *a, const char *name, void (*fill)(const struct reply_sink *, const void *), const void *what)
{
	struct ubus_request *req;
	struct reply_sink sink;
	struct blob_out out;
	void *table;

	if (!a->connected || !a->ubus)
		return;
	req = calloc(1, sizeof(*req));
	if (!req)
		return;
	blob_buf_init(&event_buf, 0);
	blobmsg_add_string(&event_buf, "id", name);
	table = blobmsg_open_table(&event_buf, "data");
	sink_for(&sink, &out, &event_buf);
	fill(&sink, what);
	blobmsg_close_table(&event_buf, table);
	if (ubus_invoke_async(a->ubus, UBUS_SYSTEM_OBJECT_EVENT, "send", event_buf.head, req)) {
		free(req);
		return;
	}
	req->complete_cb = event_done;
	ubus_complete_request_async(a->ubus, req);
}

static void fill_line(const struct reply_sink *s, const void *what)
{
	reply_event_line(s, *(const enum phone_line *)what);
}

static void fill_call(const struct reply_sink *s, const void *what)
{
	reply_event_call(s, what, 0);
}

void api_event(void *ctx, const struct phone_event *ev)
{
	struct api_ctx *a = ctx;

	if (ev->kind == PHONE_EV_MACHINE)
		return;			/* not a change of the call itself: the machine is read from the status reply */
	if (ev->kind == PHONE_EV_LINE) {
		send_event(a, "awvoice.line", fill_line, &ev->line);
		return;
	}
	if (ev->call == CALL_GONE || ev->call == CALL_NONE) {
		if (ev->call_id == a->ended_id)
			return;			/* the core reports the end of a call twice (gone, then none) */
		a->ended_id = ev->call_id;
	}
	send_event(a, "awvoice.call", fill_call, ev);
}

static void fill_sip_event(const struct reply_sink *s, const void *what)
{
	arbiter_event(the_api->arbiter, what, s);
}

void api_sip_event(struct api_ctx *a, const struct fe_event *ev)
{
	if (!a->arbiter || !a->sip_registered)
		return;			/* the object is not there: the event would be published on a topic nobody serves */
	send_event(a, SIP_EVENT_TOPIC, fill_sip_event, ev);
}

unsigned long long api_epoch_s(unsigned long long mono_ms_value, unsigned long long now_mono_ms)
{
	unsigned long long now_epoch = (unsigned long long)time(NULL);
	unsigned long long ago = now_mono_ms > mono_ms_value ? (now_mono_ms - mono_ms_value) / 1000 : 0;

	return now_epoch > ago ? now_epoch - ago : 0;
}
