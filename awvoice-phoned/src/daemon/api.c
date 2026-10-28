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
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>

#include <linux/awvoice-fxs.h>

#include <libubox/blobmsg.h>
#include <libubus.h>

#include "api.h"
#include "indicator.h"
#include "machine.h"
#include "recording.h"
#include "reply.h"
#include "slic_map.h"
#include "storage.h"

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
		.hd = a->hd,
		.machine_delay_s = a->machine_delay_s,
		.record_path = a->record_path,
		.message_path = a->message_path,
		.record_store = a->record_store,
		.message_store = a->message_store,
	};
	if (a->machine) {
		info->machine_greeting = a->machine->enabled;
		if (a->machine->enabled)
			info->machine_greeting_ms = (unsigned int)((long long)a->machine->frames[0] * 1000 / 8000);
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
 * still being written, or not ours) is never named. `with_ts` adds the epoch in the name, for the message list. */
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
			continue;		/* not a finished recording of ours: never named, never listed */
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

/* one chunk of a file of `dir`, base64 (a ubus reply is small; the page walks the offsets until "more" is 0) */
static int file_chunk(struct ubus_context *ctx, struct ubus_request_data *req, struct blob_attr *msg, const char *dir)
{
	char path[256];
	struct blob_attr *tb[RG_MAX];
	unsigned char *raw;
	char *b64;
	const char *name;
	u32 offset;
	FILE *f;
	struct stat st;
	u32 take;

	blobmsg_parse(recording_get_policy, RG_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[RG_NAME] || !tb[RG_OFFSET])
		return UBUS_STATUS_INVALID_ARGUMENT;
	name = blobmsg_get_string(tb[RG_NAME]);
	offset = blobmsg_get_u32(tb[RG_OFFSET]);
	if (!recording_name_ok(name) || !dir)
		return UBUS_STATUS_INVALID_ARGUMENT;
	recording_path_of(dir, name, path, sizeof(path));
	f = fopen(path, "r");
	if (!f)
		return UBUS_STATUS_NOT_FOUND;
	/* offset is a plain file offset: 0 carries the header, so the page assembles the exact file */
	if (fstat(fileno(f), &st) || offset > (u32)st.st_size) {
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
	recording_base64(raw, take, b64);
	fclose(f);
	blob_buf_init(&reply_buf, 0);
	blobmsg_add_string(&reply_buf, "name", name);
	blobmsg_add_u32(&reply_buf, "total", (u32)st.st_size);
	blobmsg_add_u32(&reply_buf, "offset", offset);
	blobmsg_add_string(&reply_buf, "b64", b64);
	blobmsg_add_u8(&reply_buf, "more", offset + take < (u32)st.st_size);
	free(raw);
	free(b64);
	return send_reply(ctx, req);
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

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("status", m_status),
	UBUS_METHOD_NOARG("health", m_health),
	UBUS_METHOD_NOARG("hangup", m_hangup),
	UBUS_METHOD("ring_test", m_ring_test, ring_policy),
	UBUS_METHOD_NOARG("reload", m_reload),
	UBUS_METHOD_NOARG("line_test", m_line_test),
	UBUS_METHOD_NOARG("forwarding_get", m_forwarding_get),
	UBUS_METHOD_NOARG("calls", m_calls),
	UBUS_METHOD("calls_delete", m_calls_delete, calls_delete_policy),
	UBUS_METHOD_NOARG("recordings", m_recordings),
	UBUS_METHOD("recording_get", m_recording_get, recording_get_policy),
	UBUS_METHOD("recording_delete", m_recording_delete, recording_delete_policy),
	UBUS_METHOD_NOARG("messages", m_messages),
	UBUS_METHOD("message_get", m_message_get, recording_get_policy),
	UBUS_METHOD("message_delete", m_message_delete, recording_delete_policy),
};

static struct ubus_object_type object_type = UBUS_OBJECT_TYPE("awvoice", methods);
static struct ubus_object object = { .name = "awvoice", .type = &object_type, .methods = methods, .n_methods = ARRAY_SIZE(methods) };

int api_init(struct ubus_context *ubus, struct api_ctx *a)
{
	int err;

	the_api = a;
	a->ubus = ubus;
	err = ubus_add_object(ubus, &object);
	a->connected = err == 0;
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

unsigned long long api_epoch_s(unsigned long long mono_ms_value, unsigned long long now_mono_ms)
{
	unsigned long long now_epoch = (unsigned long long)time(NULL);
	unsigned long long ago = now_mono_ms > mono_ms_value ? (now_mono_ms - mono_ms_value) / 1000 : 0;

	return now_epoch > ago ? now_epoch - ago : 0;
}
