// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * The shapes of the ubus replies and events, checked as JSON text through a text sink (the daemon's own sink is
 * blobmsg, which the host does not have): make -f Makefile.host test
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fakes.h"
#include "phone.h"
#include "phonebook.h"
#include "reply.h"
#include "slic_map.h"

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

static const struct reply_info running = { .line_in_service = 1, .my_number = "", .channel = 1, .at_interface = 2, .port = "/dev/ttyUSB2",
	.module_loaded = 1 };

static struct phone_status blank(void)
{
	struct phone_status st;

	memset(&st, 0, sizeof(st));
	st.sim_ready = st.registered = st.ims_registered = st.audio_slave = st.clir = st.clir_status = st.call_waiting_net = PHONE_UNKNOWN;
	st.signal_dbm = PHONE_DBM_UNKNOWN;
	return st;
}

static const char *status_json(struct jtext *j, const struct phone_status *st, const struct reply_info *info, unsigned long long now)
{
	struct reply_sink sink;

	begin(j, &sink);
	reply_status(&sink, st, info, now);
	return end(j);
}

static void test_state_names(void)
{
	assert(!strcmp(reply_state_name(PHONE_LINE_IDLE), "idle"));
	assert(!strcmp(reply_state_name(PHONE_LINE_RINGING), "ring"));
	assert(!strcmp(reply_state_name(PHONE_LINE_DIAL_TONE), "offhook"));
	assert(!strcmp(reply_state_name(PHONE_LINE_DIALLING), "offhook"));
	assert(!strcmp(reply_state_name(PHONE_LINE_SETUP), "call"));
	assert(!strcmp(reply_state_name(PHONE_LINE_CALL), "call"));
	assert(!strcmp(reply_state_name(PHONE_LINE_RELEASE), "call"));
	assert(!strcmp(reply_state_name(PHONE_LINE_NO_SERVICE), "noservice"));

	/* the line word is the core's own name, for every value */
	assert(!strcmp(phone_line_name(PHONE_LINE_DIAL_TONE), "dial_tone"));
	assert(!strcmp(phone_line_name(PHONE_LINE_NO_SERVICE), "no_service"));
}

static void test_phases(void)
{
	assert(!strcmp(reply_phase_name(CALL_INCOMING, PHONE_DIR_IN, 0), "ringing"));
	assert(!strcmp(reply_phase_name(CALL_DIALING, PHONE_DIR_OUT, 0), "dialing"));
	assert(!strcmp(reply_phase_name(CALL_ALERTING, PHONE_DIR_OUT, 0), "alerting"));
	assert(!strcmp(reply_phase_name(CALL_ACTIVE, PHONE_DIR_OUT, 1), "active"));
	/* the modem has not listed the call yet: the direction says what it is */
	assert(!strcmp(reply_phase_name(CALL_NONE, PHONE_DIR_OUT, 0), "dialing"));
	assert(!strcmp(reply_phase_name(CALL_NONE, PHONE_DIR_IN, 0), "ringing"));
	assert(!strcmp(reply_phase_name(CALL_GONE, PHONE_DIR_OUT, 1), "active"));
}

/* the status.state word for every line state, through the whole reply */
static void test_status_states(void)
{
	static const struct { enum phone_line line; const char *state, *line_name; } want[] = {
		{ PHONE_LINE_IDLE, "idle", "idle" }, { PHONE_LINE_RINGING, "ring", "ringing" }, { PHONE_LINE_DIAL_TONE, "offhook", "dial_tone" },
		{ PHONE_LINE_DIALLING, "offhook", "dialling" }, { PHONE_LINE_SETUP, "call", "setup" }, { PHONE_LINE_CALL, "call", "call" },
		{ PHONE_LINE_RELEASE, "call", "release" }, { PHONE_LINE_NO_SERVICE, "noservice", "no_service" },
	};
	struct jtext j;
	unsigned int k;
	char expect[64];

	for (k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
		struct phone_status st = blank();

		st.line = want[k].line;
		status_json(&j, &st, &running, 5000);
		snprintf(expect, sizeof(expect), "\"state\":\"%s\",\"line\":\"%s\"", want[k].state, want[k].line_name);
		assert(strstr(j.text, expect));
	}
}

