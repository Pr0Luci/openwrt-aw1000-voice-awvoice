/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
/*
 * The qmodem_voip replies, events and method calls as JSON text, through the real rules object, the real binding
 * table and a fake core: make -f Makefile.host test
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "front_end.h"
#include "phone.h"
#include "qmsk_server.h"
#include "reply.h"
#include "call_arbiter.h"

/* ---- a JSON text sink ---- */

struct jtext {
	char text[4096];
	size_t len;
	int first[8];
	int array[8];
	int depth;
};

static void put(struct jtext *j, const char *s)
{
	size_t n = strlen(s);

	assert(j->len + n < sizeof(j->text));
	memcpy(j->text + j->len, s, n + 1);
	j->len += n;
}

static void member(struct jtext *j, const char *name)
{
	if (!j->first[j->depth])
		put(j, ",");
	j->first[j->depth] = 0;
	if (name && !j->array[j->depth]) {
		put(j, "\"");
		put(j, name);
		put(j, "\":");
	}
}

static void j_open(void *ctx, const char *name, int array)
{
	struct jtext *j = ctx;

	member(j, name);
	put(j, array ? "[" : "{");
	j->depth++;
	j->first[j->depth] = 1;
	j->array[j->depth] = array;
}

static void j_close(void *ctx)
{
	struct jtext *j = ctx;
	int array = j->array[j->depth];

	j->depth--;
	put(j, array ? "]" : "}");
}

static void j_text(void *ctx, const char *name, const char *value)
{
	member(ctx, name);
	put(ctx, "\"");
	put(ctx, value);
	put(ctx, "\"");
}

static void j_number(void *ctx, const char *name, long long value)
{
	char n[32];

	member(ctx, name);
	snprintf(n, sizeof(n), "%lld", value);
	put(ctx, n);
}

static void j_flag(void *ctx, const char *name, int value)
{
	member(ctx, name);
	put(ctx, value ? "true" : "false");
}

static void j_null(void *ctx, const char *name)
{
	member(ctx, name);
	put(ctx, "null");
}

/* a whole object around what the builder writes */
static void begin(struct jtext *j, struct reply_sink *sink)
{
	memset(j, 0, sizeof(*j));
	*sink = (struct reply_sink){ .ctx = j, .open = j_open, .close = j_close, .text = j_text, .number = j_number, .flag = j_flag, .null = j_null };
	put(j, "{");
	j->first[0] = 1;
}

static const char *end(struct jtext *j)
{
	assert(j->depth == 0);
	put(j, "}");
	return j->text;
}

/* ---- the stage machine's fake ---- */

struct fake_core {
	unsigned int answer_calls, dial_calls, hangup_calls, dtmf_calls, flash_calls, takeover_calls;
	int answer_err, dial_err, hangup_err, dtmf_err, flash_err, takeover_err;
	int last_via;			/* the door the answer or the take-over last came in by */
	char number[PHONE_NUMBER_MAX + 1];
	char digit;
	struct phone_status st;
	u64 now;
};

static struct fake_core fc;
static struct fe *fe;
static struct qmsk_bindings bindings;
static struct ctl_ops core;
static struct call_arbiter arb;
static struct jtext J;
static struct reply_sink S;

static int fake_answer(void *ctx, int via_sip)
{
	struct fake_core *c = ctx;

	c->answer_calls++;
	c->last_via = via_sip;
	return c->answer_err;
}

static int fake_takeover(void *ctx, int via_sip)
{
	struct fake_core *c = ctx;

	c->takeover_calls++;
	c->last_via = via_sip;
	return c->takeover_err;
}

static int fake_dial(void *ctx, const char *number, int via_sip)
{
	struct fake_core *c = ctx;

	c->dial_calls++;
	c->last_via = via_sip;
	snprintf(c->number, sizeof(c->number), "%s", number);
	return c->dial_err;
}

static int fake_hangup(void *ctx)
{
	struct fake_core *c = ctx;

	c->hangup_calls++;
	return c->hangup_err;
}

static int fake_dtmf(void *ctx, char digit)
{
	struct fake_core *c = ctx;

	c->dtmf_calls++;
	c->digit = digit;
	return c->dtmf_err;
}

static int fake_flash(void *ctx)
{
	struct fake_core *c = ctx;

	c->flash_calls++;
	return c->flash_err;
}

static void fake_status(void *ctx, struct phone_status *out)
{
	struct fake_core *c = ctx;

	*out = c->st;
}

