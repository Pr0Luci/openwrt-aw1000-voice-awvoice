// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>

#include "phonebook.h"
#include "reply.h"

const char *reply_state_name(enum phone_line line)
{
	switch (line) {
	case PHONE_LINE_IDLE:		return "idle";
	case PHONE_LINE_DIAL_TONE:
	case PHONE_LINE_DIALLING:	return "offhook";
	case PHONE_LINE_RINGING:	return "ring";
	case PHONE_LINE_SETUP:
	case PHONE_LINE_CALL:
	case PHONE_LINE_RELEASE:	return "call";
	case PHONE_LINE_NO_SERVICE:	return "noservice";
	}
	return "idle";
}

const char *reply_phase_name(enum call_state call, enum phone_dir dir, int answered)
{
	switch (call) {
	case CALL_INCOMING:	return "ringing";
	case CALL_DIALING:	return "dialing";
	case CALL_ALERTING:	return "alerting";
	case CALL_ACTIVE:
	case CALL_HELD:		return "active";
	case CALL_GONE:
	case CALL_NONE:
		break;
	}
	/* the modem has not listed the call yet (or lists it as gone): what we know is the direction and the answer */
	if (answered)
		return "active";
	return dir == PHONE_DIR_IN ? "ringing" : "dialing";
}

/* a tri-state of the status (1 / 0 / PHONE_UNKNOWN) as a flag or null */
static void tri(const struct reply_sink *o, const char *name, int v)
{
	if (v == PHONE_UNKNOWN)
		o->null(o->ctx, name);
	else
		o->flag(o->ctx, name, v != 0);
}

static void ago_s(const struct reply_sink *o, const char *name, unsigned long long then_ms, unsigned long long now_ms)
{
	if (!then_ms)
		o->null(o->ctx, name);
	else
		o->number(o->ctx, name, now_ms > then_ms ? (long long)((now_ms - then_ms) / 1000) : 0);
}

static void network(const struct reply_sink *o, const struct phone_status *st)
{
	if (st->network[0])
		o->text(o->ctx, "network", st->network);
	else
		o->null(o->ctx, "network");
}

void reply_status(const struct reply_sink *o, const struct phone_status *st, const struct reply_info *info, unsigned long long now_ms)
{
	if (!info->line_in_service) {
		o->text(o->ctx, "state", "down");
		o->text(o->ctx, "line", "down");
		o->null(o->ctx, "hook");
		o->null(o->ctx, "call");
	} else {
		o->text(o->ctx, "state", reply_state_name(st->line));
		o->text(o->ctx, "line", phone_line_name(st->line));
		o->text(o->ctx, "hook", st->off_hook ? "off" : "on");
		if (!st->call_id) {
			o->null(o->ctx, "call");
		} else {
			o->open(o->ctx, "call", 0);
			o->number(o->ctx, "id", st->call_id);
			o->text(o->ctx, "direction", st->dir == PHONE_DIR_IN ? "IN" : "OUT");
			o->text(o->ctx, "number", st->number);
			o->text(o->ctx, "phase", reply_phase_name(st->call, st->dir, st->answered_ms != 0));
			o->number(o->ctx, "elapsedSec", st->answered_ms && now_ms > st->answered_ms ? (long long)((now_ms - st->answered_ms) / 1000) : 0);
			o->close(o->ctx);
		}
	}
	o->open(o->ctx, "voiceNetwork", 0);
	tri(o, "registered", st->registered);
	tri(o, "ims", st->ims_registered);
	o->close(o->ctx);
	o->open(o->ctx, "signal", 0);
	if (st->signal_dbm == PHONE_DBM_UNKNOWN)
		o->null(o->ctx, "rsrpDbm");
	else
		o->number(o->ctx, "rsrpDbm", st->signal_dbm);
	network(o, st);
	o->close(o->ctx);
	o->open(o->ctx, "service", 0);
	o->flag(o->ctx, "running", 1);
	o->close(o->ctx);
	o->text(o->ctx, "myNumber", info->my_number ? info->my_number : "");
}

