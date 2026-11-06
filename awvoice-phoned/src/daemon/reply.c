// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <string.h>

#include "phonebook.h"
#include "dnd.h"
#include "reply.h"

/* why the line is quiet, and what was asked of it: the health reply's two words, dnd.h owns the meanings */
static const char *quiet_cause_word(int cause)
{
	switch (cause) {
	case DND_CAUSE_WINDOW:	return "window";
	case DND_CAUSE_ALL_DAY:	return "all-day";
	case DND_CAUSE_MANUAL:	return "manual";
	default: break;
	}
	return "";
}

static const char *quiet_ask_word(int manual)
{
	switch (manual) {
	case DND_MANUAL_ON:	return "quiet";
	case DND_MANUAL_OFF:	return "ringing";
	default: break;
	}
	return "schedule";
}

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
	/* the modem has not listed the call yet (or lists it as gone): all that is known is the direction and the answer */
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
	/* the network the line is registered on, as the three digits it is known by (ITU-T E.212): the code the numbering
	 * plan is chosen from when the setting is auto */
	if (st->mcc[0])
		o->text(o->ctx, "operator", st->mcc);
	else
		o->null(o->ctx, "operator");
	if (st->netname[0])
		o->text(o->ctx, "operatorName", st->netname);
	else
		o->null(o->ctx, "operatorName");
}