static void test_status_idle(void)
{
	struct jtext j;
	struct phone_status st = blank();

	assert(!strcmp(status_json(&j, &st, &running, 5000),
		"{\"state\":\"idle\",\"line\":\"idle\",\"hook\":\"on\",\"call\":null,"
		"\"voiceNetwork\":{\"registered\":null,\"ims\":null},\"signal\":{\"rsrpDbm\":null,\"network\":null},"
		"\"service\":{\"running\":true},\"myNumber\":\"\"}"));
	st.off_hook = 1;
	assert(strstr(status_json(&j, &st, &running, 5000), "\"hook\":\"off\""));
}

static void test_status_down(void)
{
	struct jtext j;
	struct phone_status st = blank();
	struct reply_info info = running;

	info.line_in_service = 0;
	assert(!strcmp(status_json(&j, &st, &info, 5000),
		"{\"state\":\"down\",\"line\":\"down\",\"hook\":null,\"call\":null,"
		"\"voiceNetwork\":{\"registered\":null,\"ims\":null},\"signal\":{\"rsrpDbm\":null,\"network\":null},"
		"\"service\":{\"running\":true},\"myNumber\":\"\"}"));
}

static void test_status_call_and_elapsed(void)
{
	struct jtext j;
	struct phone_status st = blank();

	st.line = PHONE_LINE_SETUP;
	st.off_hook = 1;
	st.call_id = 3;
	st.dir = PHONE_DIR_OUT;
	st.call = CALL_ALERTING;
	snprintf(st.number, sizeof(st.number), "%s", "+1234");
	st.call_since_ms = 1000;
	/* before the answer: 0 seconds, whatever the clock says */
	assert(strstr(status_json(&j, &st, &running, 9000),
		"\"call\":{\"id\":3,\"direction\":\"OUT\",\"number\":\"+1234\",\"phase\":\"alerting\",\"elapsedSec\":0}"));
	/* after the answer: whole seconds since it */
	st.line = PHONE_LINE_CALL;
	st.call = CALL_ACTIVE;
	st.answered_ms = 4000;
	assert(strstr(status_json(&j, &st, &running, 4999), "\"phase\":\"active\",\"elapsedSec\":0}"));
	assert(strstr(status_json(&j, &st, &running, 5000), "\"phase\":\"active\",\"elapsedSec\":1}"));
	assert(strstr(status_json(&j, &st, &running, 64500), "\"elapsedSec\":60}"));
	/* a clock that is behind the answer (never expected) gives 0, not a huge number */
	assert(strstr(status_json(&j, &st, &running, 3000), "\"elapsedSec\":0}"));
	/* an incoming call before the answer; a withheld number is the empty string */
	st = blank();
	st.line = PHONE_LINE_RINGING;
	st.call_id = 4;
	st.dir = PHONE_DIR_IN;
	st.call = CALL_INCOMING;
	assert(strstr(status_json(&j, &st, &running, 9000), "\"call\":{\"id\":4,\"direction\":\"IN\",\"number\":\"\",\"phase\":\"ringing\",\"elapsedSec\":0}"));
}

static void test_status_modem_fields(void)
{
	struct jtext j;
	struct phone_status st = blank();
	struct reply_info info = running;

	info.my_number = "*88";
	st.registered = 1;
	st.ims_registered = 0;
	st.signal_dbm = -95;
	snprintf(st.network, sizeof(st.network), "%s", "4G");
	assert(strstr(status_json(&j, &st, &info, 5000), "\"voiceNetwork\":{\"registered\":true,\"ims\":false},\"signal\":{\"rsrpDbm\":-95,\"network\":\"4G\"}"));
	assert(strstr(j.text, "\"myNumber\":\"*88\""));
	info.my_number = NULL;
	assert(strstr(status_json(&j, &st, &info, 5000), "\"myNumber\":\"\""));
}