static u64 fake_now(void *ctx)
{
	struct fake_core *c = ctx;

	return c->now;
}

static void setup(int line_off)
{
	assert(!fe);
	memset(&fc, 0, sizeof(fc));
	assert(fe_open(&fe, NULL, line_off) == 0);
	qmsk_bindings_init(&bindings);
	core = (struct ctl_ops){ .ctx = &fc, .answer = fake_answer, .takeover = fake_takeover, .dial = fake_dial, .hangup = fake_hangup,
		.dtmf = fake_dtmf, .flash = fake_flash, .status = fake_status, .history = NULL, .now_ms = fake_now };
	arbiter_init(&arb, fe, &bindings, &core);
	arb.restart_epoch = 7;
}

static void teardown(void)
{
	fe_close(fe);
	fe = NULL;
}

static const char *ask_log(enum sip_method m, const struct sip_args *a, int *err, FILE *log)
{
	begin(&J, &S);
	*err = arbiter_request(&arb, m, a, &S, log);
	return end(&J);
}

static const char *ask(enum sip_method m, const struct sip_args *a, int *err)
{
	return ask_log(m, a, err, NULL);
}

static const char *status_text(void)
{
	begin(&J, &S);
	arbiter_status(&arb, &S);
	return end(&J);
}

static const char *event_text(const struct fe_event *ev)
{
	begin(&J, &S);
	arbiter_event(&arb, ev, &S);
	return end(&J);
}

static const char *const status_keys[] = {
	"state", "origin", "endpoint", "number_present", "caller_id_withheld", "remote_number",
	"call_duration_seconds", "enabled", "revision", "restart_epoch", "sequence", "drop_count",
	"reconcile_pending", "answer_owner", "media_engine", "media_capture_frames", "media_playback_frames",
};

/* ---- the method names ---- */

static void test_method_of(void)
{
	assert(sip_method_of("status") == SIP_M_STATUS);
	assert(sip_method_of("capabilities") == SIP_M_CAPABILITIES);
	assert(sip_method_of("originate") == SIP_M_ORIGINATE);
	assert(sip_method_of("answer") == SIP_M_ANSWER);
	assert(sip_method_of("reject") == SIP_M_REJECT);
	assert(sip_method_of("hangup") == SIP_M_HANGUP);
	assert(sip_method_of("send_dtmf") == SIP_M_SEND_DTMF);
	assert(sip_method_of("issue_socket_session") == SIP_M_ISSUE_SOCKET_SESSION);
	assert(sip_method_of("ring_test") == SIP_M_NONE);
	assert(sip_method_of("Status") == SIP_M_NONE);
	assert(sip_method_of("") == SIP_M_NONE);
	assert(sip_method_of(NULL) == SIP_M_NONE);
}

/* ---- status ---- */

static void test_status_idle(void)
{
	const char *t;
	char want[64];
	unsigned int k;

	setup(0);
	fe_observe(fe, PHONE_LINE_IDLE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE);
	t = status_text();
	assert(!strncmp(t, "{\"status\":\"ok\",", 15));
	for (k = 0; k < sizeof(status_keys) / sizeof(status_keys[0]); k++) {
		snprintf(want, sizeof(want), "\"%s\":", status_keys[k]);
		assert(strstr(t, want));
	}
	assert(strstr(t, "\"state\":\"idle\""));
	assert(strstr(t, "\"origin\":\"cellular\""));
	assert(strstr(t, "\"endpoint\":\"cellular\""));
	assert(strstr(t, "\"number_present\":false"));
	assert(strstr(t, "\"caller_id_withheld\":false"));
	assert(strstr(t, "\"remote_number\":\"\""));
	assert(strstr(t, "\"call_duration_seconds\":0"));
	assert(strstr(t, "\"enabled\":true"));
	assert(strstr(t, "\"revision\":1"));
	assert(strstr(t, "\"restart_epoch\":7"));
	assert(strstr(t, "\"sequence\":0"));
	assert(strstr(t, "\"drop_count\":0"));
	assert(strstr(t, "\"reconcile_pending\":false"));
	assert(strstr(t, "\"answer_owner\":\"none\""));
	assert(strstr(t, "\"media_engine\":\"idle\""));
	assert(strstr(t, "\"media_capture_frames\":0"));
	assert(strstr(t, "\"media_playback_frames\":0"));

	begin(&J, &S);
	assert(arbiter_request(&arb, SIP_M_CAPABILITIES, NULL, &S, NULL) == 0);
	assert(!strcmp(end(&J),
		"{\"status\":\"ok\",\"media\":\"qmsk\",\"sample_rate\":8000,\"frame_ms\":20,\"frame_samples\":160,"
		"\"wideband\":false,\"calls\":1,\"endpoints\":[\"lan_sip\",\"cellular\",\"browser\"]}"));
	teardown();
}