void reply_status(const struct reply_sink *o, const struct phone_status *st, const struct reply_info *info, unsigned long long now_ms)
{
	if (!info->line_in_service) {
		o->text(o->ctx, "state", "down");
		o->text(o->ctx, "line", "down");
		o->null(o->ctx, "hook");
		o->null(o->ctx, "machine");
		o->null(o->ctx, "quiet");
		o->null(o->ctx, "call");
	} else {
		o->text(o->ctx, "state", reply_state_name(st->line));
		o->text(o->ctx, "line", phone_line_name(st->line));
		o->text(o->ctx, "hook", st->off_hook ? "off" : "on");
		o->text(o->ctx, "machine", st->machine == PHONE_MACHINE_ANSWERING ? "answering"
			: st->machine == PHONE_MACHINE_ANSWERED ? "answered" : "none");
		/* the network offers the call and a lifted handset answers it, but quiet hours keep this side from making
		 * a sound: the page must not say the phone is ringing when it is deliberately not */
		o->flag(o->ctx, "quiet", st->quiet != 0);
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

/* one folder of the storage block: where it is, whether it took a file at the last check, and how much room it has */
static void storage_sink(const struct reply_sink *o, const char *name, const char *path, const struct reply_storage *s)
{
	o->open(o->ctx, name, 0);
	o->text(o->ctx, "path", path ? path : "");
	o->flag(o->ctx, "ready", s->ready);
	if (s->ready)
		o->number(o->ctx, "freeMb", (long long)s->free_mb);
	else
		o->null(o->ctx, "freeMb");
	if (!s->ready && s->err)
		/* the page shows the reason the folder was refused; a /mnt folder whose drive is not mounted says so */
		o->text(o->ctx, "error", s->err == ENODEV ? "the drive is not mounted" : strerror(s->err));
	else
		o->null(o->ctx, "error");
	o->close(o->ctx);
}

void reply_health(const struct reply_sink *o, const struct phone_status *st, const struct reply_info *info, unsigned long long now_ms)
{
	o->open(o->ctx, "atPort", 0);
	o->text(o->ctx, "port", info->port ? info->port : "");
	o->number(o->ctx, "usbInterface", info->at_interface);
	o->flag(o->ctx, "responding", info->line_in_service && st->modem_up);
	ago_s(o, "lastReplyAgoSec", st->modem_reply_ms, now_ms);
	o->text(o->ctx, "portOwner", info->sip_port_owner ? info->sip_port_owner : "free");
	o->close(o->ctx);

	o->open(o->ctx, "line", 0);
	o->number(o->ctx, "channel", info->channel);
	/* the numbering plan the line reads numbers with: the one the setting names, or the one the network pointed at */
	o->text(o->ctx, "numbering", info->region ? info->region : "");
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
	/* the two facts that make the setting judgeable from outside: what the modem says it puts on the bus, and what
	   this line was actually brought up at. Without them a half-changed layout is invisible - the PCM slots, the
	   chip's bring-up and the bridge move together or not at all (phone.h, phone_layout_due). */
	tri(o, "audioRate", st->audio_rate);
	o->number(o->ctx, "voiceLayout", info->layout ? 16000 : 8000);
	tri(o, "callWaiting", st->call_waiting_net);
	/* what the network reports for the caller-id restriction after the write asked here (AT+CLIR?): the setting and its provisioning state */
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
	/* the audio path's own answer, apart from the counters: "the line is quiet, nothing is crossing" and "the
	   path should be running and is not" are different faults, and the page must be able to tell them apart
	   without a shell (src/core/phone.h, phone_audio_apply) */
	o->open(o->ctx, "audioPath", 0);
	if (info->audio_ok) {
		o->flag(o->ctx, "running", info->audio_up != 0);
		o->flag(o->ctx, "wanted", info->audio_wanted != 0);
		o->text(o->ctx, "why", info->audio_reason ? info->audio_reason : "idle");
		o->open(o->ctx, "claims", 0);
		o->flag(o->ctx, "recorder", (info->audio_claims & PHONE_AUDIO_RECORDER) != 0);
		o->flag(o->ctx, "message", (info->audio_claims & PHONE_AUDIO_MESSAGE) != 0);
		o->flag(o->ctx, "page", (info->audio_claims & PHONE_AUDIO_PAGE) != 0);
		o->flag(o->ctx, "frontEnd", (info->audio_claims & PHONE_AUDIO_FRONT_END) != 0);
		o->close(o->ctx);
		o->number(o->ctx, "upMs", (long long)info->audio_up_ms);
		o->number(o->ctx, "starts", info->audio_starts);
		o->number(o->ctx, "stops", info->audio_stops);
		if (info->audio_error)
			o->text(o->ctx, "lastError", strerror(-info->audio_error));
		else
			o->null(o->ctx, "lastError");
	} else {
		o->null(o->ctx, "running");
		o->null(o->ctx, "wanted");
		o->null(o->ctx, "why");
		o->null(o->ctx, "claims");
		o->null(o->ctx, "upMs");
		o->null(o->ctx, "starts");
		o->null(o->ctx, "stops");
		o->null(o->ctx, "lastError");
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

	/* the answering machine's greeting: what the daemon holds whatever the switch says, and the word for why not */
	o->open(o->ctx, "machine", 0);
	o->flag(o->ctx, "greetingLoaded", info->machine_greeting);
	o->number(o->ctx, "greetingMs", info->machine_greeting_ms);
	o->text(o->ctx, "greetingError", info->machine_greeting_err ? info->machine_greeting_err : "");
	o->number(o->ctx, "delaySec", info->machine_delay_s);
	o->close(o->ctx);

	/* quiet hours: whether the line must stay quiet right now, what decided that, and what was asked of it. The
	 * windows themselves are the settings page's to read from the config; this is the answer they add up to. */
	o->open(o->ctx, "quiet", 0);
	{
		struct dnd_state decided;

		decided.quiet = 0;
		decided.cause = DND_CAUSE_NONE;
		if (info->dnd)
			dnd_decide(info->dnd->window, info->dnd->count, info->dnd->manual, info->dnd->now.clock_known,
				info->dnd->now.weekday, info->dnd->now.minute, &decided);
		o->flag(o->ctx, "on", decided.quiet);
		o->text(o->ctx, "cause", quiet_cause_word(decided.cause));
		o->text(o->ctx, "ask", quiet_ask_word(info->dnd ? info->dnd->manual : DND_MANUAL_AUTO));
	}
	o->close(o->ctx);

	/* the two folders the files land in, as they stood at the last check (start-up, a reload, a call's start): a
	 * folder that cannot take a file is named with its reason, so the page can warn before a call is lost */
	o->open(o->ctx, "storage", 0);
	storage_sink(o, "recordings", info->record_path, &info->record_store);
	storage_sink(o, "messages", info->message_path, &info->message_store);
	o->close(o->ctx);

	/* the line's own side of the mode: which mode the settings ask for, and the browser-audio listener that serves
	 * it (its word, its port and the address it is bound to) */
	o->open(o->ctx, "web", 0);
	o->text(o->ctx, "mode", info->sip_mode ? info->sip_mode : "web");
	o->text(o->ctx, "audio", info->sip_web_audio ? info->sip_web_audio : "off");
	o->number(o->ctx, "port", (long long)info->sip_web_port);
	o->text(o->ctx, "address", info->sip_web_addr ? info->sip_web_addr : "");
	o->close(o->ctx);
	/* the installed SIP service's side: who owns the qmodem_voip name, the last refusal the object logged, and the
	 * consumer that holds a media binding (0 while none does) */
	o->open(o->ctx, "sip", 0);
	o->text(o->ctx, "objectOwner", info->sip_object_owner ? info->sip_object_owner : "none");
	o->text(o->ctx, "lastRefusal", info->sip_last_refusal ? info->sip_last_refusal : "");
	o->number(o->ctx, "consumerPid", info->sip_consumer_pid);
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
		o->text(o->ctx, "recording", e->recording);
		o->text(o->ctx, "message", e->message);
		o->text(o->ctx, "answeredBy", phone_answered_by_name(e->answered_by));
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

/*
 * The one word a greeting's state goes by. The health reply and the upload methods' refusals share it, so the page
 * learns one vocabulary about one file: the word names what is wrong with the audio, and the page turns it into a
 * sentence the user can act on. An errno nobody expected still gets a word - "UNREADABLE" - because a file the daemon
 * could not make sense of is worth telling the user about whatever the reason turns out to be.
 */
const char *reply_greeting_word(int err)
{
	switch (err) {
	case 0:			return "";
	case -ENOENT:		return "MISSING";
	case -EINVAL:		return "FORMAT";
	case -ENODATA:	return "EMPTY";
	case -EBADMSG:	return "TRUNCATED";
	case -EFBIG:		return "TOO_BIG";
	case -ENOMEM:		return "MEMORY";
	case -ELOOP:		return "SYMLINK";
	case -EBUSY:		return "BUSY";
	case -EPROTO:	return "OFFSET";
	case -EIO:		return "IO";
	default:		return "UNREADABLE";
	}
}