static void test_health(void)
{
	struct jtext j;
	struct reply_sink sink;
	struct phone_status st = blank();
	struct reply_info info = running;

	st.modem_up = 1;
	st.modem_reply_ms = 3000;
	st.battery_mv = 59111;
	st.loop_ua = -903;
	st.modem_timeouts = 2;
	st.hook_gaps = 1;
	st.hook_gap_max_ms = 31;
	st.faults = 4;
	begin(&j, &sink);
	reply_health(&sink, &st, &info, 10500);
	assert(!strcmp(end(&j),
		"{\"atPort\":{\"port\":\"/dev/ttyUSB2\",\"usbInterface\":2,\"responding\":true,\"lastReplyAgoSec\":7},"
		"\"line\":{\"channel\":1,\"hook\":\"on\",\"state\":\"idle\",\"calibrated\":true,\"batteryMv\":59111,\"loopUa\":-903},"
		"\"modem\":{\"simReady\":null,\"registered\":null,\"imsRegistered\":null,\"rsrpDbm\":null,\"network\":null,\"audioSlave\":null,\"hdVoice\":\"off\",\"callWaiting\":null,\"clir\":null,\"clirStatus\":null},"
		"\"system\":{\"moduleLoaded\":true,\"bridge\":{\"running\":null,\"frames\":null,\"resyncs\":null,\"txSkipped\":null,\"maxGapUs\":null}},"
		"\"indicator\":{\"led\":null,\"state\":\"off\",\"leds\":null},"
		"\"counters\":{\"modemTimeouts\":2,\"hookGapsOver20ms\":1,\"hookGapMaxMs\":31,\"faults\":4}}"));

	/* a modem that never answered: no reply time; the bridge counters when the kernel gave them */
	st.modem_up = 0;
	st.modem_reply_ms = 0;
	info.port = "";
	st.audio_slave = 1;
	info.bridge_ok = 1;
	info.bridge_frames = 5000000000ull;
	info.bridge_running = 1;
	info.bridge_resyncs = 3;
	info.bridge_tx_skipped = 2;
	info.bridge_max_gap_us = 1400;
	begin(&j, &sink);
	reply_health(&sink, &st, &info, 10500);
	end(&j);
	assert(strstr(j.text, "\"atPort\":{\"port\":\"\",\"usbInterface\":2,\"responding\":false,\"lastReplyAgoSec\":null}"));
	assert(strstr(j.text, "\"audioSlave\":true"));
	assert(strstr(j.text, "\"bridge\":{\"running\":true,\"frames\":5000000000,\"resyncs\":3,\"txSkipped\":2,\"maxGapUs\":1400}"));

	/* the line out of service */
	info.line_in_service = 0;
	begin(&j, &sink);
	reply_health(&sink, &st, &info, 10500);
	end(&j);
	assert(strstr(j.text, "\"state\":\"down\",\"calibrated\":false"));
}

static void test_events(void)
{
	struct jtext j;
	struct reply_sink sink;
	struct phone_event ev = { .kind = PHONE_EV_CALL, .line = PHONE_LINE_CALL, .call_id = 7, .dir = PHONE_DIR_IN, .call = CALL_ACTIVE, .at_ms = 1 };

	begin(&j, &sink);
	reply_event_line(&sink, PHONE_LINE_DIAL_TONE);
	assert(!strcmp(end(&j), "{\"state\":\"offhook\",\"line\":\"dial_tone\"}"));

	begin(&j, &sink);
	reply_event_call(&sink, &ev, 0);
	assert(!strcmp(end(&j), "{\"id\":7,\"direction\":\"IN\",\"phase\":\"active\"}"));
	ev.call = CALL_INCOMING;
	begin(&j, &sink);
	reply_event_call(&sink, &ev, 0);
	assert(strstr(end(&j), "\"phase\":\"ringing\""));
	ev.call = CALL_GONE;
	begin(&j, &sink);
	reply_event_call(&sink, &ev, 0);
	assert(strstr(end(&j), "\"phase\":\"ended\""));
	ev.call = CALL_NONE;
	ev.dir = PHONE_DIR_OUT;
	begin(&j, &sink);
	reply_event_call(&sink, &ev, 0);
	assert(!strcmp(end(&j), "{\"id\":7,\"direction\":\"OUT\",\"phase\":\"ended\"}"));
}