static void test_status_ringing(void)
{
	const char *t;

	setup(0);
	snprintf(fc.st.number, sizeof(fc.st.number), "%s", "5551234");
	fc.st.call_id = 9;
	fe_observe(fe, PHONE_LINE_RINGING, CALL_INCOMING, PHONE_DIR_IN, 9, FE_ANSWER_NONE, 1, 0, FE_REASON_NONE);
	t = status_text();
	assert(strstr(t, "\"state\":\"incoming_ringing\""));
	assert(strstr(t, "\"endpoint\":\"cellular\""));
	assert(strstr(t, "\"caller_id_withheld\":true"));
	assert(strstr(t, "\"remote_number\":\"\""));
	assert(strstr(t, "\"number_present\":true"));
	assert(strstr(t, "\"answer_owner\":\"none\""));
	/* a number the network passed on: in this reply, in full */
	fe_observe(fe, PHONE_LINE_RINGING, CALL_INCOMING, PHONE_DIR_IN, 9, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE);
	t = status_text();
	assert(strstr(t, "\"caller_id_withheld\":false"));
	assert(strstr(t, "\"remote_number\":\"5551234\""));
	assert(strstr(t, "\"number_present\":true"));
	/* a call the network gave no number for */
	fc.st.number[0] = 0;
	t = status_text();
	assert(strstr(t, "\"remote_number\":\"\""));
	assert(strstr(t, "\"number_present\":false"));
	teardown();
}

static void test_status_active_media(void)
{
	const char *t;

	setup(0);
	fe_observe(fe, PHONE_LINE_RINGING, CALL_INCOMING, PHONE_DIR_IN, 12, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE);
	fe_observe(fe, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_IN, 12, FE_ANSWER_WEB, 0, 4, FE_REASON_NONE);
	t = status_text();
	assert(strstr(t, "\"state\":\"active\""));
	assert(strstr(t, "\"endpoint\":\"browser\""));	/* the page's own call */
	assert(strstr(t, "\"answer_owner\":\"web\""));
	assert(strstr(t, "\"call_duration_seconds\":4"));
	assert(strstr(t, "\"held\":false") && strstr(t, "\"waiting\":false"));
	/* the legs the network lists: a held call and a waiting one are named beside the active leg */
	fc.st.leg_count = 3;
	fc.st.legs[0].stat = LEG_ACTIVE;
	fc.st.legs[1].stat = LEG_HELD;
	fc.st.legs[2].stat = LEG_WAITING;
	t = status_text();
	assert(strstr(t, "\"held\":true") && strstr(t, "\"waiting\":true"));
	fc.st.leg_count = 0;
	assert(strstr(t, "\"media_engine\":\"idle\""));
	/* one direction alone does not make the engine ready; each direction is counted for the page */
	arbiter_media(&arb, 1, 0);
	assert(arb.capture_frames == 1 && arb.playback_frames == 0);
	assert(strstr(status_text(), "\"media_engine\":\"idle\""));
	assert(strstr(status_text(), "\"media_capture_frames\":1"));
	assert(strstr(status_text(), "\"media_playback_frames\":0"));
	arbiter_media(&arb, 0, 1);
	assert(strstr(status_text(), "\"media_engine\":\"ready\""));
	assert(strstr(status_text(), "\"media_capture_frames\":1"));
	assert(strstr(status_text(), "\"media_playback_frames\":1"));
	/* a new revision starts the counters over */
	fe_observe(fe, PHONE_LINE_RELEASE, CALL_GONE, PHONE_DIR_IN, 12, FE_ANSWER_WEB, 0, 5, FE_REASON_LOCAL);
	t = status_text();
	assert(strstr(t, "\"state\":\"terminating\""));
	assert(strstr(t, "\"media_engine\":\"idle\""));
	assert(strstr(t, "\"media_capture_frames\":0"));
	assert(strstr(t, "\"media_playback_frames\":0"));
	arbiter_media(&arb, 1, 1);
	assert(strstr(status_text(), "\"media_engine\":\"ready\""));
	/* a dropped frame is counted */
	arbiter_dropped(&arb);
	arbiter_dropped(&arb);
	assert(strstr(status_text(), "\"drop_count\":2"));
	teardown();
}