void reply_health(const struct reply_sink *o, const struct phone_status *st, const struct reply_info *info, unsigned long long now_ms)
{
	o->open(o->ctx, "atPort", 0);
	o->text(o->ctx, "port", info->port ? info->port : "");
	o->number(o->ctx, "usbInterface", info->at_interface);
	o->flag(o->ctx, "responding", info->line_in_service && st->modem_up);
	ago_s(o, "lastReplyAgoSec", st->modem_reply_ms, now_ms);
	o->close(o->ctx);

	o->open(o->ctx, "line", 0);
	o->number(o->ctx, "channel", info->channel);
	o->text(o->ctx, "hook", st->off_hook ? "off" : "on");
	o->text(o->ctx, "state", info->line_in_service ? phone_line_name(st->line) : "down");
	o->flag(o->ctx, "calibrated", info->line_in_service);	/* the daemon serves only after the calibration stages passed */
	o->number(o->ctx, "batteryMv", st->battery_mv);
	o->number(o->ctx, "loopUa", st->loop_ua);
	o->close(o->ctx);

	o->open(o->ctx, "modem", 0);
	tri(o, "simReady", st->sim_ready);
	tri(o, "registered", st->registered);
	tri(o, "imsRegistered", st->ims_registered);
	if (st->signal_dbm == PHONE_DBM_UNKNOWN)
		o->null(o->ctx, "rsrpDbm");
	else
		o->number(o->ctx, "rsrpDbm", st->signal_dbm);
	network(o, st);
	tri(o, "audioSlave", st->audio_slave);
	o->text(o->ctx, "hdVoice", info->hd == 2 ? "on" : info->hd == 1 ? "set" : "off");
	tri(o, "callWaiting", st->call_waiting_net);
	/* what the network reports for the caller-id restriction after our write (AT+CLIR?): the setting and its provisioning state */
	if (st->clir < 0 || st->clir > 2)
		o->null(o->ctx, "clir");
	else
		o->text(o->ctx, "clir", st->clir == 1 ? "hide" : st->clir == 2 ? "show" : "network");
	if (st->clir_status < 0 || st->clir_status > 4)
		o->null(o->ctx, "clirStatus");
	else
		o->text(o->ctx, "clirStatus", (const char *const[]){ "not_provisioned", "provisioned", "unknown", "restricted", "allowed" }[st->clir_status]);
	o->close(o->ctx);

	o->open(o->ctx, "system", 0);
	o->flag(o->ctx, "moduleLoaded", info->module_loaded);
	o->open(o->ctx, "bridge", 0);
	if (info->bridge_ok) {
		o->flag(o->ctx, "running", info->bridge_running != 0);
		o->number(o->ctx, "frames", (long long)info->bridge_frames);
		o->number(o->ctx, "resyncs", info->bridge_resyncs);
		o->number(o->ctx, "txSkipped", info->bridge_tx_skipped);
		o->number(o->ctx, "maxGapUs", info->bridge_max_gap_us);
	} else {
		o->null(o->ctx, "running");
		o->null(o->ctx, "frames");
		o->null(o->ctx, "resyncs");
		o->null(o->ctx, "txSkipped");
		o->null(o->ctx, "maxGapUs");
	}
	o->close(o->ctx);
	o->close(o->ctx);

	o->open(o->ctx, "indicator", 0);
	if (info->ind && info->ind->led[0])
		o->text(o->ctx, "led", info->ind->led);
	else
		o->null(o->ctx, "led");
	o->text(o->ctx, "state", info->ind ? indicator_state_name(info->ind) : "off");
	if (info->nleds > 0) {
		int k;

		o->open(o->ctx, "leds", 1);
		for (k = 0; k < info->nleds; k++)
			o->text(o->ctx, NULL, info->leds[k]);
		o->close(o->ctx);
	} else {
		o->null(o->ctx, "leds");
	}
	o->close(o->ctx);

	o->open(o->ctx, "counters", 0);
	o->number(o->ctx, "modemTimeouts", st->modem_timeouts);
	o->number(o->ctx, "hookGapsOver20ms", st->hook_gaps);
	o->number(o->ctx, "hookGapMaxMs", st->hook_gap_max_ms);
	o->number(o->ctx, "faults", st->faults);
	o->close(o->ctx);
}