static const char *status_json_health(struct jtext *j, const struct phone_status *st)
{
	struct reply_sink sink;

	begin(j, &sink);
	reply_health(&sink, st, &running, 5000);
	return end(j);
}

static void test_calls(void)
{
	struct jtext j;
	struct reply_sink sink;
	struct calllog *l = calloc(1, sizeof(*l));
	struct phone_call_record a = { .id = 1, .dir = PHONE_DIR_IN, .number = "5551", .answered_ms = 1000, .end_ms = 66000, .result = PHONE_RES_ANSWERED };
	struct phone_call_record o = { .id = 2, .dir = PHONE_DIR_OUT, .number = "45", .end_ms = 9000, .cause = 17, .result = PHONE_RES_BUSY };

	struct phonebook pb;
	static const char *const nums[] = { "5551" };

	assert(l);
	calllog_init(l, NULL, NULL, 0);
	begin(&j, &sink);
	reply_calls(&sink, l, NULL);
	assert(!strcmp(end(&j), "{\"calls\":[]}"));
	calllog_add(l, &a, 1700000000);
	calllog_add(l, &o, 1700000100);
	begin(&j, &sink);
	reply_calls(&sink, l, NULL);
	assert(!strcmp(end(&j), "{\"calls\":[{\"id\":2,\"type\":\"OUT\",\"number\":\"45\",\"name\":\"\",\"ts\":1700000100,\"durationSec\":0,\"result\":\"busy\",\"cause\":17},"
		"{\"id\":1,\"type\":\"IN\",\"number\":\"5551\",\"name\":\"\",\"ts\":1700000000,\"durationSec\":65,\"result\":\"answered\",\"cause\":0}]}"));
	/* with a phone book the record carries the contact's name */
	phonebook_clear(&pb, NULL);
	assert(!phonebook_add(&pb, "Ann", nums, 1, NULL, stderr));
	begin(&j, &sink);
	reply_calls(&sink, l, &pb);
	assert(strstr(end(&j), "\"number\":\"5551\",\"name\":\"Ann\"") && strstr(end(&j), "\"number\":\"45\",\"name\":\"\""));
	free(l);
}

static void test_clir_readback(void)
{
	struct jtext j;
	struct phone_status st = blank();

	assert(strstr(status_json_health(&j, &st), "\"audioSlave\":null,\"hdVoice\":\"off\",\"callWaiting\":null,\"clir\":null,\"clirStatus\":null"));
	st.clir = 1;
	st.clir_status = 4;
	assert(strstr(status_json_health(&j, &st), "\"clir\":\"hide\",\"clirStatus\":\"allowed\""));
	st.clir = 0;
	st.clir_status = 0;
	assert(strstr(status_json_health(&j, &st), "\"clir\":\"network\",\"clirStatus\":\"not_provisioned\""));
	st.clir = 2;
	st.clir_status = 3;
	assert(strstr(status_json_health(&j, &st), "\"clir\":\"show\",\"clirStatus\":\"restricted\""));
	st.clir = 7;
	st.clir_status = 9;
	assert(strstr(status_json_health(&j, &st), "\"clir\":null,\"clirStatus\":null"));
}