/* ---- the methods ---- */

static void test_answer(void)
{
	int err;

	setup(0);
	fe_observe(fe, PHONE_LINE_RINGING, CALL_INCOMING, PHONE_DIR_IN, 5, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE);
	assert(!strcmp(ask(SIP_M_ANSWER, NULL, &err), "{\"ok\":true,\"refused\":null}"));
	assert(err == 0 && fc.answer_calls == 1 && fc.last_via == 0);	/* the page's own door */
	fc.answer_err = -EBUSY;
	assert(!strcmp(ask(SIP_M_ANSWER, NULL, &err), "{\"ok\":false,\"refused\":\"refused-busy\"}"));
	assert(err == -EBUSY && fc.answer_calls == 2);
	/* the door travels with the answer even when it is refused: the page's own, then the installed service's */
	assert(fc.last_via == 0);
	{
		struct sip_args via;

		memset(&via, 0, sizeof(via));
		via.via_sip = 1;
		(void)ask(SIP_M_ANSWER, &via, &err);
		assert(fc.last_via == 1);
	}
	/* nothing ringing: the core is not asked */
	fe_observe(fe, PHONE_LINE_IDLE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_REJECTED);
	assert(!strcmp(ask(SIP_M_ANSWER, NULL, &err), "{\"ok\":false,\"refused\":\"refused-nocall\"}"));
	assert(err == -ENOENT && fc.answer_calls == 3);	/* the door check above asked the core once more */
	teardown();
}

/*
 * The caller the answering machine holds, given to the front end that asks: the same verb a ringing call is answered
 * with, and the view's owner is what chooses the door. The line owns the refusal of a call it cannot hand over.
 */
static void test_takeover(void)
{
	int err;

	setup(0);
	fe_observe(fe, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_IN, 14, FE_ANSWER_MACHINE, 0, 6, FE_REASON_NONE);
	assert(!strcmp(ask(SIP_M_ANSWER, NULL, &err), "{\"ok\":true,\"refused\":null}"));
	assert(err == 0 && fc.takeover_calls == 1 && fc.answer_calls == 0 && fc.last_via == 0);	/* the page's own door */
	{
		struct sip_args via;

		memset(&via, 0, sizeof(via));
		via.via_sip = 1;		/* the installed service's leg: its endpoint becomes the call's */
		(void)ask(SIP_M_ANSWER, &via, &err);
		assert(err == 0 && fc.takeover_calls == 2 && fc.last_via == 1);
	}
	/* the line can refuse the hand-over: the word is its own errno's */
	fc.takeover_err = -EBUSY;
	assert(!strcmp(ask(SIP_M_ANSWER, NULL, &err), "{\"ok\":false,\"refused\":\"refused-busy\"}"));
	assert(err == -EBUSY && fc.takeover_calls == 3);
	fc.takeover_err = 0;
	/* an answer still on its way is not a call to take: the ringing view keeps its own refusal */
	fe_observe(fe, PHONE_LINE_RINGING, CALL_INCOMING, PHONE_DIR_IN, 15, FE_ANSWER_MACHINE, 0, 0, FE_REASON_NONE);
	assert(!strcmp(ask(SIP_M_ANSWER, NULL, &err), "{\"ok\":false,\"refused\":\"refused-busy\"}"));
	assert(err == -EBUSY && fc.takeover_calls == 3 && fc.answer_calls == 0);
	/* a call the handset holds, or one a front end already holds, is not the machine's to give */
	fe_observe(fe, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_IN, 16, FE_ANSWER_HANDSET, 0, 2, FE_REASON_NONE);
	assert(!strcmp(ask(SIP_M_ANSWER, NULL, &err), "{\"ok\":false,\"refused\":\"refused-nocall\"}"));
	assert(err == -ENOENT && fc.takeover_calls == 3);
	fe_observe(fe, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_IN, 17, FE_ANSWER_WEB, 0, 1, FE_REASON_NONE);
	assert(!strcmp(ask(SIP_M_ANSWER, NULL, &err), "{\"ok\":false,\"refused\":\"refused-nocall\"}"));
	assert(err == -ENOENT && fc.takeover_calls == 3);
	teardown();
}