void reply_event_line(const struct reply_sink *o, enum phone_line line)
{
	o->text(o->ctx, "state", reply_state_name(line));
	o->text(o->ctx, "line", phone_line_name(line));
}

void reply_event_call(const struct reply_sink *o, const struct phone_event *ev, int answered)
{
	const int ended = ev->call == CALL_GONE || ev->call == CALL_NONE;

	o->number(o->ctx, "id", ev->call_id);
	o->text(o->ctx, "direction", ev->dir == PHONE_DIR_IN ? "IN" : "OUT");
	o->text(o->ctx, "phase", ended ? "ended" : reply_phase_name(ev->call, ev->dir, answered));
}

void reply_calls(const struct reply_sink *o, const struct calllog *log, const struct phonebook *book)
{
	unsigned int k;

	o->open(o->ctx, "calls", 1);
	for (k = 0; k < log->n; k++) {
		const struct calllog_entry *e = &log->e[k];
		char name[PB_NAME_MAX + 1];

		o->open(o->ctx, NULL, 0);
		o->number(o->ctx, "id", e->id);
		o->text(o->ctx, "type", calllog_type_name(e->type));
		o->text(o->ctx, "number", e->number);
		/* the name is the daemon's to find: it compares the numbers in one form, the page need not */
		o->text(o->ctx, "name", book && e->number[0] && !phonebook_name(book, e->number, name, sizeof(name)) ? name : "");
		o->number(o->ctx, "ts", (long long)e->ts);
		o->number(o->ctx, "durationSec", e->duration_s);
		o->text(o->ctx, "result", phone_result_name(e->result));
		o->number(o->ctx, "cause", e->cause);
		o->close(o->ctx);
	}
	o->close(o->ctx);
}

void reply_forwarding(const struct reply_sink *o, const struct phone_forward_state *fwd)
{
	o->text(o->ctx, "network", fwd->network == PHONE_UNKNOWN ? "unknown" : fwd->network ? "on" : "off");
	o->text(o->ctx, "to", fwd->number);
	o->number(o->ctx, "delaySec", fwd->delay_s);
	o->flag(o->ctx, "applying", fwd->applying);
	if (fwd->error[0])
		o->text(o->ctx, "error", fwd->error);
	else
		o->null(o->ctx, "error");
}

void reply_line_test(const struct reply_sink *o, const struct phone_line_test_result *res)
{
	o->flag(o->ctx, "ok", res->verdict == PHONE_LT_OK);
	o->null(o->ctx, "refused");
	o->text(o->ctx, "verdict", phone_line_verdict_name(res->verdict));
	o->text(o->ctx, "hook", res->hook_off ? "off" : "on");
	o->number(o->ctx, "batteryMv", res->battery_mv);
	o->number(o->ctx, "expectedMv", res->expected_mv);
	o->number(o->ctx, "loopUa", res->loop_ua);
	o->flag(o->ctx, "leakSuspect", res->leak_suspect);
}

const char *reply_refusal(int err, int off_hook)
{
	switch (err) {
	case 0:		return NULL;
	case -ENOENT:	return "NO_CALL";
	case -EBUSY:	return off_hook ? "OFF_HOOK" : "ACTIVE_CALL";
	case -ENODEV:	return "SERVICE_DOWN";
	default:	return "NOT_AVAILABLE";
	}
}