static void test_line_test_reply(void)
{
	struct jtext j;
	struct reply_sink sink;
	struct phone_line_test_result res = { .verdict = PHONE_LT_OK, .battery_mv = 59100, .expected_mv = 60000, .loop_ua = 900 };

	begin(&j, &sink);
	reply_line_test(&sink, &res);
	assert(!strcmp(end(&j), "{\"ok\":true,\"refused\":null,\"verdict\":\"ok\",\"hook\":\"on\",\"batteryMv\":59100,\"expectedMv\":60000,\"loopUa\":900,\"leakSuspect\":false}"));
	res.verdict = PHONE_LT_OFF_HOOK;
	res.hook_off = 1;
	res.leak_suspect = 1;
	begin(&j, &sink);
	reply_line_test(&sink, &res);
	assert(strstr(end(&j), "\"ok\":false,\"refused\":null,\"verdict\":\"off_hook\",\"hook\":\"off\"") && strstr(j.text, "\"leakSuspect\":true"));
}

static void test_forwarding_reply(void)
{
	struct jtext j;
	struct reply_sink sink;
	struct phone_forward_state fw;

	memset(&fw, 0, sizeof(fw));
	fw.network = PHONE_UNKNOWN;
	begin(&j, &sink);
	reply_forwarding(&sink, &fw);
	assert(!strcmp(end(&j), "{\"network\":\"unknown\",\"to\":\"\",\"delaySec\":0,\"applying\":false,\"error\":null}"));
	fw.network = 1;
	fw.done = 1;
	snprintf(fw.number, sizeof(fw.number), "%s", "+9412345");
	fw.delay_s = 20;
	begin(&j, &sink);
	reply_forwarding(&sink, &fw);
	assert(!strcmp(end(&j), "{\"network\":\"on\",\"to\":\"+9412345\",\"delaySec\":20,\"applying\":false,\"error\":null}"));
	fw.network = 0;
	fw.applying = 1;
	fw.number[0] = 0;
	snprintf(fw.error, sizeof(fw.error), "%s", "+CME ERROR: 30");
	begin(&j, &sink);
	reply_forwarding(&sink, &fw);
	assert(!strcmp(end(&j), "{\"network\":\"off\",\"to\":\"\",\"delaySec\":20,\"applying\":true,\"error\":\"+CME ERROR: 30\"}"));
}

static void test_refusals(void)
{
	assert(reply_refusal(0, 0) == NULL);
	assert(!strcmp(reply_refusal(-ENOENT, 0), "NO_CALL"));
	assert(!strcmp(reply_refusal(-EBUSY, 1), "OFF_HOOK"));
	assert(!strcmp(reply_refusal(-EBUSY, 0), "ACTIVE_CALL"));
	assert(!strcmp(reply_refusal(-ENODEV, 0), "SERVICE_DOWN"));
	assert(!strcmp(reply_refusal(-EOPNOTSUPP, 0), "NOT_AVAILABLE"));
}

/* ---- through the real core: a call with a number, and no number in any event ---- */

#define EVENTS_MAX 64

struct course {
	struct phone_event ev[EVENTS_MAX];
	unsigned int count;
	struct fake_modem fm;
	int listed;
};

static struct course *the_course;

static int list_poll(void *user, int *st)
{
	(void)user;
	*st = the_course->listed;
	return 0;
}

static int accept_all(void *user, const char *what)
{
	(void)user;
	(void)what;
	return 0;
}

static void keep_event(void *ctx, const struct phone_event *ev)
{
	struct course *c = ctx;

	if (c->count < EVENTS_MAX)
		c->ev[c->count] = *ev;
	c->count++;
}