static void test_originate(void)
{
	struct sip_args a = { 0 };
	int err;

	setup(0);
	a.endpoint = "lan_sip";
	a.number = "+94112345678";
	assert(!strcmp(ask(SIP_M_ORIGINATE, &a, &err), "{\"ok\":true,\"refused\":null}"));
	assert(err == 0 && fc.dial_calls == 1 && !strcmp(fc.number, "+94112345678"));
	assert(fc.last_via == 0);	/* the page's own door */
	a.endpoint = "cellular";
	assert(!strcmp(ask(SIP_M_ORIGINATE, &a, &err), "{\"ok\":true,\"refused\":null}"));
	assert(err == 0 && fc.dial_calls == 2);
	/* the installed service's door travels with its dial */
	a.via_sip = 1;
	a.endpoint = "lan_sip";
	assert(!strcmp(ask(SIP_M_ORIGINATE, &a, &err), "{\"ok\":true,\"refused\":null}"));
	assert(err == 0 && fc.dial_calls == 3 && fc.last_via == 1);
	a.via_sip = 0;
	/* the page's own control surface is a front-end endpoint of this build: it dials the cellular leg */
	a.endpoint = "browser";
	assert(!strcmp(ask(SIP_M_ORIGINATE, &a, &err), "{\"ok\":true,\"refused\":null}"));
	assert(err == 0 && fc.dial_calls == 4);
	/* an external SIP leg is not this build's */
	a.endpoint = "external_sip";
	assert(!strcmp(ask(SIP_M_ORIGINATE, &a, &err), "{\"ok\":false,\"refused\":\"refused-mode\"}"));
	assert(err == -EPERM && fc.dial_calls == 4);
	a.endpoint = NULL;
	assert(!strcmp(ask(SIP_M_ORIGINATE, &a, &err), "{\"ok\":false,\"refused\":\"refused-mode\"}"));
	assert(err == -EPERM && fc.dial_calls == 4);
	/* the core's refusals carry their own words */
	a.endpoint = "lan_sip";
	fc.dial_err = -EPERM;
	assert(!strcmp(ask(SIP_M_ORIGINATE, &a, &err), "{\"ok\":false,\"refused\":\"NOT_AVAILABLE\"}"));
	assert(err == -EPERM && fc.dial_calls == 5);
	fc.dial_err = -ENODEV;
	assert(!strcmp(ask(SIP_M_ORIGINATE, &a, &err), "{\"ok\":false,\"refused\":\"refused-nocall\"}"));
	assert(err == -ENODEV);
	fc.dial_err = -EBUSY;
	assert(!strcmp(ask(SIP_M_ORIGINATE, &a, &err), "{\"ok\":false,\"refused\":\"refused-busy\"}"));
	assert(err == -EBUSY);
	/* the line's front end off refuses before the core is asked */
	arbiter_set_off(&arb, 1);
	fc.dial_err = 0;
	assert(!strcmp(ask(SIP_M_ORIGINATE, &a, &err), "{\"ok\":false,\"refused\":\"refused-mode\"}"));
	assert(err == -EPERM && fc.dial_calls == 7);
	teardown();
}

static void test_reject(void)
{
	int err;

	setup(0);
	fe_observe(fe, PHONE_LINE_RINGING, CALL_INCOMING, PHONE_DIR_IN, 6, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE);
	assert(!strcmp(ask(SIP_M_REJECT, NULL, &err), "{\"ok\":true,\"refused\":null}"));
	assert(err == 0 && fc.hangup_calls == 1);
	assert(arbiter_take_reason(&arb) == FE_REASON_REJECTED);
	assert(arbiter_take_reason(&arb) == FE_REASON_NONE);
	/* nothing ringing: a reject is refused, not sent */
	fe_observe(fe, PHONE_LINE_IDLE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_REJECTED);
	assert(!strcmp(ask(SIP_M_REJECT, NULL, &err), "{\"ok\":false,\"refused\":\"refused-nocall\"}"));
	assert(err == -ENOENT && fc.hangup_calls == 1);
	teardown();
}

static void test_hangup(void)
{
	int err;

	setup(0);
	/* a hangup with nothing to end is ok: the intent is satisfied */
	fc.hangup_err = -ENOENT;
	assert(!strcmp(ask(SIP_M_HANGUP, NULL, &err), "{\"ok\":true,\"refused\":null}"));
	assert(err == 0 && fc.hangup_calls == 1);
	assert(arbiter_take_reason(&arb) == FE_REASON_NONE);
	/* a call that is there: the core ends it, and a local hangup is the reason it ended */
	fe_observe(fe, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_OUT, 8, FE_ANSWER_WEB, 0, 3, FE_REASON_NONE);
	fc.hangup_err = 0;
	assert(!strcmp(ask(SIP_M_HANGUP, NULL, &err), "{\"ok\":true,\"refused\":null}"));
	assert(err == 0 && fc.hangup_calls == 2);
	assert(arbiter_take_reason(&arb) == FE_REASON_LOCAL);
	assert(arbiter_take_reason(&arb) == FE_REASON_NONE);
	fc.hangup_err = -ENODEV;
	assert(!strcmp(ask(SIP_M_HANGUP, NULL, &err), "{\"ok\":false,\"refused\":\"refused-nocall\"}"));
	assert(err == -ENODEV);
	teardown();
}

static void test_send_dtmf(void)
{
	struct sip_args a = { .digit = '5' };
	int err;

	setup(0);
	assert(!strcmp(ask(SIP_M_SEND_DTMF, &a, &err), "{\"ok\":false,\"refused\":\"refused-nocall\"}"));
	assert(err == -ENOENT && fc.dtmf_calls == 0);
	fe_observe(fe, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_IN, 4, FE_ANSWER_HANDSET, 0, 2, FE_REASON_NONE);
	assert(!strcmp(ask(SIP_M_SEND_DTMF, &a, &err), "{\"ok\":true,\"refused\":null}"));
	assert(err == 0 && fc.dtmf_calls == 1 && fc.digit == '5');
	/* the call ended: no digit goes anywhere */
	fe_observe(fe, PHONE_LINE_IDLE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_HANDSET);
	assert(!strcmp(ask(SIP_M_SEND_DTMF, &a, &err), "{\"ok\":false,\"refused\":\"refused-nocall\"}"));
	assert(fc.dtmf_calls == 1);
	teardown();
}

static void session_id_of(const char *json, char out[QMSK_ID_MAX + 1])
{
	const char *p = strstr(json, "\"session_id\":\"");

	assert(p != NULL);
	p += strlen("\"session_id\":\"");
	assert(strlen(p) > 16 && p[16] == '"');
	memcpy(out, p, 16);
	out[16] = 0;
}

static void test_flash(void)
{
	int err;

	setup(0);
	/* nothing a flash can do: the core decides and its word travels */
	fc.flash_err = -ENOENT;
	assert(!strcmp(ask(SIP_M_FLASH, NULL, &err), "{\"ok\":false,\"refused\":\"refused-nocall\"}"));
	assert(err == -ENOENT && fc.flash_calls == 1);
	/* the call view is no gate: a flash reaches the core even with the view idle, as it does for a held call */
	fc.flash_err = 0;
	assert(!strcmp(ask(SIP_M_FLASH, NULL, &err), "{\"ok\":true,\"refused\":null}"));
	assert(err == 0 && fc.flash_calls == 2);
	fe_observe(fe, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_IN, 9, FE_ANSWER_WEB, 0, 3, FE_REASON_NONE);
	assert(!strcmp(ask(SIP_M_FLASH, NULL, &err), "{\"ok\":true,\"refused\":null}"));
	assert(err == 0 && fc.flash_calls == 3);
	/* the line's front end off refuses before the core is asked */
	arbiter_set_off(&arb, 1);
	fc.flash_err = 0;
	assert(!strcmp(ask(SIP_M_FLASH, NULL, &err), "{\"ok\":false,\"refused\":\"refused-mode\"}"));
	assert(err == -EPERM && fc.flash_calls == 3);
	teardown();
}