static void test_core_course(void)
{
	static const int32_t steady[] = { 59000, 59000, 59000, 59000, 59000 };
	static const int32_t offhook[] = { 20000, 20000, 20000, 20000, 20000 };
	static struct course c;
	struct model m;
	struct chip chip = { &model_ops, &m };
	struct board b;
	uint8_t file[4096];
	struct call_ops ops;
	struct phone_hooks hooks = { .ctx = &c, .event = keep_event };
	struct phone_cfg cfg = { .channel = TEST_CH, .service_log = 1 };
	struct phone *p;
	struct phone_status st;
	struct jtext j;
	struct reply_sink sink;
	unsigned int t, k;
	FILE *null = fopen("/dev/null", "w");
	size_t n = make_linefeed_board(file, &(struct test_board){ .with_converter = 1, .with_imped = 1, .imped_word0 = 0x1000, .with_tone = 1,
		.tone_word0 = 0x1234000, .with_iir = 1, .with_ring = 1 });

	assert(board_parse(file, n, &b) == 0);
	linefeed_model(&m, steady, 5, offhook, 5, NULL, 0);
	m.dtmf_events[0].at = 300; m.dtmf_events[0].code = 0x04;	/* '4' */
	m.dtmf_events[1].at = 375; m.dtmf_events[1].code = 0x0c;	/* '#', the send key */
	m.dtmf_event_count = 2;
	the_course = &c;
	c.fm = (struct fake_modem){ .poll = list_poll, .act = accept_all };
	c.listed = CALL_NONE;
	ops = (struct call_ops){ .ctx = &c.fm, .begin = fm_begin, .pump = fm_pump, .setup = 1, .ring = 1, .service_log = 1 };
	assert(phone_open(&p, &chip, &b, &ops, &cfg, &hooks, null) == 0);
	m.reg[TEST_CH][REG_LCRRTP] = LCR_ON_HOOK;
	for (t = 0; t < 1000; t += HOOK_POLL_MS) {
		m.vtime_ms += HOOK_POLL_MS;
		assert(phone_step(p, model_ops.now_ms(&m)) == 0);
	}
	phone_status(p, &st);
	assert(st.line == PHONE_LINE_SETUP && !strcmp(st.number, "4"));
	status_json(&j, &st, &running, model_ops.now_ms(&m));
	assert(strstr(j.text, "\"state\":\"call\",\"line\":\"setup\",\"hook\":\"off\"") && strstr(j.text, "\"number\":\"4\",\"phase\":\"dialing\""));

	/* the answer: elapsedSec counts from it */
	c.listed = CALL_ACTIVE;
	for (t = 0; t < 3000; t += HOOK_POLL_MS) {
		m.vtime_ms += HOOK_POLL_MS;
		assert(phone_step(p, model_ops.now_ms(&m)) == 0);
	}
	phone_status(p, &st);
	assert(st.answered_ms != 0);
	status_json(&j, &st, &running, model_ops.now_ms(&m));
	assert(strstr(j.text, "\"phase\":\"active\",\"elapsedSec\":"));
	assert(!strstr(j.text, "\"phase\":\"active\",\"elapsedSec\":0}"));

	/* every event of the course, as the daemon would send it: no number anywhere */
	assert(c.count > 0 && c.count <= EVENTS_MAX);
	for (k = 0; k < c.count; k++) {
		begin(&j, &sink);
		if (c.ev[k].kind == PHONE_EV_LINE)
			reply_event_line(&sink, c.ev[k].line);
		else
			reply_event_call(&sink, &c.ev[k], 0);
		end(&j);
		assert(!strstr(j.text, "number") && !strstr(j.text, "4\"") && !strstr(j.text, ":4,") && strlen(j.text) < 100);
	}
	assert(phone_close(p, 0) == 0);
	fclose(null);
}

int main(void)
{
	test_state_names();
	test_phases();
	test_status_states();
	test_status_idle();
	test_status_down();
	test_status_call_and_elapsed();
	test_status_modem_fields();
	test_health();
	test_events();
	test_calls();
	test_forwarding_reply();
	test_line_test_reply();
	test_clir_readback();
	test_refusals();
	test_core_course();
	printf("reply: all tests passed\n");
	return 0;
}