static void test_socket_session(void)
{
	struct sip_args a = { 0 };
	struct qmsk_binding *b;
	char id[QMSK_ID_MAX + 1];
	const char *t;
	unsigned int k;
	int err;

	setup(0);
	fe_observe(fe, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_IN, 21, FE_ANSWER_SIP, 0, 1, FE_REASON_NONE);
	a.call_revision = fe_call_view(fe)->revision;
	fc.now = 1000;
	arbiter_set_off(&arb, 1);
	assert(!strcmp(ask(SIP_M_ISSUE_SOCKET_SESSION, &a, &err), "{\"ok\":false,\"refused\":\"refused-mode\"}"));
	assert(err == -EPERM);
	arbiter_set_off(&arb, 0);
	a.call_revision++;
	assert(!strcmp(ask(SIP_M_ISSUE_SOCKET_SESSION, &a, &err), "{\"ok\":false,\"refused\":\"refused-revision\"}"));
	assert(err == -ESTALE);
	a.call_revision--;
	/* a call the page owns has no audio for this door */
	fe_observe(fe, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_IN, 21, FE_ANSWER_WEB, 0, 1, FE_REASON_NONE);
	a.call_revision = fe_call_view(fe)->revision;
	assert(!strcmp(ask(SIP_M_ISSUE_SOCKET_SESSION, &a, &err), "{\"ok\":false,\"refused\":\"refused-busy\"}"));
	assert(err == -EBUSY);
	fe_observe(fe, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_IN, 21, FE_ANSWER_SIP, 0, 1, FE_REASON_NONE);
	a.call_revision = fe_call_view(fe)->revision;
	t = ask(SIP_M_ISSUE_SOCKET_SESSION, &a, &err);
	assert(err == 0 && strstr(t, "\"ok\":true") && strstr(t, "\"refused\":null"));
	session_id_of(t, id);
	assert(strlen(id) == 16);
	for (k = 0; k < 16; k++)
		assert((id[k] >= '0' && id[k] <= '9') || (id[k] >= 'a' && id[k] <= 'f'));
	b = qmsk_binding_find(&bindings, id);
	assert(b && b->revision == a.call_revision);
	/* eight live bindings: the next mint is refused-busy */
	for (k = 1; k < QMSK_SLOTS; k++) {
		t = ask(SIP_M_ISSUE_SOCKET_SESSION, &a, &err);
		assert(err == 0 && strstr(t, "\"ok\":true"));
	}
	t = ask(SIP_M_ISSUE_SOCKET_SESSION, &a, &err);
	assert(err == -EBUSY && !strcmp(t, "{\"ok\":false,\"refused\":\"refused-busy\"}"));
	/* the call ended: the idle revision is current but there is no call */
	fe_observe(fe, PHONE_LINE_IDLE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_LOCAL);
	a.call_revision = fe_call_view(fe)->revision;
	assert(!strcmp(ask(SIP_M_ISSUE_SOCKET_SESSION, &a, &err), "{\"ok\":false,\"refused\":\"refused-nocall\"}"));
	assert(err == -ENOENT);
	a.call_revision--;
	assert(!strcmp(ask(SIP_M_ISSUE_SOCKET_SESSION, &a, &err), "{\"ok\":false,\"refused\":\"refused-revision\"}"));
	teardown();
}

/* ---- the events ---- */

static void test_events(void)
{
	struct fe_event ev;
	const char *t;

	setup(0);
	memset(&ev, 0, sizeof(ev));
	ev.event = FE_EV_RING;
	ev.id = 3;
	ev.dir = PHONE_DIR_IN;
	ev.phase = PHONE_LINE_RINGING;
	ev.withheld = 1;
	snprintf(fc.st.number, sizeof(fc.st.number), "%s", "5551234");
	fe_observe(fe, PHONE_LINE_RINGING, CALL_INCOMING, PHONE_DIR_IN, 3, FE_ANSWER_NONE, 1, 0, FE_REASON_NONE);
	t = event_text(&ev);
	assert(strstr(t, "\"event\":\"ring\""));
	assert(strstr(t, "\"state\":\"incoming_ringing\""));
	assert(strstr(t, "\"revision\":"));
	assert(strstr(t, "\"remote_number\":\"\""));
	assert(!strstr(t, "5551234"));		/* an event never carries the number */
	assert(arb.sequence == 1);

	ev.event = FE_EV_ACTIVE;
	ev.withheld = 0;
	fe_observe(fe, PHONE_LINE_CALL, CALL_ACTIVE, PHONE_DIR_IN, 3, FE_ANSWER_WEB, 0, 2, FE_REASON_NONE);
	t = event_text(&ev);
	assert(strstr(t, "\"event\":\"call_state\""));
	assert(strstr(t, "\"state\":\"active\""));
	assert(strstr(t, "\"revision\":"));
	assert(arb.sequence == 2);

	ev.event = FE_EV_RELEASE;
	ev.reason = FE_REASON_REJECTED;
	fe_observe(fe, PHONE_LINE_IDLE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 9, FE_REASON_REJECTED);
	t = event_text(&ev);
	assert(strstr(t, "\"event\":\"release\""));
	assert(strstr(t, "\"state\":\"idle\""));
	assert(strstr(t, "\"call_duration_seconds\":9"));
	assert(strstr(t, "\"revision\":"));
	assert(!strstr(t, "5551234"));
	assert(arb.sequence == 3);

	ev.event = FE_EV_FAULT;
	ev.id = 0;
	fe_observe(fe, PHONE_LINE_NO_SERVICE, CALL_NONE, PHONE_DIR_NONE, 0, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE);
	t = event_text(&ev);
	assert(strstr(t, "\"event\":\"call_state\""));
	assert(strstr(t, "\"state\":\"fault\""));
	assert(strstr(t, "\"revision\":"));
	assert(arb.sequence == 4);

	/* a digit is not published on this topic and does not climb the sequence */
	ev.event = FE_EV_DTMF;
	ev.digit = '7';
	t = event_text(&ev);
	assert(!strstr(t, "\"event\"") && arb.sequence == 4);
	teardown();
}

/* ---- the line's front end off ---- */

static void test_line_off(void)
{
	const char *t;
	int err;

	setup(0);
	arbiter_set_off(&arb, 1);
	fe_set_off(fe, 1);
	t = status_text();
	assert(strstr(t, "\"enabled\":false"));
	assert(strstr(t, "\"state\":\"disabled\""));
	/* the state is the rules object's, whatever the arbiter's own flag says */
	fe_set_off(fe, 0);
	fe_observe(fe, PHONE_LINE_RINGING, CALL_INCOMING, PHONE_DIR_IN, 2, FE_ANSWER_NONE, 0, 0, FE_REASON_NONE);
	t = status_text();
	assert(strstr(t, "\"enabled\":false"));
	assert(strstr(t, "\"state\":\"incoming_ringing\""));
	assert(!strcmp(ask(SIP_M_ANSWER, NULL, &err), "{\"ok\":false,\"refused\":\"refused-mode\"}"));
	assert(err == -EPERM && fc.answer_calls == 0);
	/* every action is refused while the line's front end is off, so the handset's call is not touched */
	assert(!strcmp(ask(SIP_M_REJECT, NULL, &err), "{\"ok\":false,\"refused\":\"refused-mode\"}"));
	assert(err == -EPERM && fc.hangup_calls == 0);
	assert(!strcmp(ask(SIP_M_HANGUP, NULL, &err), "{\"ok\":false,\"refused\":\"refused-mode\"}"));
	assert(err == -EPERM && fc.hangup_calls == 0);
	{
		struct sip_args a = { .digit = '1' };

		assert(!strcmp(ask(SIP_M_SEND_DTMF, &a, &err), "{\"ok\":false,\"refused\":\"refused-mode\"}"));
		assert(err == -EPERM && fc.dtmf_calls == 0);
	}
	teardown();
}

/* ---- the refusal line ---- */

static unsigned int log_lines(FILE *log)
{
	char line[128];
	unsigned int n = 0;

	fflush(log);
	rewind(log);
	while (fgets(line, sizeof(line), log))
		n++;
	fseek(log, 0, SEEK_END);
	return n;
}

static void test_refusal_log(void)
{
	FILE *log = tmpfile();
	char all[512];
	size_t n;
	int err;

	assert(log != NULL);
	setup(0);
	snprintf(fc.st.number, sizeof(fc.st.number), "%s", "5551234");
	fc.now = 5000;
	ask_log(SIP_M_ANSWER, NULL, &err, log);
	ask_log(SIP_M_ANSWER, NULL, &err, log);
	assert(log_lines(log) == 1);		/* the same word inside the second is written once */
	fc.now = 5999;
	ask_log(SIP_M_ANSWER, NULL, &err, log);
	assert(log_lines(log) == 1);
	fc.now = 6000;
	ask_log(SIP_M_ANSWER, NULL, &err, log);
	assert(log_lines(log) == 2);
	fflush(log);
	rewind(log);
	n = fread(all, 1, sizeof(all) - 1, log);
	all[n] = 0;
	assert(strstr(all, "answer") && strstr(all, "refused-nocall") && strstr(all, "call 0"));
	assert(!strstr(all, "5551234"));	/* a number never reaches the log */
	teardown();
	fclose(log);
}

int main(void)
{
	test_method_of();
	test_status_idle();
	test_status_ringing();
	test_status_active_media();
	test_answer();
	test_takeover();
	test_originate();
	test_reject();
	test_hangup();
	test_send_dtmf();
	test_flash();
	test_socket_session();
	test_events();
	test_line_off();
	test_refusal_log();
	printf("arbiter: all tests passed\n");
	return 0;
}
