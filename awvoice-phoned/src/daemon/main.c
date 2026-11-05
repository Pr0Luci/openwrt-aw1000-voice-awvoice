// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * awvoice-phoned: the phone service. One thread, one event loop: a periodic 5 ms clock calls the call
 * core's phone_step(); the modem is pumped inside that step, so nothing else may block. The start-up is the stage
 * tool's --live path (the board's init section, stages 0..4, then the voice path); the settings come from UCI.
 *
 * Exit codes: 0 = asked to stop, or the service is switched off, or the line has to be set up again from the start
 * (a changed channel or board); 1 = a fault (procd starts the service again).
 * The one option, --check-module, is the init script's handshake with the kernel module: it reads the module's interface
 * number and returns 0, 1 or 2 for it (see check_module below) or 3 when the device cannot be read.
 *
 * The log is stdout, line buffered; procd forwards it to the system log. Never a phone number, a digit or a name.
 */
#define _GNU_SOURCE	/* struct ucred: the media socket's peer pid, which the health reply carries */
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>

#include <libubox/uloop.h>
#include <libubox/blobmsg.h>
#include <libubus.h>

#include "answer.h"
#include "api.h"
#include "alarm.h"
#include "blocking.h"
#include "forward.h"
#include "front_end.h"
#include "calllog.h"
#include "phonebook.h"
#include "numbers.h"
#include "region_follow.h"
#include "board.h"
#include "cid.h"
#include "config.h"
#include "hw.h"
#include "indicator.h"
#include "media_rate.h"
#include "play_mirror.h"
#include "play_fill.h"
#include "dnd.h"
#include "provision.h"
#include "machine.h"
#include "module_abi.h"
#include "phone.h"
#include "port.h"
#include "qmsk_server.h"
#include "recorder.h"
#include "recording.h"
#include "script.h"
#include "call_arbiter.h"
#include "sip_guard.h"
#include "slic_map.h"
#include "stages.h"
#include "storage.h"
#include "web_audio.h"
#include "web_proto.h"

#define LOCK_PATH	"/var/run/awvoice-phoned.lock"
#define CALLS_RAM_PATH	"/var/run/awvoice-phoned/calls.json"
#define CALLS_FLASH_PATH "/etc/awvoice/calls.json"
#define INIT_SECTION	"init_nopatch"
#define BOARD_FILE_MAX	(64 * 1024)	/* far above a real board file; a larger file is not one */

struct daemon {
	struct aw_config cfg;
	struct device dev;
	struct chip chip;
	struct board board;
	uint8_t *board_file;
	struct answer ans;
	int ans_open;
	unsigned long long port_next_ms;	/* the next time to look for the modem port */
	unsigned long long port_logged_ms;	/* the last "port missing" line (0 = none yet) */
	struct call_ops ops;
	struct phone_hooks hooks;
	struct phone *phone;
	struct uloop_fd timer_fd, signal_fd;
	struct ubus_auto_conn ubus;
	struct api_ctx api;
	struct calllog calls;
	struct phonebook book;
	struct blocking block;
	struct dnd dnd;		/* quiet hours: the windows and the numbers that ring through them */
	int quiet;			/* the last status the line published said quiet hours cover this caller */
	unsigned long long dnd_clock_ms;	/* when the clock behind the quiet hours was last read */
	const struct number_region *region;	/* the numbering plan in force: the one the setting names, or the one the reported network points at */
	char region_mcc[4];				/* the network code the plan was chosen from (E.212), "" while none was read */
	struct alarms alarms;
	struct phone_forward forward;
	struct indicator ind;		/* the phone LED (indicator.c): the line state, driven through /sys/class/leds */
	unsigned int audio_call;		/* the answered call whose audio counters were noted at its start (0 = none) */
	struct awfxs_bridge_stats audio_base;
	struct recorder rec;		/* the answered call being recorded (awvoice.record.enabled); one at a time */
	int rec_armed;			/* the recorder is armed on the capture tap */
	u32 rec_call_id;		/* the recorded call's id: it names the file with the epoch */
	int rec_rate;			/* the recording's sample rate: the call's own (8000, or 16000 on HD) */
	char rec_name[64];		/* the recording's file name while it writes */
	struct machine machine;		/* the answering machine's greeting and its one message (awvoice.machine.*) */
	int play_armed;			/* the kernel playback ring is armed (the greeting, or the front end's audio) */
	u32 play_generation;		/* the playback generation the greeting follows */
	struct play_mirror play_mirror;	/* the front end's audio as it waits for its slot: the recording's second channel */
	int msg_call;			/* the machine's call whose message is coming (0 = none) */
	unsigned long long msg_at_ms;	/* the call's answer: the message file name's epoch */
	int msg_armed;			/* the machine's message is armed on the capture tap */
	int msg_expected;		/* the greeting is through: the message starts (the ring's count says when) */
	u32 msg_greeting_frames;	/* the ring frames the greeting fills */
	u32 msg_max_frames;		/* the message's cap in mono frames (machine_max_s at the call's rate) */
	int msg_rate;			/* the message's rate: the call's own (8000, or 16000 on HD) */
	int greeting_stale;		/* a reload changed the greeting while a machine call was under way */
	struct machine_upload greeting_up;	/* the page's greeting upload, staged in the folder's tmp (one at a time) */
	struct fe *fe;			/* the front end's module: the call view, the cut and the ring owners (front_end.h) */
	struct ctl_ops ctl_ops;	/* what the arbiter asks of the line: answer, dial, hangup, a digit, the status (call_arbiter.h) */
	struct call_arbiter arbiter;		/* the call's arbiter and published view (call_arbiter.h) */
	struct sip_guard_state guard;	/* the line and the name, as the last guard run found them */
	struct uloop_timeout guard_timer;	/* the slow re-check: a freed port, a freed name or a listener to retry */
	struct uloop_timeout provision_timer;	/* the slow round that retires the scanned code's account files */
	struct qmsk_server media;	/* the media socket: the front end's frames, and the binding ids minted from it */
	struct uloop_fd media_fd;	/* the listening socket in the loop (fd -1 while it is not) */
	unsigned int media_pid[QMSK_PEERS];	/* each peer's pid, for the health reply (0 for a free slot) */
	int fe_armed;			/* the capture tap is armed for the front end's frames */
	int fe_arm_logged;		/* the tap's last failure was said (a repeated one says nothing new) */
	int fe_play_logged;		/* the front end's playback last failure was said */
	struct web_audio *web;		/* the page's browser audio: the TLS listener and its one session */
	struct uloop_fd web_fd;		/* the listening socket in the loop (fd -1 while it is not) */
	int web_armed;			/* the page's audio is on the tap (FE_SINK_WEB) */
	char web_reason[48];		/* why the listener is (not) up: the health word */
	char web_addr[16];		/* the network address it is bound to (a moved address is a new listener) */
	unsigned int web_port_used;	/* the port it listens on (a changed port is a new listener) */
	long web_cert_mtime;		/* the certificate pair it loaded: a replaced pair is a new listener */
	long web_cert_size;
	long web_key_mtime;
	long web_key_size;
	int loop_up;			/* the event loop exists: the media socket may join it */
	int tap_armed;			/* the kernel capture tap is armed for every sink that needs it */
	int tap_wideband;		/* what the kernel last said about the bridge: 1 = a frame carries two samples */
	int hd_layout;			/* the layout the audio path was brought up with: 0 = 8 kHz, 1 = 16 kHz (the PCM slots, the chip and the bridge agree on it) */
	u32 tap_generation;		/* the tap generation the drain follows */
	struct storage_state rec_store;	/* the recordings folder: made and proven at start-up, on reload, at a call */
	struct storage_state msg_store;	/* the messages folder, the same way */
	int exit_code;
};

static struct daemon d;

static void sip_guards_run(struct daemon *dm);	/* defined with the front end: a port that appears or goes re-runs them */

static unsigned long long mono_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return now.tv_sec * 1000ull + now.tv_nsec / 1000000;
}

static void report(const char *what, int err)
{
	fprintf(stderr, "%s: %s\n", what, strerror(abs(err)));
}

/* ---- the modem ops of the core, on the daemon's AT port ------------------------------------------------------------ */

static int modem_begin(void *ctx, enum call_req kind, const char *cmd)
{
	struct daemon *dm = ctx;

	return answer_begin(&dm->ans, (enum answer_kind)kind, cmd, mono_ms());
}

static int modem_pump(void *ctx, int *result)
{
	struct daemon *dm = ctx;

	return answer_pump(&dm->ans, mono_ms(), result);
}

static const char *modem_reply(void *ctx)
{
	static char line[64];
	struct daemon *dm = ctx;

	return answer_reply_line(&dm->ans, line, sizeof(line));
}

static const char *modem_body(void *ctx)
{
	struct daemon *dm = ctx;

	return answer_body(&dm->ans);
}

static unsigned int modem_legs(void *ctx, struct call_leg *out, unsigned int max)
{
	struct daemon *dm = ctx;
	unsigned int n = dm->ans.nlegs < max ? dm->ans.nlegs : max;

	memcpy(out, dm->ans.legs, n * sizeof(out[0]));
	return n;
}

static int modem_number(void *ctx, char *out, size_t cap)
{
	struct daemon *dm = ctx;

	snprintf(out, cap, "%s", dm->ans.number);
	return 0;
}

/* the caller's name from the phone book (the page writes the contacts) */
static int modem_name(void *ctx, const char *number, char *out, size_t cap)
{
	struct daemon *dm = ctx;

	return phonebook_name(&dm->book, number, out, cap);
}

/* ---- the settings, as the core takes them ---------------------------------------------------------------------------- */

static void phone_cfg_of(const struct aw_config *c, struct phone_cfg *out)
{
	*out = (struct phone_cfg){
		.channel = c->channel,
		.hook = { .flash_min_ms = c->flash_min_ms, .flash_max_ms = c->flash_max_ms },
		.dial_pause_ms = c->dial_pause_s * 1000u,
		.clir_set = 1, .clir = c->clir,
		.call_waiting = c->call_waiting, .conference = c->conference,
		.hotline_delay_ms = c->hotline_delay_s * 1000u,
		.region = d.region,				/* the plan in force, not the setting's word: auto resolves through the network (region_follow) */
		.book = &d.book,
		.block = &d.block,
		.dnd = &d.dnd,
		.alarms = &d.alarms,
		.forward = d.forward,
		.cid = CID_DTMF,
		.hd = c->hd,
		.machine = c->machine_enabled && d.machine.greeting_loaded && storage_can_write(&d.msg_store),
		.machine_delay_ms = c->machine_delay_s * 1000u,
		.no_cid = c->cid_kind == AW_CID_OFF,
		.cid_prering = c->cid_prering,
		.ringback = c->ringback,
		.service_log = 1,
		.level_trace = c->level_trace,
		.volume_set = c->earpiece_volume != AW_CONFIG_UNSET, .earpiece_volume = c->earpiece_volume,
		.rx_gain_set = c->modem_rx_gain != AW_CONFIG_UNSET, .modem_rx_gain = c->modem_rx_gain,
	};
	snprintf(out->hotline, sizeof(out->hotline), "%s", c->hotline_number);
}

/* the call ops carry what the core reads from them directly: caller id and the hook limits' fallback */
static void call_ops_of(struct daemon *dm)
{
	dm->ops.ctx = dm;
	dm->ops.begin = modem_begin;
	dm->ops.pump = modem_pump;
	dm->ops.reply = modem_reply;
	dm->ops.body = modem_body;
	dm->ops.number = modem_number;
	dm->ops.legs = modem_legs;
	dm->ops.name = modem_name;
	dm->ops.setup = 1;			/* AT+CLIP=1, AT+CRC=1 at every modem open */
	dm->ops.ring = 1;
	dm->ops.service_log = 1;
	dm->ops.cid = dm->cfg.cid_kind == AW_CID_OFF ? CID_OFF
		: dm->cfg.cid_kind == AW_CID_BELLCORE ? CID_BELLCORE
		: dm->cfg.cid_kind == AW_CID_ETSI ? CID_ETSI : CID_DTMF;	/* the DTMF variant is the one verified on this board */
	dm->ops.hook_cfg.flash_min_ms = dm->cfg.flash_min_ms;
	dm->ops.hook_cfg.flash_max_ms = dm->cfg.flash_max_ms;
}

/* ---- the modem port: found by USB interface, looked for again while it is missing -------------------------------- */

static void modem_find(unsigned long long now)
{
	char path[sizeof(d.ans.port)];
	int err = port_find(PORT_SYSFS_ROOT, d.cfg.at_interface, path, sizeof(path));

	if (!err) {
		err = answer_open_path(&d.ans, path);
		if (err)
			printf("the modem port %s cannot be opened (%s)\n", path, strerror(-err));
		else if (port_exclusive(d.ans.fd))
			printf("the voice port would not take the exclusive flag: a second opener is not refused by the tty\n");
	}
	if (err) {
		d.port_next_ms = now + PORT_RETRY_MS;
		if (!d.port_logged_ms || now - d.port_logged_ms >= PORT_MISSING_LOG_MS) {
			printf("no modem port yet (USB interface %u): the line works without calls until it appears\n", d.cfg.at_interface);
			d.port_logged_ms = now;
		}
		return;
	}
	d.ans_open = 1;
	d.port_logged_ms = 0;
	printf("the modem port is %s\n", d.ans.port);
	if (d.phone)
		phone_modem_up(d.phone);
	sip_guards_run(&d);		/* the port is here now: who else holds it decides whether a front end may run */
}

static void modem_close(void)
{
	answer_close(&d.ans);
	d.ans_open = 0;
	d.ans.port[0] = 0;		/* the status shows no port */
	sip_guards_run(&d);		/* the port is gone: the guard's answer about it changes */
}

/* after each step: a port the core no longer trusts is closed and looked for again (the modem may have reset) */
static void modem_watch(unsigned long long now)
{
	struct phone_status st;

	if (!d.ans_open) {
		if (now >= d.port_next_ms)
			modem_find(now);
		return;
	}
	phone_status(d.phone, &st);
	if (st.modem_up)
		return;
	modem_close();
	d.port_next_ms = now + PORT_RETRY_MS;
	printf("the modem port was closed: looking for it again\n");
}

/* ---- the answering machine: the greeting, played toward the caller; the message, recorded from the tap ----------------- */

/* the controller below runs from the step: the greeting's ring is refilled at every tick, the tap drained */
static void machine_play_feed(struct daemon *dm);
static void machine_call_start(struct daemon *dm, const struct phone_event *ev);
static void machine_call_end(struct daemon *dm);
static void machine_no_message(struct daemon *dm, const char *why);
static void layout_rebuild(struct daemon *dm, int want);	/* the audio path at another rate: bring_up is defined below the step loop */
static void storage_measure(struct daemon *dm);	/* both folders checked, the reply told (the storage section below) */

/* the line is quiet when a file may be read: on hook, no call, no message in progress. A flash read may take a moment
 * and the 5 ms step must not wait for it; the call log is written by the same rule. */
static int line_quiet(struct daemon *dm)
{
	struct phone_status st;

	if (dm->msg_call)
		return 0;
	phone_status(dm->phone, &st);
	return st.line == PHONE_LINE_IDLE && !st.off_hook;
}

/*
 * Read the greeting for the current settings. The file is read whether or not the machine is switched on: the page's
 * card, the Play button and the next save all want to know what the managed file actually is, and whether the machine
 * answers is phone_cfg_of's decision, made from this one plus the switch and the message folder. A refusal still drops
 * what was loaded - a file the machine cannot play is not a greeting - but it is only logged when the machine is
 * switched on, because a router with the feature off and no greeting is not a fault.
 *
 * -EBUSY while the greeting is in flight or the line is busy: the audio is neither freed under the fill nor read while
 * a call is up, and the reload's deferred path picks it up when the line is quiet.
 */
static int machine_refresh(struct daemon *dm)
{
	unsigned long long t0;
	int err;

	if (dm->machine.playing || dm->machine.recording)
		return -EBUSY;		/* checked first: the greeting must not be freed under the fill */
	if (!line_quiet(dm))
		return -EBUSY;
	t0 = mono_ms();
	err = machine_greeting_load(&dm->machine, MACHINE_GREETING_PATH, dm->cfg.machine_enabled ? stdout : NULL);
	if (!err)
		printf("the greeting was loaded in %llu ms\n", mono_ms() - t0);
	return err;
}

/* a greeting may be swapped into place only when nothing is playing out of it and the line is quiet: a call up, the
 * handset off hook, the machine's own fill in flight, or the playback ring armed for anyone at all says no */
static int greeting_may_swap(void *ctx)
{
	struct daemon *dm = ctx;

	if (dm->machine.playing || dm->machine.recording || dm->play_armed)
		return 0;
	return line_quiet(dm);
}

/*
 * The core caches the answer flag it was last given, so a greeting that arrives or goes must be pushed or the machine
 * keeps answering - or keeps refusing - as it did before the file changed. The same pair the deferred reload uses.
 */
static void greeting_applied(void *ctx)
{
	struct daemon *dm = ctx;
	struct phone_cfg pc;

	phone_cfg_of(&dm->cfg, &pc);
	if (phone_set_cfg(dm->phone, &pc))
		printf("the call core refused the settings after the greeting changed: it takes them at the next reload\n");
}

/* the greeting a reload left for later: loaded now that the machine's call is over and the line is quiet, and the core
 * gets the settings it was waiting for (the machine flag follows the greeting: a file that cannot be used switches the
 * machine off) */
static void machine_retry(struct daemon *dm)
{
	struct phone_cfg pc;

	if (machine_refresh(dm) == -EBUSY)
		return;			/* the greeting is still in use, or a call began in between: later */
	dm->greeting_stale = 0;
	phone_cfg_of(&dm->cfg, &pc);
	if (phone_set_cfg(dm->phone, &pc))
		printf("the new settings were refused\n");
	else
		printf("the deferred settings were applied\n");
}

/* ---- the capture tap: one drain, every sink ------------------------------------------------------------------------- */

/* the call's own rate: 8000, or 16000 on HD (the bridge runs at it, and so do the tap and the files) */
/* the rate the audio is really moving at: the kernel's own word about the bridge, never the setting and never the
 * chip's register - a WAV header may only claim what the frames actually carry */
static int tap_rate(struct daemon *dm)
{
	return dm->tap_wideband ? 16000 : 8000;
}

/* A sink is armed: its bit goes in, and the device is armed only when it was not already (arming clears the ring
 * under the sinks that are on it). The generation the drain follows is the one of the arm's own cleared ring. */
static int tap_add(struct daemon *dm, unsigned int bits)
{
	struct fe_owners *own = fe_owners(dm->fe);
	struct awfxs_tap_frame frames[32];
	struct awfxs_tap_read meta;
	int err;

	fe_sink_set(own, bits, 1);
	if (dm->tap_armed)
		return 0;
	err = dev_tap_set(&dm->dev, 1);
	if (err) {
		fe_sink_set(own, bits, 0);
		return err;
	}
	(void)dev_tap_read(&dm->dev, frames, 32, &meta);	/* the arm reset the ring: take the generation to follow */
	dm->tap_generation = meta.generation;
	dm->tap_wideband = !!meta.wideband;
	dm->tap_armed = 1;
	(void)fe_capture_reset(dm->fe, (u32)tap_rate(dm), meta.wideband);
	return 0;
}

/* a sink is done: its bit goes, and the device follows when no sink is left */
static void tap_drop(struct daemon *dm, unsigned int bits)
{
	fe_sink_set(fe_owners(dm->fe), bits, 0);
	if (!fe_tap_needed(fe_owners(dm->fe)) && dm->tap_armed) {
		(void)dev_tap_set(&dm->dev, 0);
		dm->tap_armed = 0;
	}
}

/* The bridge restarted under the tap (an HD switch changes the rate with it): each armed sink handles what it holds,
 * and the cut restarts at the new rate before the first slot of the new generation is fed. */
static void tap_new_generation(struct daemon *dm, const struct awfxs_tap_read *meta, unsigned long long now)
{
	long long saved;
	int err;

	dm->tap_generation = meta->generation;
	if (dm->rec_armed) {
		/* the part so far is kept and a new file starts at the new rate */
		saved = recorder_finish(&dm->rec);
		if (saved >= 0)
			printf("the recording goes on in a new file after the rate change (%lld bytes kept)\n", saved);
		dm->rec_rate = tap_rate(dm);
		snprintf(dm->rec_name, sizeof(dm->rec_name), "%lld-%u.wav", api_epoch_s(now, now), dm->rec_call_id);
		char base[sizeof(dm->rec_name)];

		snprintf(base, sizeof(base), "%.*s", (int)strlen(dm->rec_name) - 4, dm->rec_name);
		err = recorder_start(&dm->rec, dm->cfg.record_path, base, (u32)dm->rec_rate, 2);
		if (err) {
			printf("the recording stops at the rate change: %s: %s\n", dm->cfg.record_path, strerror(-err));
			tap_drop(dm, FE_SINK_RECORDER);
			dm->rec_call_id = 0;
			dm->rec_name[0] = 0;	/* the log never names a file that was not written */
			dm->rec_armed = 0;
		}
	}
	if (dm->msg_armed) {
		printf("the capture tap was reset under the message: it is kept and the recording stops\n");
		tap_drop(dm, FE_SINK_MACHINE);
		dm->msg_armed = 0;
		if (dm->machine.recording && machine_message_finish(&dm->machine) > 0)
			printf("the answering machine's message was saved\n");
		machine_no_message(dm, "the capture tap was reset");
	}
	(void)fe_capture_reset(dm->fe, (u32)tap_rate(dm), meta->wideband);
}

/* The tap's one drain: up to 32 x 256 frames a tick, the generation judged once, then every armed sink takes the
 * same frames in one pass: the recorder's stereo pair, the machine's message, the front end's slots. */
static void tap_tick(struct daemon *dm, unsigned long long now)
{
	struct awfxs_tap_frame frames[256];
	struct awfxs_tap_read meta;
	struct fe_slot slots[256];
	uint16_t samples[1024];
	int16_t mono[512];
	unsigned int got, k, n, loops = 0;
	int err;

	if (!dm->tap_armed)
		return;			/* no sink is on the tap: nothing is read */
	/* the bridge hands out 8000 frames a second and a tick is 5 ms: a single read of 32 frames drained
	 * only 6400 a second, the ring filled in about five seconds and the file turned to distortion.
	 * Drain until the ring is empty (bounded by the ring itself) before the next tick. */
	do {
		got = dev_tap_read(&dm->dev, frames, 256, &meta);
		dm->tap_wideband = !!meta.wideband;		/* the bridge's layout as the kernel says it, read by read */
		if (meta.generation != dm->tap_generation)
			tap_new_generation(dm, &meta, now);
		if (!got)
			break;
		if (dm->rec_armed) {
			unsigned int mir_n = 0, mir_k = 0;
			u16 mir[512];
			int front = dm->play_armed && (fe_owners(dm->fe)->play == FE_PLAY_FRONT_END ||
				fe_owners(dm->fe)->play == FE_PLAY_WEB);

			/* the front end's audio never comes back through the tap, so the second channel takes it from the
			 * mirror, in step with the slots the bridge drained; the page's browser audio is the same case.
			 * The handset's own slot is the fallback */
			if (front)
				mir_n = play_mirror_pull(&dm->play_mirror, mir, got * ((meta.wideband && dm->rec_rate == 16000) ? 2u : 1u));
			/* interleaved stereo: the far end (the modem's slot) and the handset (the line chip's slot);
			 * wideband adds the second time sample of each, so the pair is 16 kHz like the call itself */
			for (k = 0, n = 0; k < got; k++) {
				samples[n++] = frames[k].modem;
				samples[n++] = mir_k < mir_n ? mir[mir_k++] : frames[k].line;
				if (meta.wideband && dm->rec_rate == 16000) {
					samples[n++] = frames[k].modem2;
					samples[n++] = mir_k < mir_n ? mir[mir_k++] : frames[k].line2;
				}
			}
			err = recorder_feed(&dm->rec, samples, n);
			if (err) {
				printf("the recording failed at %s: %s\n", dm->cfg.record_path, strerror(-err));
				tap_drop(dm, FE_SINK_RECORDER);	/* the failed feed discarded its tmp: the sink goes with it */
				dm->rec_call_id = 0;
				dm->rec_name[0] = 0;		/* the log never names a file that was not written */
				dm->rec_armed = 0;
			}
		}
		/* the greeting is still going: the caller's words now are its echo, so the message skips them */
		if (dm->msg_armed && dm->machine.recording) {
			for (k = 0, n = 0; k < got; k++) {
				mono[n++] = (int16_t)frames[k].modem;
				if (meta.wideband && dm->msg_rate == 16000)
					mono[n++] = (int16_t)frames[k].modem2;
			}
			err = machine_message_feed(&dm->machine, mono, n);
			if (err) {
				printf("the answering machine's message failed: %s\n", strerror(-err));
				tap_drop(dm, FE_SINK_MACHINE);
				dm->msg_armed = 0;
				machine_no_message(dm, "the message could not be written");
			} else if (dm->machine.rec_frames >= dm->msg_max_frames) {
				/* the caller had the time the setting allows: the message is closed and the call ends here */
				long long bytes = machine_message_finish(&dm->machine);

				if (bytes > 0)
					printf("the answering machine's message was saved: %lld bytes\n", bytes);
				tap_drop(dm, FE_SINK_MACHINE);
				dm->msg_armed = 0;
				if (phone_hangup(dm->phone))
					printf("the call could not be ended at the message's limit\n");
				else
					printf("the answering machine's message reached its limit (%u s): the call is ended\n", dm->cfg.machine_max_s);
			}
		}
		if (fe_owners(dm->fe)->sink & (FE_SINK_FRONT_END | FE_SINK_WEB)) {
			for (k = 0; k < got; k++) {	/* field by field: the two structs are never cast */
				slots[k].modem = frames[k].modem;
				slots[k].line = frames[k].line;
				slots[k].modem2 = frames[k].modem2;
				slots[k].line2 = frames[k].line2;
			}
			fe_tap(dm->fe, slots, got);
		}
	} while (++loops < 32);	/* 32 x 256 frames is the whole ring; more cannot arrive inside one tick */
}

/* ---- the media socket: the far end toward a front-end, its audio toward the line -------------------------------- */

/* The media socket exists while the line's front end runs. A front-end connects, attaches its id to the call revision
 * it knows, and then receives one capture frame per 20 ms and sends its playback frames back. The ring is kept the
 * same small lead ahead of the bridge the greeting keeps: a frame written now is heard about MEDIA_FILL_MS later. */

#define MEDIA_SOCKET_PATH	"/var/run/awvoice-phoned/media.sock"
#define MEDIA_FILL_MS		120	/* the ring stays about this far ahead of the bridge */
#define MEDIA_READS		26	/* a peer's frames taken in one tick: a little over the fill's span */
#define MEDIA_FILL_FRAMES	8	/* a tick writes at most this many frames: the lead is small */

/* play_fill.h places a frame's four fields through a uint16_t stride; the kernel's own order is the one below, so the
   fill lands on the right slots. If the struct ever changes shape this is where the build stops. */
_Static_assert(sizeof(struct awfxs_play_frame) == PLAY_FIELD_STRIDE * sizeof(uint16_t),
	"the playback frame is four uint16_t");
_Static_assert(offsetof(struct awfxs_play_frame, modem) == 0 && offsetof(struct awfxs_play_frame, line) == 2 &&
	offsetof(struct awfxs_play_frame, modem2) == 4 && offsetof(struct awfxs_play_frame, line2) == 6,
	"play_fill.h assumes modem, line, modem2, line2 in that order");

/* the playback ring goes: the device disarmed and the ring's owner released in the same step (the greeting's last
 * frame, the front end's release, the socket closing) */
static void play_release(struct daemon *dm)
{
	if (dm->play_armed) {
		(void)dev_play_set(&dm->dev, 0);
		dm->play_armed = 0;
	}
	fe_play_release(fe_owners(dm->fe));
	play_mirror_reset(&dm->play_mirror);	/* what waited for a slot that never came is not the next call's */
}

/* The health reply names the pid on the other end of the media socket: the first peer that is connected. The pid is
 * the kernel's, from the socket itself, so a front-end that restarts is seen with its new pid. */
static unsigned int media_peer_pid(int fd)
{
	struct ucred cred;
	socklen_t len = sizeof(cred);

	if (fd >= 0 && !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len))
		return (unsigned int)cred.pid;
	return 0;
}

static void media_consumer_update(struct daemon *dm)
{
	unsigned int i;

	dm->api.sip_consumer_pid = 0;
	for (i = 0; i < QMSK_PEERS; i++) {
		if (dm->media_pid[i]) {
			dm->api.sip_consumer_pid = dm->media_pid[i];
			break;
		}
	}
}

/* one 20 ms far-end frame from the cut goes to every peer bound to the call that is up, and to the page while it owns
 * the audio. The wire's frames are 8000 Hz, so a wideband call is cut down on the way; a frame that is neither shape,
 * and a peer that is not reading, are counted. */
static void fe_sink_capture(void *ctx, const struct fe_frame *fr)
{
	struct daemon *dm = ctx;
	struct qmsk_pcm f;
	struct qmsk_binding *b;
	unsigned int i, sent = 0;
	u64 rev;
	int web = dm->web && web_audio_granted(dm->web);

	if (!dm->fe_armed && !web)
		return;
	rev = fe_call_view(dm->fe)->revision;
	f.sequence = fr->sequence;
	f.timestamp_ms = fr->sequence * QMSK_PCM_MS;
	if (fr->rate == 8000 && fr->count == QMSK_PCM_SAMPLES)
		memcpy(f.sample, fr->sample, sizeof(f.sample));
	else if (fr->rate == 16000 && fr->count == 2 * QMSK_PCM_SAMPLES)
		media_rate_down16(fr->sample, f.sample);
	else {
		arbiter_dropped(&dm->arbiter);
		return;
	}
	for (i = 0; dm->fe_armed && i < QMSK_PEERS; i++) {
		b = qmsk_server_peer_binding(&dm->media, i);
		if (!b || b->revision != rev)
			continue;
		if (qmsk_server_send_capture(&dm->media, i, &f))
			arbiter_dropped(&dm->arbiter);
		else
			sent++;
	}
	if (sent)
		arbiter_media(&dm->arbiter, 1, 0);
	if (web)
		web_audio_capture(dm->web, f.sample, QMSK_PCM_SAMPLES, fr->sequence);
}

/* A published change reaches the topic and the front-end's socket, so a consumer that watches the media sees the
 * state when the call does. Every peer that attached hears it, the one whose call has just ended included: the
 * revision in the frame is the call view's, and a binding minted for another one is told the call moved on. */
static void media_state(struct daemon *dm, const struct fe_event *ev)
{
	unsigned int i;

	if (dm->media.fd < 0)
		return;
	for (i = 0; i < QMSK_PEERS; i++) {
		if (!qmsk_server_peer_binding(&dm->media, i))
			continue;
		(void)qmsk_server_send_state(&dm->media, i, fe_state_of(0, ev->phase), (u32)ev->revision);
	}
}

/* The capture tap carries the front end's frames only while a peer holds the call that is up: an armed tap with no
 * peer is work for nothing, and a peer that attached to a call already over is not fed. */
static void media_sinks(struct daemon *dm)
{
	struct qmsk_binding *b;
	unsigned int i;
	u64 rev = fe_call_view(dm->fe)->revision;
	int want = 0, err;

	for (i = 0; i < QMSK_PEERS && !want; i++) {
		b = qmsk_server_peer_binding(&dm->media, i);
		if (b && b->revision == rev)
			want = 1;
	}
	if (want && !dm->fe_armed) {
		err = tap_add(dm, FE_SINK_FRONT_END);
		if (!err) {
			dm->fe_armed = 1;
			if (dm->fe_arm_logged)
				printf("the front end's frames are on the tap\n");
			dm->fe_arm_logged = 0;
		} else if (!dm->fe_arm_logged) {
			printf("the front end's frames cannot reach it (the capture tap: %s)\n", strerror(-err));
			dm->fe_arm_logged = 1;
		}
	} else if (!want && dm->fe_armed) {
		tap_drop(dm, FE_SINK_FRONT_END);
		dm->fe_armed = 0;
	}
}

/* the peer holding a live binding to the call that is up and a frame waiting: the first one, in slot order */
static struct qmsk_binding *media_next(struct daemon *dm, u64 rev)
{
	struct qmsk_binding *b;
	unsigned int i;

	for (i = 0; i < QMSK_PEERS; i++) {
		b = qmsk_server_peer_binding(&dm->media, i);
		if (b && b->revision == rev && b->count)
			return b;
	}
	return NULL;
}

/* The front end's audio toward the line: frames are popped from the peers bound to the call that is up and the ring
 * is kept about MEDIA_FILL_MS ahead of the bridge, as the greeting's is. The wire's frames are 8000 Hz; a wideband
 * call is played two samples per slot, so one wire sample is held for both. The cross to the line is sample by sample:
 * the audio goes to the modem and the handset stays silent, because it is not part of a bridged call. */
static void media_play_feed(struct daemon *dm)
{
	struct awfxs_play_status ps;
	struct awfxs_play_frame frames[QMSK_PCM_SAMPLES];
	struct awfxs_play_write w;
	struct qmsk_binding *b;
	struct qmsk_pcm f;
	const u32 rate = (u32)tap_rate(dm);
	unsigned int slots, loops = 0;
	u64 rev;
	int err;

	if (dm->media.fd < 0 || fe_owners(dm->fe)->play == FE_PLAY_GREETING)
		return;			/* no socket, or the greeting holds the ring: nothing of the front end's plays */
	rev = fe_call_view(dm->fe)->revision;
	if (!media_next(dm, rev))
		return;			/* nothing waits: the ring keeps its lead and runs into silence */
	if (!dm->play_armed) {
		if (fe_play_claim(fe_owners(dm->fe), FE_PLAY_FRONT_END)) {
			arbiter_dropped(&dm->arbiter);	/* the ring has another owner: never mixed */
			return;
		}
		err = dev_play_set(&dm->dev, 1);
		if (err) {
			fe_play_release(fe_owners(dm->fe));
			arbiter_dropped(&dm->arbiter);
			if (!dm->fe_play_logged) {
				printf("the front end's audio cannot be played: %s\n", strerror(-err));
				dm->fe_play_logged = 1;
			}
			return;
		}
		if (!dev_play_status(&dm->dev, &ps))
			dm->play_generation = ps.generation;
		dm->play_armed = 1;
	}
	if (dev_play_status(&dm->dev, &ps) || !ps.armed || ps.generation != dm->play_generation) {
		printf("the playback ring was reset under the front end's audio: it stops\n");
		play_release(dm);
		return;
	}
	slots = play_lead_frames(MEDIA_FILL_MS);	/* the ring counts frames at either band: 8 of them per millisecond */
	while (loops++ < MEDIA_FILL_FRAMES && ps.stored < slots) {
		int16_t wide[2 * QMSK_PCM_SAMPLES];
		const int16_t *src;
		unsigned int n, written;

		b = media_next(dm, rev);
		if (!b || qmsk_binding_pop(b, &f))
			break;
		if (rate == 16000) {
			media_rate_up16(f.sample, wide);
			src = wide;
			n = 2 * QMSK_PCM_SAMPLES;
		} else {
			src = f.sample;
			n = QMSK_PCM_SAMPLES;
		}
		written = play_fill_frames(&frames[0].modem, QMSK_PCM_SAMPLES, src, n, rate == 16000);
		if (!written)
			break;					/* half a frame waits for its pair, as the web feed's does */
		if (dev_play_write(&dm->dev, frames, written, &w) || w.written < written) {
			play_release(dm);
			if (!dm->fe_play_logged) {
				printf("the front end's audio could not be fed to the ring\n");
				dm->fe_play_logged = 1;
			}
			return;
		}
		dm->fe_play_logged = 0;
		/* the same samples wait here for the tap, so the recording carries the SIP party in the handset's channel */
		play_mirror_push(&dm->play_mirror, (const u16 *)src, n);
		arbiter_media(&dm->arbiter, 0, 1);
		if (dev_play_status(&dm->dev, &ps))
			break;
	}
}

/* one connection may wait behind another (a front-end that restarts connects at once): accept until none does */
static void on_media_accept(struct uloop_fd *u, unsigned int events)
{
	unsigned int slot;
	int r;

	(void)u;
	(void)events;
	for (;;) {
		r = qmsk_server_accept(&d.media, &slot);
		if (r == 0)
			break;			/* no connection waits */
		if (r > 0) {
			d.media_pid[slot] = media_peer_pid(d.media.peer[slot].fd);
			media_consumer_update(&d);
			continue;
		}
		if (r == -ENOSPC) {
			printf("the media socket has no free slot: the connection was closed\n");
			continue;		/* the socket went at once: one more may wait */
		}
		printf("the media socket could not take a connection: %s\n", strerror(-r));
		break;
	}
}

/* the media socket joins the loop when the guards allow a front end; a failure is said and the line is untouched */
static void media_open(struct daemon *dm)
{
	int err;

	if (!dm->loop_up || dm->media.fd >= 0)
		return;
	(void)mkdir("/var/run/awvoice-phoned", 0700);	/* the runtime folder also holds the call log; an existing one is fine */
	err = qmsk_server_open(&dm->media, MEDIA_SOCKET_PATH);
	if (err) {
		printf("the media socket did not open at %s: %s\n", MEDIA_SOCKET_PATH, strerror(-err));
		return;
	}
	dm->media_fd.fd = dm->media.fd;
	dm->media_fd.cb = on_media_accept;
	if (uloop_fd_add(&dm->media_fd, ULOOP_READ)) {
		printf("the media socket could not join the event loop: a front end cannot attach\n");
		qmsk_server_close(&dm->media);
		dm->media_fd.fd = -1;
		return;
	}
	printf("the media socket is at %s\n", MEDIA_SOCKET_PATH);
}

/* the mode or the guards took a front end away: the socket and the audio it carried go in the same step */
static void media_close(struct daemon *dm)
{
	unsigned int i;

	if (dm->media.fd < 0)
		return;
	if (dm->media_fd.fd >= 0) {
		uloop_fd_delete(&dm->media_fd);
		dm->media_fd.fd = -1;
	}
	qmsk_server_close(&dm->media);
	for (i = 0; i < QMSK_PEERS; i++)
		dm->media_pid[i] = 0;
	media_consumer_update(dm);
	if (fe_owners(dm->fe)->play == FE_PLAY_FRONT_END)
		play_release(dm);
	if (dm->fe_armed) {
		tap_drop(dm, FE_SINK_FRONT_END);
		dm->fe_armed = 0;
	}
	printf("the media socket is closed\n");
}

/* The media socket's turn in the step: the peers' frames, the bindings' ages, and the tap that carries the far end
 * to the front end. Nothing here blocks; a peer that is gone is dropped, its binding left to its ttl so a reconnect
 * can attach again. */
static void media_tick(struct daemon *dm, unsigned long long now)
{
	unsigned int i;
	int r, k;

	if (dm->media.fd < 0)
		return;
	(void)qmsk_bindings_expire(&dm->media.bindings, (u64)now);
	for (i = 0; i < QMSK_PEERS; i++) {
		if (dm->media.peer[i].fd < 0)
			continue;
		for (k = 0; k < MEDIA_READS; k++) {
			r = qmsk_server_read(&dm->media, i, (u64)now);
			if (r == QMSK_PCM_PLAYBACK) {
				arbiter_media(&dm->arbiter, 0, 1);
				continue;
			}
			if (r == QMSK_ATTACH) {
				const struct fe_call *v = fe_call_view(dm->fe);

				printf("the media socket: a binding attached at revision %llu (%s)\n",
					(unsigned long long)v->revision, fe_state_name(v->state));
				/* the call's state goes back the moment an attach lands: without it the front end never
				 * sees its attach confirmed, keeps retrying, and the call it is waiting for ends unused */
				(void)qmsk_server_send_state(&dm->media, i, v->state, (u32)v->revision);
				continue;
			}
			if (r == QMSK_DETACH || r == QMSK_HEARTBEAT)
				continue;
			if (r == 0)
				break;			/* nothing waits */
			if (r == -ENOSPC) {
				arbiter_dropped(&dm->arbiter);	/* the queue was full: an ERROR went out, the peer stays */
				continue;
			}
			/* -ECONNRESET, or the -EPROTO of a refusal: the peer is gone */
			printf("the media socket: the peer went (%d)\n", r);
			dm->media_pid[i] = 0;
			media_consumer_update(dm);
			break;
		}
	}
	media_sinks(dm);
}

/* ---- the page's browser audio: a TLS stream, its own session, the same rings ------------------------------------- */

struct lan_addr {
	char addr[16];
};

static void lan_status_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct lan_addr *a = req->priv;
	enum { LA_V4, LA_MAX };
	static const struct blobmsg_policy policy[LA_MAX] = {
		[LA_V4] = { .name = "ipv4-address", .type = BLOBMSG_TYPE_ARRAY }
	};
	static const struct blobmsg_policy addr_policy[1] = {
		{ .name = "address", .type = BLOBMSG_TYPE_STRING }
	};
	struct blob_attr *tb[LA_MAX] = { 0 }, *cur;
	size_t rem;

	(void)type;
	if (a->addr[0])
		return;
	blobmsg_parse(policy, LA_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[LA_V4])
		return;
	blobmsg_for_each_attr(cur, tb[LA_V4], rem) {
		struct blob_attr *atb[1] = { 0 };

		blobmsg_parse(addr_policy, 1, atb, blobmsg_data(cur), blobmsg_data_len(cur));
		if (atb[0]) {
			snprintf(a->addr, sizeof(a->addr), "%s", blobmsg_get_string(atb[0]));
			return;
		}
	}
}

/* the configured network's own IPv4 address, from the network package's own status */
static int lan_ipv4(struct daemon *dm, char *out, size_t cap)
{
	struct lan_addr a;
	struct blob_buf b = { 0 };
	char obj[40];
	uint32_t id;

	out[0] = 0;
	if (!dm->api.ubus || !dm->api.connected)
		return -ENODEV;
	snprintf(obj, sizeof(obj), "network.interface.%s", dm->cfg.web_iface);
	if (ubus_lookup_id(dm->api.ubus, obj, &id))
		return -ENODEV;
	memset(&a, 0, sizeof(a));
	blob_buf_init(&b, 0);
	(void)ubus_invoke(dm->api.ubus, id, "status", NULL, lan_status_cb, &a, 1000);
	blob_buf_free(&b);
	if (!a.addr[0] || strlen(a.addr) >= cap)
		return -ENODEV;
	snprintf(out, cap, "%s", a.addr);
	return 0;
}

/* the page's session ended (BYE, a closed stream, a timeout, the call's end): the ring and the tap go with it, and the
 * handset gets its own audio back on the next frame */
static void web_release(void *ctx)
{
	struct daemon *dm = ctx;

	if (dm->web_armed) {
		tap_drop(dm, FE_SINK_WEB);
		dm->web_armed = 0;
	}
	if (fe_owners(dm->fe)->play == FE_PLAY_WEB)
		play_release(dm);
	printf("the page's audio is released\n");
}

/* A page presented a token for the call it saw: the one-owner ladder decides. The call must still be the front end's,
 * because the page is a controller of this daemon and its microphone replaces the handset's transmit slots. */
static int web_grant(void *ctx, uint64_t revision)
{
	struct daemon *dm = ctx;
	const struct fe_call *v = fe_call_view(dm->fe);
	struct awfxs_play_status ps;
	int err;

	if (dm->guard.refusal != FE_REFUSE_NONE)
		return -EPERM;
	if (!v->id || v->state != FE_STATE_ACTIVE)
		return -ENOENT;
	if (v->owner != FE_ANSWER_WEB)
		return -EBUSY;		/* the handset or the machine has the call: the page's microphone must not take it */
	if (revision != v->revision)
		return -ESTALE;
	if (fe_play_claim(fe_owners(dm->fe), FE_PLAY_WEB))
		return -EBUSY;
	err = dev_play_set(&dm->dev, 1);
	if (err) {
		fe_play_release(fe_owners(dm->fe));
		return err == -EBUSY ? -EBUSY : -EIO;
	}
	dm->play_armed = 1;
	if (!dev_play_status(&dm->dev, &ps))
		dm->play_generation = ps.generation;
	err = tap_add(dm, FE_SINK_WEB);
	if (err) {
		play_release(dm);
		return -EIO;
	}
	dm->web_armed = 1;
	printf("the page's audio has the line\n");
	return 0;
}

/* The call stopped being the page's (a handset take-over, the end): the microphone must stop with it, or the caller
 * would keep hearing a browser nobody is watching. */
static void web_watch(struct daemon *dm)
{
	const struct fe_call *v = fe_call_view(dm->fe);

	if (!dm->web || !web_audio_granted(dm->web))
		return;
	if (!v->id || v->state != FE_STATE_ACTIVE || v->owner != FE_ANSWER_WEB) {
		printf("the page's audio stops: the call is no longer the page's\n");
		web_audio_drop(dm->web);
	}
}

/* the page's voice toward the line, the same fill rule the SIP front end keeps: the wire's 8 kHz frame becomes one ring
 * frame per sample at 8 kHz and one ring frame per *pair* at 16 kHz, and the same samples wait in the mirror so the
 * recording's second channel carries the page's party */
static void web_play_feed(struct daemon *dm)
{
	struct awfxs_play_status ps;
	struct awfxs_play_frame frames[WEB_SAMPLES];
	struct awfxs_play_write w;
	int16_t mono[WEB_SAMPLES], wide[2 * WEB_SAMPLES];
	const u32 rate = (u32)tap_rate(dm);
	unsigned int slots = play_lead_frames(MEDIA_FILL_MS), loops = 0;

	if (!dm->web_armed || fe_owners(dm->fe)->play != FE_PLAY_WEB)
		return;
	if (dev_play_status(&dm->dev, &ps) || !ps.armed || ps.generation != dm->play_generation) {
		printf("the playback ring was reset under the page's audio: it stops\n");
		web_audio_drop(dm->web);
		return;
	}
	while (loops++ < MEDIA_FILL_FRAMES && ps.stored < slots) {
		const int16_t *src;
		unsigned int n, written;

		if (!web_audio_take_playback(dm->web, mono, WEB_SAMPLES))
			break;
		if (rate == 16000) {
			media_rate_up16(mono, wide);
			src = wide;
			n = 2 * WEB_SAMPLES;
		} else {
			src = mono;
			n = WEB_SAMPLES;
		}
		written = play_fill_frames(&frames[0].modem, WEB_SAMPLES, src, n, rate == 16000);
		if (dev_play_write(&dm->dev, frames, written, &w) || w.written < written) {
			printf("the page's audio could not be fed to the ring\n");
			web_audio_drop(dm->web);
			return;
		}
		play_mirror_push(&dm->play_mirror, (const u16 *)src, n);
		if (dev_play_status(&dm->dev, &ps))
			break;
	}
}

static void web_close(struct daemon *dm);

static void on_web_accept(struct uloop_fd *u, unsigned int events)
{
	(void)u;
	(void)events;
	if (!d.web)
		return;
	(void)web_audio_accept(d.web, mono_ms());
}

static void web_open(struct daemon *dm)
{
	struct web_audio_ops ops = { .ctx = dm, .grant = web_grant, .release = web_release };
	char was[sizeof(dm->web_reason)];
	struct stat sc, sk;
	char addr[16];
	int have, err;

	if (!dm->loop_up)
		return;
	snprintf(was, sizeof(was), "%s", dm->web_reason);
	snprintf(dm->web_reason, sizeof(dm->web_reason), "waiting");
	if (!dm->api.ubus || !dm->api.connected)
		return;			/* the ubus connection runs the guards again, and this opens then */
	err = lan_ipv4(dm, addr, sizeof(addr));
	if (err) {
		if (dm->web)
			return;		/* the listener keeps the address it was bound to */
		snprintf(dm->web_reason, sizeof(dm->web_reason), "no LAN address");
		if (strcmp(was, dm->web_reason))
			printf("the page's audio waits: %s has no IPv4 address\n", dm->cfg.web_iface);
		return;
	}
	have = stat(dm->cfg.web_cert, &sc) == 0 && stat(dm->cfg.web_key, &sk) == 0;
	if (dm->web) {
		/* the same listener: the address, the port and the certificate pair all unchanged */
		if (have && (long)sc.st_mtime == dm->web_cert_mtime && (long)sc.st_size == dm->web_cert_size &&
		    (long)sk.st_mtime == dm->web_key_mtime && (long)sk.st_size == dm->web_key_size &&
		    dm->cfg.web_port == dm->web_port_used && !strcmp(addr, dm->web_addr))
			return;
		web_close(dm);		/* the address, the port or the certificate moved: a new listener */
	}
	err = web_audio_open(&dm->web, &ops, addr, dm->cfg.web_port, dm->cfg.web_cert, dm->cfg.web_key, stdout);
	if (err) {
		snprintf(dm->web_reason, sizeof(dm->web_reason), "not listening");
		if (strcmp(was, dm->web_reason))
			printf("the page's audio listener did not open: %s\n", strerror(-err));
		return;
	}
	dm->web_fd.fd = web_audio_fd(dm->web);
	dm->web_fd.cb = on_web_accept;
	if (uloop_fd_add(&dm->web_fd, ULOOP_READ)) {
		printf("the page's audio listener could not join the event loop\n");
		web_audio_close(&dm->web);
		dm->web_fd.fd = -1;
		snprintf(dm->web_reason, sizeof(dm->web_reason), "not listening");
		return;
	}
	snprintf(dm->web_addr, sizeof(dm->web_addr), "%s", addr);
	dm->web_port_used = dm->cfg.web_port;
	if (have) {
		dm->web_cert_mtime = (long)sc.st_mtime;
		dm->web_cert_size = (long)sc.st_size;
		dm->web_key_mtime = (long)sk.st_mtime;
		dm->web_key_size = (long)sk.st_size;
	}
	snprintf(dm->web_reason, sizeof(dm->web_reason), "listening");
	printf("the page's audio listener is at wss://%s:%u (one page at a time)\n", addr, dm->cfg.web_port);
}

static void web_close(struct daemon *dm)
{
	if (!dm->web)
		return;
	if (dm->web_fd.fd >= 0) {
		uloop_fd_delete(&dm->web_fd);
		dm->web_fd.fd = -1;
	}
	web_audio_close(&dm->web);	/* a granted session tells web_release: the ring and the tap go */
	snprintf(dm->web_reason, sizeof(dm->web_reason), "off");
	printf("the page's audio listener is closed\n");
}

/* ---- the step ------------------------------------------------------------------------------------------------------- */

/* The call's public view, as the front end hears of it: the line, the call, who answered it and how long it has run.
 * The number stays out; the daemon keeps no separate withheld flag, so the view carries none. A release carries why it
 * ended: the front end's own hangup or reject when it asked for one, else the handset's own reason. */
static enum fe_answer_owner fe_owner_of(enum phone_answered_by who)
{
	switch (who) {
	case PHONE_ANSWERED_HANDSET:
		return FE_ANSWER_HANDSET;
	case PHONE_ANSWERED_MACHINE:
		return FE_ANSWER_MACHINE;
	case PHONE_ANSWERED_WEB:
		return FE_ANSWER_WEB;
	case PHONE_ANSWERED_SIP:
		return FE_ANSWER_SIP;
	case PHONE_ANSWERED_NONE:
		break;
	}
	return FE_ANSWER_NONE;
}

static void fe_observe_line(struct daemon *dm, enum phone_line line, enum call_state call, enum phone_dir dir, unsigned int call_id,
	enum phone_answered_by who, unsigned int duration_s)
{
	enum fe_answer_owner owner = fe_owner_of(who);
	enum fe_reason reason = FE_REASON_NONE;

	if (line == PHONE_LINE_IDLE) {
		reason = arbiter_take_reason(&dm->arbiter);	/* the front end's own hangup or reject, when it asked for one */
		if (reason == FE_REASON_NONE && fe_call_view(dm->fe)->owner == FE_ANSWER_HANDSET)
			reason = FE_REASON_HANDSET;	/* the handset was put down: that is why the call ended */
	}
	(void)fe_observe(dm->fe, line, call, dir, call_id, owner, 0, duration_s, reason);
}

/* the front end's events, out on qmodem_voip.call; the capture frames are the media socket's (qmsk_server.h) */
static void fe_sink_event(void *ctx, const struct fe_event *ev)
{
	struct daemon *dm = ctx;

	/* quiet hours: the page still sees the call and can take it, and the handset can be lifted to answer it,
	   but no network phone is invited to ring - the consumer sends its INVITE on this one event */
	if (!(ev->event == FE_EV_RING && dm->quiet))
		api_sip_event(&dm->api, ev);
	if (ev->event == FE_EV_ACTIVE)
		api_voip_answer_settle(&dm->api, 1);	/* the installed service's held answer goes now: it can attach */
	else if (ev->event == FE_EV_RELEASE || ev->event == FE_EV_FAULT)
		api_voip_answer_settle(&dm->api, 0);
	media_state(dm, ev);
	if (ev->event == FE_EV_RELEASE) {
		/* one-way audio is otherwise invisible: the call looks fine while only one direction carries frames */
		printf("the call's media: %u frames to the front end, %u from it\n",
			dm->arbiter.capture_frames, dm->arbiter.playback_frames);
		if (fe_owners(dm->fe)->play == FE_PLAY_FRONT_END)
			play_release(dm);	/* the call is over: the front end's audio stops in the same step */
		if (dm->web && web_audio_granted(dm->web))
			web_audio_drop(dm->web);	/* the page's stream ends with the call it belonged to */
	}
}

static const struct fe_sink fe_sink = { .ctx = &d, .event = fe_sink_event, .capture = fe_sink_capture };

/* the qmodem_voip methods: the same paths the handset's keys take, and the line's own status for the number */
static int ctl_answer(void *ctx, int via_sip)
{
	struct daemon *dm = ctx;

	return dm->phone ? phone_answer(dm->phone, via_sip ? PHONE_ANSWERED_SIP : PHONE_ANSWERED_WEB) : -ENODEV;
}

static int ctl_takeover(void *ctx, int via_sip)
{
	struct daemon *dm = ctx;

	return dm->phone ? phone_takeover(dm->phone, via_sip ? PHONE_ANSWERED_SIP : PHONE_ANSWERED_WEB) : -ENODEV;
}

static int ctl_dial(void *ctx, const char *number, int via_sip)
{
	struct daemon *dm = ctx;

	return dm->phone ? phone_dial(dm->phone, number, via_sip ? PHONE_ANSWERED_SIP : PHONE_ANSWERED_WEB) : -ENODEV;
}

static int ctl_hangup(void *ctx)
{
	struct daemon *dm = ctx;

	return dm->phone ? phone_hangup(dm->phone) : -ENODEV;
}

static int ctl_dtmf(void *ctx, char digit)
{
	struct daemon *dm = ctx;

	return dm->phone ? phone_dtmf(dm->phone, digit) : -ENODEV;
}

static int ctl_flash(void *ctx)
{
	struct daemon *dm = ctx;

	return dm->phone ? phone_flash(dm->phone) : -ENODEV;
}

static void ctl_status(void *ctx, struct phone_status *out)
{
	struct daemon *dm = ctx;

	memset(out, 0, sizeof(*out));
	if (dm->phone)
		phone_status(dm->phone, out);
}

static u64 ctl_now(void *ctx)
{
	(void)ctx;
	return (u64)mono_ms();
}

/* What the line's front end and the SIP feature each follow. The line's side (the front end, the media socket, the
 * page's audio) is the daemon's own work: it runs while the guards leave the line to it, whatever the mode. The SIP
 * feature is the qmodem_voip object and the installed service behind it: it exists only in a mode that serves one,
 * and never while another owner has the name. The media socket needs the event loop, so it opens on the next run
 * after the loop exists. */
static void sip_apply(struct daemon *dm)
{
	enum sip_mode mode = sip_mode_of(dm->cfg.sip_mode);
	int line_ok = dm->guard.refusal == FE_REFUSE_NONE;
	int sip_on = line_ok && sip_mode_feature(mode) && !dm->guard.name_taken;
	int held, moved;

	arbiter_set_off(&dm->arbiter, !line_ok);
	fe_set_off(dm->fe, !line_ok);
	/* the handover of the qmodem_voip name is said when it happens, because it is what a SIP service binds itself to
	 * at its own start: a call that arrives with no audio is a call whose service started on the wrong side of this
	 * line, and the one line in the log that says which side that was is what makes it findable */
	held = dm->api.sip_registered;
	if (api_sip_set(&dm->api, sip_on)) {
		printf("the qmodem_voip object could not follow the mode and the guards\n");
	} else if ((moved = sip_handover(held, dm->api.sip_registered)) > 0) {
		printf("the qmodem_voip name is this daemon's now (%s): a service started from here on finds it\n",
			sip_mode_name(mode));
	} else if (moved < 0) {
		printf("the qmodem_voip name is released (%s): a service still running is answering nothing\n",
			sip_mode_name(mode));
	}
	if (line_ok) {
		media_open(dm);
		web_open(dm);
	} else {
		web_close(dm);
		media_close(dm);
	}
	dm->api.web = dm->web;
	dm->api.web_port = dm->cfg.web_port;
	dm->api.web_addr = dm->web ? dm->web_addr : "";
	dm->api.sip_web_audio = dm->web_reason;
}

/* an init script enabled at boot: an S<nn><name> link in /etc/rc.d (the service that owns qmodem_voip when it is on) */
static int service_enabled(const char *name)
{
	const char *p;
	struct dirent *e;
	DIR *dir = opendir("/etc/rc.d");
	int found = 0;

	while (dir && (e = readdir(dir)) != NULL) {
		p = e->d_name;
		if (p[0] != 'S' || !isdigit((unsigned char)p[1]))
			continue;
		for (p += 2; isdigit((unsigned char)*p); p++)
			;
		if (!strcmp(p, name)) {
			found = 1;
			break;
		}
	}
	if (dir)
		closedir(dir);
	return found;
}

/* qmodem_voip is registered by someone else: this daemon's own registration does not count (the name is looked up only while this one's is not
 * registered, so the answer is always about another owner) */
static int qmodem_taken(struct api_ctx *a)
{
	uint32_t id;

	if (a->sip_registered || !a->ubus || !a->connected)
		return 0;		/* this daemon's, or no connection on which anyone could have registered it */
	return ubus_lookup_id(a->ubus, SIP_OBJECT, &id) == 0;
}

/* Gather what the guards look at and let sip_guard.c decide. The checks are cheap but touch /proc and ubus, so they
 * run at start, on a reload, when the voice port appears or goes, and on the slow re-check; never per step. The line
 * and the name are said separately, and only when they change: a repeated run says nothing new. */
static void sip_guards_run(struct daemon *dm)
{
	struct sip_guard_facts f;
	struct sip_guard_state was = dm->guard;
	enum sip_mode mode = sip_mode_of(dm->cfg.sip_mode);
	char comm[24] = "";
	int pid = 0;

	memset(&f, 0, sizeof(f));
	f.object_owned = qmodem_taken(&dm->api);
	f.service_enabled = !f.object_owned && service_enabled("voipd");
	if (dm->ans.port[0] && port_holder("/proc", dm->ans.port, (int)getpid(), &pid, comm, sizeof(comm)) == 1) {
		f.port_held = 1;
		f.port_pid = pid;
		snprintf(f.port_comm, sizeof(f.port_comm), "%s", comm);
	}
	(void)sip_guard_check(&f, &dm->guard);
	if (dm->guard.refusal != was.refusal) {
		switch (dm->guard.refusal) {
		case FE_REFUSE_PORT:
			printf("the line's front end waits: pid %d (%s) holds the voice port\n", f.port_pid, f.port_comm[0] ? f.port_comm : "?");
			break;
		case FE_REFUSE_NONE:
			printf("the line's front end runs (%s): the voice port is free\n", sip_mode_name(mode));
			break;
		default:
			break;		/* the pre-check word is never a check's answer */
		}
	}
	if (dm->guard.name_taken != was.name_taken) {
		if (dm->guard.name_taken)
			printf("the SIP feature waits: %s owns the qmodem_voip name\n", dm->guard.object_holder);
		else
			printf("the qmodem_voip name is free: the SIP feature follows the mode\n");
	}
	sip_apply(dm);
}

/* The slow re-check: a port that frees up, a name that frees up, a registration that lost its race, or a listener
 * that could not open yet (its certificate or its LAN address appearing late) has a way back without a reload. When
 * nothing is due the timer costs the event loop a wake-up a minute and touches nothing else. */
#define GUARD_RECHECK_MS	60000

static void guard_recheck(struct uloop_timeout *t)
{
	enum sip_mode mode = sip_mode_of(d.cfg.sip_mode);

	if (sip_recheck_due(d.guard.refusal, mode, d.api.sip_registered, d.web != NULL))
		sip_guards_run(&d);
	uloop_timeout_set(t, GUARD_RECHECK_MS);
}

/*
 * The account file a scanned code points a soft phone at carries the account's password, and the page promises it
 * stops being served when its lifetime is through. The file writer has run and exited by then, and the web server
 * that serves the folder knows nothing of lifetimes, so this round keeps that promise whether or not anyone asks for
 * another code. A folder that is not there - the page not installed, or nothing ever scanned - is one call and
 * nothing else; the rules themselves are in provision.h, where the host tests reach them whole.
 */
#define PROVISION_SWEEP_MS	60000

static void provision_recheck(struct uloop_timeout *t)
{
	int retired = provision_prune(PROVISION_DIR, time(NULL), PROVISION_TTL_SEC);

	if (retired > 0)
		printf("the scanned code's account files were retired (%d past their lifetime)\n", retired);
	uloop_timeout_set(t, PROVISION_SWEEP_MS);
}

/*
 * The quiet hours read anew from the settings. What the handset's code asked for, or the page's switch, is the
 * line's own state and not a setting: it outlives a save of anything else, and only the code that undoes it changes
 * it. A restart falls back to the windows, which are written down and stay written.
 *
 * The phone book is handed over with it, because the rule set is read from nothing each time. A contact that asks to
 * ring through stays a contact: the quiet hours ask the book who that caller is instead of holding a copy of the
 * number, so editing or deleting one cannot leave a stranger ringing through.
 */
static void dnd_reload(struct daemon *dm)
{
	int asked = dm->dnd.manual;

	config_load_dnd(&dm->dnd, dm->region, stdout);
	dm->dnd.manual = asked;
	dm->dnd.book = &dm->book;
}

/*
 * Which numbering plan the line reads numbers with: `region` may name one, or ask for the one the registered network
 * belongs to. A change is a settings change like any other - it takes hold when the line is idle - and the lists that
 * were read against the old plan (the phonebook's numbers, the blocking rules, the emergency numbers with them) are
 * read again with the new one, because a rule written as a national number means something else under another plan.
 */
static void region_follow(struct daemon *dm, const struct phone_status *st)
{
	const struct number_region *r;
	struct phone_cfg pc;
	int first_read = !dm->region_mcc[0] && st->mcc[0];	/* the first network the line ever reported */

	if (!strcmp(dm->region_mcc, st->mcc))
		return;					/* the questions come round every couple of seconds: only a new network is worth the walk */
	snprintf(dm->region_mcc, sizeof(dm->region_mcc), "%s", st->mcc);
	r = number_region_find(region_choice(dm->cfg.region, st->mcc, dm->region ? dm->region->name : NULL));
	if (!r)
		r = number_region_default();
	if (!region_changed(dm->region ? dm->region->name : NULL, r->name)) {
		/* which network the line ended up on, said once per registration: without it the read leaves no trace when
		 * the network is the one the plan already fits (a Sri Lankan SIM on the shipped plan) */
		if (first_read)
			printf("the line is registered on network %s: the %s numbering plan applies\n", dm->region_mcc, r->name);
		return;
	}
	dm->region = r;
	dm->api.region = r->name;
	config_load_blocking(&dm->block, r, stdout);
	dnd_reload(dm);
	config_load_book(&dm->book, r, stdout);
	phone_cfg_of(&dm->cfg, &pc);
	if (phone_set_cfg(dm->phone, &pc))
		printf("the line would not take the %s numbering plan: it is offered again at the next save\n", r->name);
	else
		printf("the line reads numbers with the %s plan (network %s)\n", r->name, dm->region_mcc);
}

/*
 * The clock the quiet hours read. The core never asks the time itself: the daemon fills these few fields and the
 * windows are judged against them. `known` is not a guess about the modem or the network - an epoch below the floor
 * in dnd.h is the only honest sign that the time has not arrived yet. Once a second is enough of a rate: a window's
 * two edges are whole minutes, and the local time comes from localtime_r, which reads the zone the router holds
 * (/etc/TZ, the system config's zone).
 */
static void dnd_clock(struct daemon *dm, unsigned long long now_ms)
{
	time_t when;
	struct tm local;

	if (dm->dnd_clock_ms && now_ms - dm->dnd_clock_ms < 1000)
		return;
	dm->dnd_clock_ms = now_ms;
	when = time(NULL);
	dm->dnd.now.epoch_s = (uint64_t)when;
	dm->dnd.now.clock_known = dnd_clock_known((uint64_t)when);
	if (!dm->dnd.now.clock_known)
		return;
	if (!localtime_r(&when, &local))
		return;
	dm->dnd.now.minute = local.tm_hour * 60 + local.tm_min;
	dm->dnd.now.weekday = local.tm_wday;
}

static void on_timer(struct uloop_fd *u, unsigned int events)
{
	uint64_t expired;
	unsigned long long now;
	int err;

	(void)events;
	if (read(u->fd, &expired, sizeof(expired)) != (ssize_t)sizeof(expired))
		return;		/* nothing to read: another source woke the loop */
	/* more than one expiry = steps were missed: the core's own gap record logs them */
	now = mono_ms();
	dnd_clock(&d, now);		/* the windows are judged against this tick's time, not the last one's */
	err = phone_step(d.phone, now);
	if (err) {
		printf("the line stopped: %s\n", strerror(-err));
		d.exit_code = 1;
		uloop_end();
		return;
	}
	modem_watch(now);
	if (d.phone) {
		int want;

		if (phone_layout_due(d.phone, &want))
			layout_rebuild(&d, want);		/* the modem confirmed the rate on a quiet line: the layout moves now */
	}
	if (d.phone) {
		struct phone_status st;

		phone_status(d.phone, &st);
		indicator_step(&d.ind, &st, now);	/* the phone LED follows the line (the patterns, indicator.c) */
		region_follow(&d, &st);
		d.quiet = st.quiet;		/* the front end's ring is withheld while this says the line is quiet */
		fe_observe_line(&d, st.line, st.call, st.dir, st.call_id, st.answered_by,
			st.answered_ms && now > st.answered_ms ? (unsigned int)((now - st.answered_ms) / 1000) : 0);
	}
	if (d.msg_call)
		machine_play_feed(&d);			/* the greeting toward the caller, then the silence */
	tap_tick(&d, now);				/* one drain: the recorder, the machine's message and the front end */
	media_tick(&d, now);				/* the front end's frames in, and the tap they need */
	media_play_feed(&d);				/* the front end's audio toward the line */
	web_watch(&d);					/* the page's audio ends with the call that was its own */
	web_audio_tick(d.web, now);			/* the page's stream: handshake, frames, timeouts */
	web_play_feed(&d);				/* the page's voice toward the line */
	api_tick(&d.api);
	if (d.calls.ram_dirty || d.calls.flash_dirty) {
		struct phone_status st;

		phone_status(d.phone, &st);		/* files are written only while nobody uses the line: a flash write may take a moment */
		if (st.line == PHONE_LINE_IDLE && !st.off_hook && calllog_persist(&d.calls, now, 0))
			printf("the call log could not be saved\n");
	}
	if (d.greeting_stale && !d.msg_call)
		machine_retry(&d);
}

static int timer_start(struct uloop_fd *u)
{
	const struct itimerspec every = {
		.it_interval = { .tv_nsec = HOOK_POLL_MS * 1000000L },
		.it_value = { .tv_nsec = HOOK_POLL_MS * 1000000L },
	};
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

	if (fd < 0)
		return -errno;
	if (timerfd_settime(fd, 0, &every, NULL)) {
		int err = -errno;

		close(fd);
		return err;
	}
	u->fd = fd;
	u->cb = on_timer;
	return uloop_fd_add(u, ULOOP_READ) ? -EIO : 0;
}

/* ---- signals: read from a signalfd inside the loop, nothing happens in a handler ------------------------------------ */

static int reload(void)
{
	struct aw_config now;
	struct phone_cfg pc;
	int web_changed;

	config_load(&now, stdout);
	if (now.channel != d.cfg.channel || strcmp(now.board, d.cfg.board)) {
		printf("the channel or the board file changed: the line is set up again\n");
		d.exit_code = 0;
		uloop_end();
		return 0;
	}
	/* a moved port or certificate is a new listener: the old one goes before the guards open the next */
	web_changed = now.web_port != d.cfg.web_port || strcmp(now.web_cert, d.cfg.web_cert) || strcmp(now.web_key, d.cfg.web_key);
	if (now.at_interface != d.cfg.at_interface) {
		if (d.ans_open)
			modem_close();
		d.port_next_ms = 0;
	}
	d.cfg = now;
	if (web_changed && d.web)
		web_close(&d);
	if (machine_refresh(&d) == -EBUSY) {
		/* the old greeting stays loaded until the machine's call is over and the line is quiet (the timer loads the
		 * new one then) */
		d.greeting_stale = 1;
		printf("the line is busy or the greeting is in use: the new greeting is loaded when the line is idle\n");
	} else {
		d.greeting_stale = 0;
	}
	/* the setting may have moved between auto and a named plan: the plan in force is worked out again, and the network
	 * code is forgotten so that auto takes the current network at once instead of waiting for it to change */
	d.region = number_region_find(region_choice(now.region, "", d.region ? d.region->name : NULL)) ?: number_region_default();
	d.api.region = d.region->name;
	d.region_mcc[0] = 0;
	d.api.hd = now.hd ? (d.phone && phone_hd_active(d.phone) ? 2 : 1) : 0;
	d.api.layout = d.hd_layout;
	indicator_apply(&d.ind, now.led, 0, 0, stdout);	/* only a change acts */
	config_load_blocking(&d.block, d.region, stdout);
	dnd_reload(&d);
	config_load_alarms(&d.alarms, stdout);
	config_load_forward(&d.forward, stdout);
	config_load_book(&d.book, d.region, stdout);		/* between two steps: the core sees the new book whole */
	d.api.at_interface = now.at_interface;
	d.api.sip_mode = sip_mode_name(sip_mode_of(d.cfg.sip_mode));
	sip_guards_run(&d);		/* the guards decide the line, the mode the object: one run applies both */
	/* d.cfg's own copies, never `now`'s: the API keeps these pointers past this return, and `now` is a stack local
	 * (pointing at it emptied the recordings and messages lists after every reload) */
	d.api.record_path = d.cfg.record_path;
	d.api.message_path = d.cfg.machine_path;
	d.api.machine_delay_s = d.cfg.machine_delay_s;
	storage_measure(&d);		/* the folders follow the new settings, and the health reply with them */
	phone_cfg_of(&now, &pc);
	call_ops_of(&d);			/* caller id on or off takes hold at the next ring */
	if (phone_set_cfg(d.phone, &pc))
		printf("the new settings were refused\n");
	else
		printf("settings reloaded (they take hold when the line is idle)\n");
	return 0;
}

static void on_signal(struct uloop_fd *u, unsigned int events)
{
	struct signalfd_siginfo si;

	(void)events;
	if (read(u->fd, &si, sizeof(si)) != (ssize_t)sizeof(si))
		return;
	if (si.ssi_signo == SIGHUP) {
		reload();
		return;
	}
	printf("stopping (signal %u)\n", si.ssi_signo);
	uloop_end();
}

static int signals_start(struct uloop_fd *u)
{
	sigset_t set;
	int fd;

	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGHUP);
	if (sigprocmask(SIG_BLOCK, &set, NULL))
		return -errno;
	signal(SIGPIPE, SIG_IGN);		/* a modem port that went away must not end the process */
	fd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
	if (fd < 0)
		return -errno;
	u->fd = fd;
	u->cb = on_signal;
	return uloop_fd_add(u, ULOOP_READ) ? -EIO : 0;
}

/* ---- the hooks of the core and the callbacks of the ubus object: both get the daemon as their context ------------------ */

static int hook_bridge(void *ctx, unsigned int modem_rx, unsigned int modem_tx, unsigned int line_rx, unsigned int line_tx, unsigned int flags)
{
	struct daemon *dm = ctx;

	return dev_bridge_start(&dm->dev, modem_rx, modem_tx, line_rx, line_tx, flags);
}

/*
 * The audio path's counters around a call, one line when the call is answered and one when it ends (a read of the kernel
 * bridge's counters, twice per call, nothing in the 5 ms step): whether the bridge lost sync during a call decides how
 * a complaint about the voice is looked at. The numbers are the module's own counters, never a phone number.
 */
static void audio_note(struct daemon *dm, const struct phone_event *ev)
{
	struct awfxs_bridge_stats bs;
	const int answered = ev->call == CALL_ACTIVE, ended = ev->call == CALL_GONE || ev->call == CALL_NONE;

	if (!answered && !ended)
		return;
	if (ended && dm->audio_call != ev->call_id)
		return;				/* the call was never answered, or this end was already noted */
	memset(&bs, 0, sizeof(bs));
	if (dev_bridge_stats(&dm->dev, &bs))
		return;
	if (answered) {
		dm->audio_call = ev->call_id;
		dm->audio_base = bs;
		return;
	}
	printf("call audio: %llu frames, %u resyncs, %u transmit skips, longest service gap since start %u us\n",
		(unsigned long long)(bs.frames - dm->audio_base.frames), bs.resyncs - dm->audio_base.resyncs,
		bs.tx_skipped - dm->audio_base.tx_skipped, bs.max_gap_us);
	dm->audio_call = 0;
}

/* a finished call goes into the call log (numbers live there only); the files are written from the timer below, never from inside the step */
static void hook_call_ended(void *ctx, const struct phone_call_record *rec)
{
	struct daemon *dm = ctx;

	/* the recording belongs to this call only when it was this call's tap that armed it
	 * (the ids are the core's own, so the match is exact) */
	const char *rec_name = dm->rec_call_id == rec->id && dm->rec_name[0] ? dm->rec_name : NULL;
	const char *msg_name = NULL;

	/* a call list that drops the call without the gone step closes the record before the machine's ENDED event:
	 * the message is finished here, so the entry names the file it kept */
	if (dm->machine.recording && dm->machine.call_id == rec->id)
		(void)machine_message_finish(&dm->machine);
	if (dm->machine.saved && dm->machine.call_id == rec->id)
		msg_name = dm->machine.name;
	calllog_add(&dm->calls, rec, api_epoch_s(rec->start_ms, mono_ms()), rec_name, msg_name);
	dm->machine.saved = 0;
	dm->machine.name[0] = 0;
	dm->machine.call_id = 0;
}

/* ---- call recording (awvoice.record.*): the files' names hold an epoch and the call's id, never a number ------------- */

/* ---- the folders the files land in (awvoice.record_path, awvoice.machine_path) --------------------------------------- */

/* the folders are made and proven before they are needed: at start-up, on every reload, and again before a call
 * starts a file, so a mount that appears (or goes) later is seen without a reload. A folder that cannot take a
 * file is said with the consequence, never left to a silent failure at the call. */
static struct reply_storage reply_store_of(const struct storage_state *st)
{
	return (struct reply_storage){ .ready = st->ready, .err = st->err, .free_mb = st->free_mb };
}

/* why a folder was refused, in words for the log */
static const char *storage_reason(const struct storage_state *st)
{
	return st->err == ENODEV ? "the drive is not mounted" : strerror(st->err);
}

static void storage_report(const char *what, const char *dir, const struct storage_state *st, const char *if_fails)
{
	if (st->ready && st->free_mb >= STORAGE_MIN_FREE_MB) {
		printf("the %s folder is ready: %s (%llu MB free)\n", what, dir, st->free_mb);
	} else if (st->ready) {
		printf("the %s folder is nearly full: %s (%llu MB free); %s\n", what, dir, st->free_mb, if_fails);
	} else {
		printf("the %s folder cannot be used: %s (%s); %s\n", what, dir, storage_reason(st), if_fails);
	}
}

#define MOUNT_MAX	32
static char mount_paths[MOUNT_MAX][STORAGE_PATH_MAX];
static const char *mount_list[MOUNT_MAX];

/* the mount points as /proc/mounts gives them (the field after the device); anything else is skipped */
static unsigned int mount_read(void)
{
	FILE *f = fopen("/proc/mounts", "r");
	char line[STORAGE_PATH_MAX + 96];
	unsigned int n = 0;

	while (f && n < MOUNT_MAX && fgets(line, sizeof(line), f)) {
		char *dev = strchr(line, ' '), *end = dev ? strchr(dev + 1, ' ') : NULL;
		size_t len;

		if (!dev || !end || strstr(line, "\\040"))	/* a space in the mount point: never a folder this program uses */
			continue;
		*end = 0;
		len = strlen(dev + 1);
		if (!len || len >= STORAGE_PATH_MAX || dev[1] != '/')
			continue;
		memcpy(mount_paths[n], dev + 1, len + 1);
		mount_list[n] = mount_paths[n];
		n++;
	}
	if (f)
		fclose(f);
	return n;
}

/* a folder under /mnt belongs to a drive that must be mounted: without it the folder would be made on the router's
 * own flash, so the drive list is read at every check and the folder refused until the drive is there */
static int storage_unmounted(const char *dir)
{
	const char *m;

	if (strncmp(dir, "/mnt", 4) || (dir[4] && dir[4] != '/'))
		return 0;
	m = storage_mount_of(dir, mount_list, mount_read());
	return !m || (m[0] == '/' && !m[1]);
}

/* prepare, but never under an unmounted /mnt: nothing is created there */
static void storage_check(const char *dir, struct storage_state *st)
{
	memset(st, 0, sizeof(*st));
	if (storage_unmounted(dir))
		st->err = ENODEV;
	else
		storage_prepare(dir, st);
}

/* the folders at start-up and on every reload: made, measured, and the health reply told (api.c's info_of) */
static void storage_measure(struct daemon *dm)
{
	storage_check(dm->cfg.record_path, &dm->rec_store);
	storage_check(dm->cfg.machine_path, &dm->msg_store);
	dm->api.record_store = reply_store_of(&dm->rec_store);
	dm->api.message_store = reply_store_of(&dm->msg_store);
	storage_report("recordings", dm->cfg.record_path, &dm->rec_store,
		dm->cfg.record_enabled ? "a call will not be recorded" : "recording is off, so nothing is written there yet");
	storage_report("messages", dm->cfg.machine_path, &dm->msg_store,
		dm->cfg.machine_enabled ? "the answering machine will not answer, and the handset rings on"
					: "the answering machine is off, so nothing is written there yet");
}

/* a restart in the middle of a write leaves its tmp: it can never be finished, so it goes (only this program's own names) */
static void storage_clean_start(const char *dir, const char *what)
{
	unsigned int gone = storage_tmp_clean(dir);

	if (gone)
		printf("the %s folder held %u half-written file%s from the last run: removed\n", what, gone, gone == 1 ? "" : "s");
}

/* one more check right before a file starts: 1 when it can, else one line with the reason and the consequence */
static int storage_startable(const char *what, const char *dir, const char *if_fails, struct storage_state *st)
{
	storage_check(dir, st);
	if (st->ready && st->free_mb >= STORAGE_MIN_FREE_MB)
		return 1;
	if (st->ready)
		printf("the %s folder is nearly full: %s (%llu MB free); %s\n", what, dir, st->free_mb, if_fails);
	else
		printf("the %s folder cannot be used: %s (%s); %s\n", what, dir, storage_reason(st), if_fails);
	return 0;
}

/* when the folder is past its budget its oldest own files go, before a new one starts (the names carry the epoch
 * and the call id, and storage_list hands them oldest first). One prune reads the oldest STORAGE_LIST_MAX files: a
 * folder that grew past that is pruned again at the next call. Nothing else in the folder is ever touched. */
static void store_prune(const char *dir, unsigned int budget_mb, const char *what)
{
	static struct storage_entry list[STORAGE_LIST_MAX];
	static u32 sizes[STORAGE_LIST_MAX];
	unsigned long long budget = (unsigned long long)budget_mb * 1024 * 1024;
	unsigned int n, k, del, gone = 0;
	int more;

	n = storage_list(dir, list, STORAGE_LIST_MAX, &more);
	for (k = 0; k < n; k++)
		sizes[k] = list[k].bytes;
	del = recording_prune_count(sizes, n, budget <= 0xffffffffu ? (u32)budget : 0xffffffffu);
	for (k = 0; k < del; k++) {
		char path[STORAGE_PATH_MAX];

		if (snprintf(path, sizeof(path), "%s/%s", dir, list[k].name) >= (int)sizeof(path))
			continue;
		if (unlink(path))
			printf("an old %s could not be removed from %s: %s\n", what, dir, strerror(errno));
		else
			gone++;
	}
	if (gone)
		printf("the %s folder was over its budget: %u of its oldest files removed%s\n", what, gone,
			more ? " (more are there: the next call takes them)" : "");
}

/* no message can be kept on this call: the caller has heard the greeting (or their voice never reached the tap), so
 * the line is ended rather than leaving someone talking to a machine that keeps nothing */
static void machine_no_message(struct daemon *dm, const char *why)
{
	if (phone_hangup(dm->phone))
		printf("the answering machine keeps no message this call (%s), and the call could not be ended\n", why);
	else
		printf("the answering machine keeps no message this call (%s): the call is ended\n", why);
}

/* start the message file: after the greeting's last frame is audible, or at once when there is no greeting */
static void machine_message_begin(struct daemon *dm)
{
	char base[64];
	int err, ok;

	dm->msg_expected = 0;
	if (!dm->msg_call)
		return;
	if (!dm->msg_armed) {
		machine_no_message(dm, "the capture tap");	/* the start already said why it could not be armed */
		return;
	}
	ok = storage_startable("messages", dm->cfg.machine_path, "the machine keeps no message", &dm->msg_store);
	dm->api.message_store = reply_store_of(&dm->msg_store);
	if (!ok) {
		tap_drop(dm, FE_SINK_MACHINE);
		dm->msg_armed = 0;
		machine_no_message(dm, "the message folder cannot take a file");
		return;
	}
	store_prune(dm->cfg.machine_path, dm->cfg.machine_max_mb, "message");
	snprintf(base, sizeof(base), "%lld-%u", (long long)api_epoch_s(dm->msg_at_ms, mono_ms()), (unsigned)dm->msg_call);
	err = machine_message_open(&dm->machine, (unsigned)dm->msg_call, (u32)dm->msg_rate, dm->cfg.machine_path, base);
	if (err) {
		printf("the answering machine's message was not started: %s: %s\n", dm->cfg.machine_path, strerror(-err));
		tap_drop(dm, FE_SINK_MACHINE);
		dm->msg_armed = 0;
		machine_no_message(dm, "the message file could not be opened");
		return;
	}
	printf("the answering machine is recording the message at %d Hz\n", dm->msg_rate);
}

/* keep the caller's ring about MACHINE_FILL_MS ahead of the bridge; the message starts once the greeting is through */
static void machine_play_feed(struct daemon *dm)
{
	struct awfxs_play_status ps;
	const unsigned int width = dm->msg_rate == 16000 ? 2 : 1;
	u32 target, loops = 0;

	if (!dm->play_armed) {
		if (dm->msg_expected)
			machine_message_begin(dm);
		return;
	}
	if (dev_play_status(&dm->dev, &ps) || !ps.armed || ps.generation != dm->play_generation) {
		/* the bridge restarted under the greeting (an HD change): the ring is gone; the message goes on alone */
		printf("the playback ring was reset under the greeting: it stops\n");
		play_release(dm);
		machine_stop(&dm->machine);
		if (dm->msg_expected)
			machine_message_begin(dm);
		return;
	}
	target = play_lead_frames(MACHINE_FILL_MS);	/* frames, the ring's own unit: 8 of them per millisecond at either band */
	while (ps.stored < target && loops++ < 8) {
		struct awfxs_play_frame frames[256];
		struct awfxs_play_write w;
		int16_t samples[512];
		u32 want = target - ps.stored, k;

		if (want > 256)
			want = 256;		/* 512 samples: the wideband case */
		machine_fill(&dm->machine, samples, want);
		for (k = 0; k < want; k++) {
			frames[k].modem = (u16)samples[k * width];
			frames[k].modem2 = width == 2 ? (u16)samples[k * width + 1] : 0;
			frames[k].line = frames[k].line2 = 0;	/* the handset is on hook: nothing goes to it */
		}
		if (dev_play_write(&dm->dev, frames, want, &w) || w.written < want) {
			printf("the greeting could not be fed to the ring\n");
			play_release(dm);
			machine_stop(&dm->machine);
			return;
		}
		ps.stored += want;
	}
	/* the greeting is audible once the bridge has answered all of its frames: then the caller's words are the message */
	if (dm->msg_expected && ps.frames >= dm->msg_greeting_frames) {
		/* the ring's own count: a non-zero underrun count is the difference between the file and what the caller heard */
		printf("the greeting played: %llu frames, %u underruns, %u drops\n",
			(unsigned long long)ps.frames, ps.underruns, ps.dropped);
		machine_message_begin(dm);
	}
}

/* the machine has the call: the tap for the message, the ring for the greeting (both best effort: the call goes on) */
static void machine_call_start(struct daemon *dm, const struct phone_event *ev)
{
	int err;

	dm->msg_call = (int)ev->call_id;
	dm->msg_at_ms = ev->at_ms;
	dm->msg_rate = tap_rate(dm);
	dm->msg_max_frames = dm->cfg.machine_max_s * (u32)dm->msg_rate;
	dm->msg_expected = 0;
	dm->play_armed = 0;
	if (!dm->cfg.machine_enabled || !dm->machine.greeting_loaded) {
		/* should not happen: the core's machine flag follows the loaded greeting and the folder's state. If a settings
		 * change raced the answer, say so and end the call: no greeting is played and no message kept */
		dm->msg_call = 0;
		printf("the answering machine answered without a greeting: the call is not recorded and the caller hears the line (record or upload one on the Phone settings page)\n");
		machine_no_message(dm, "the greeting is not loaded");
		return;
	}
	err = tap_add(dm, FE_SINK_MACHINE);
	if (!err) {
		dm->msg_armed = 1;
	} else {
		printf("the message cannot be recorded (the capture tap: %s)\n", strerror(-err));
		dm->msg_armed = 0;
	}
	if (machine_start(&dm->machine, (u32)dm->msg_rate)) {
		machine_message_begin(dm);	/* no greeting (a settings change raced the call): the message alone */
		return;
	}
	(void)fe_play_claim(fe_owners(dm->fe), FE_PLAY_GREETING);	/* the greeting always wins the ring */
	err = dev_play_set(&dm->dev, 1);
	if (!err) {
		struct awfxs_play_status ps;

		dm->play_armed = 1;
		dm->msg_expected = 1;
		dm->msg_greeting_frames = dm->machine.ring_frames;
		if (!dev_play_status(&dm->dev, &ps))
			dm->play_generation = ps.generation;
		printf("the answering machine's greeting is playing\n");
		machine_play_feed(dm);		/* primed now, not a tick later */
		return;
	}
	printf("the greeting cannot be played: %s\n", strerror(-err));
	play_release(dm);
	machine_stop(&dm->machine);
	machine_message_begin(dm);
}

/* the machine's call is over (the handset took it, or the remote ended it): the message is finished and kept. The call
 * recording does not start at a takeover: the message is this call's record, and it holds the caller up to the lift —
 * the classic answering-machine behaviour. */
static void machine_call_end(struct daemon *dm)
{
	long long bytes;

	play_release(dm);
	machine_stop(&dm->machine);
	if (dm->msg_armed) {
		tap_drop(dm, FE_SINK_MACHINE);
		dm->msg_armed = 0;
	}
	dm->msg_expected = 0;
	dm->msg_call = 0;
	if (!dm->machine.recording)
		return;
	bytes = machine_message_finish(&dm->machine);
	if (bytes > 0)
		printf("the answering machine's message was saved: %lld bytes\n", bytes);
	else if (bytes < 0)
		printf("the answering machine's message failed: %s\n", strerror((int)-bytes));
}

/* the tap frames of one call: on answered, arm the kernel tap and open the file; on the end, close and keep it */
static void record_call_event(struct daemon *dm, const struct phone_event *ev)
{
	int err;

	if (!dm->cfg.record_enabled)
		return;
	if (ev->call == CALL_ACTIVE && ev->machine != PHONE_MACHINE_NONE)
		return;			/* the machine's call: its message is this call's recording and the handset hears nothing.
					 * A lift later (PHONE_MACHINE_TAKEN) does not re-arm this recorder: the message is the call's
					 * record up to the lift, and one call keeps one record - deliberately. */
	if (ev->call == CALL_ACTIVE && !dm->rec.open) {
		int ok = storage_startable("recordings", dm->cfg.record_path, "the call is not recorded", &dm->rec_store);

		dm->api.record_store = reply_store_of(&dm->rec_store);
		if (!ok)
			return;
		store_prune(dm->cfg.record_path, dm->cfg.record_max_mb, "recording");
		dm->rec_rate = tap_rate(dm);
		snprintf(dm->rec_name, sizeof(dm->rec_name), "%lld-%u.wav",
			api_epoch_s(ev->at_ms, mono_ms()), ev->call_id);
		dm->rec_call_id = ev->call_id;
		err = tap_add(dm, FE_SINK_RECORDER);
		if (err) {
			printf("the call was not recorded: the capture tap could not be armed (%s)\n", strerror(-err));
		} else {
			char base[sizeof(dm->rec_name)];

			snprintf(base, sizeof(base), "%.*s", (int)strlen(dm->rec_name) - 4, dm->rec_name);
			err = recorder_start(&dm->rec, dm->cfg.record_path, base, (u32)dm->rec_rate, 2);
			if (err)
				printf("the call was not recorded: %s: %s\n", dm->cfg.record_path, strerror(-err));
		}
		if (err) {
			tap_drop(dm, FE_SINK_RECORDER);
			dm->rec_call_id = 0;
			dm->rec_name[0] = 0;		/* the log never names a file that was not written */
			return;
		}
		dm->rec_armed = 1;
		printf("the call is being recorded at %d Hz\n", dm->rec_rate);
		return;
	}
	if ((ev->call == CALL_GONE || ev->call == CALL_NONE) && dm->rec.open) {
		long long saved;

		tap_drop(dm, FE_SINK_RECORDER);
		dm->rec_armed = 0;
		saved = recorder_finish(&dm->rec);
		if (saved >= 0)
			printf("the recording was saved: %lld bytes\n", saved);
		else {
			dm->rec_call_id = 0;
			dm->rec_name[0] = 0;	/* the log never names a file that was not written */
			printf("the recording failed: %s\n", strerror((int)-saved));
		}
	}
}

/*
 * The wall clock for the alarms. It is trusted when the year is not before ALARM_CLOCK_MIN_YEAR: a router that lost its time starts years back,
 * and the time service steps it forward once it has an answer. The kernel's synchronised state (adjtimex() TIME_ERROR, STA_UNSYNC) is not
 * consulted: the firmware's time service sets the clock without the kernel's NTP discipline, so that flag never clears there.
 */
#define ALARM_CLOCK_MIN_YEAR	2026

static int hook_wall_clock(void *ctx, struct tm *out)
{
	time_t now = time(NULL);

	(void)ctx;
	if (!localtime_r(&now, out) || out->tm_year + 1900 < ALARM_CLOCK_MIN_YEAR)
		return -EAGAIN;
	return 0;
}

static void hook_event(void *ctx, const struct phone_event *ev)
{
	struct daemon *dm = ctx;

	if (ev->kind == PHONE_EV_CALL) {
		audio_note(dm, ev);
		record_call_event(dm, ev);
	} else if (ev->kind == PHONE_EV_MACHINE) {
		if (ev->machine == PHONE_MACHINE_ANSWERED)
			machine_call_start(dm, ev);	/* the modem put the answer through: the greeting and the message start */
		else if (ev->machine == PHONE_MACHINE_TAKEN || ev->machine == PHONE_MACHINE_ENDED)
			machine_call_end(dm);		/* the handset took the call, or it is over: the message is closed */
	} else if (ev->kind == PHONE_EV_DND) {
		dm->dnd.manual = ev->manual;		/* the handset's own code: from this step the line follows it */
	}
	/* the quiet verdict travels with the event: whether a network phone is invited to ring is decided in this step,
	   and a flag left over from the previous tick invites the ring for a call the line has just held quiet */
	dm->quiet = ev->quiet;
	fe_observe_line(dm, ev->line, ev->call, ev->dir, ev->call_id, ev->answered_by, 0);
	api_event(&dm->api, ev);
}

static int hook_level(void *ctx, unsigned int *peak, unsigned int *windows)
{
	struct daemon *dm = ctx;
	struct awfxs_bridge_stats bs;
	int err;

	memset(&bs, 0, sizeof(bs));
	err = dev_bridge_stats(&dm->dev, &bs);
	if (!err) {
		*peak = bs.modem_window_peak;
		*windows = bs.modem_windows;
	}
	return err;
}

static int api_bridge_stats(void *ctx, void *out)
{
	struct daemon *dm = ctx;

	return dev_bridge_stats(&dm->dev, out);
}

static int api_reload(void *ctx)
{
	(void)ctx;
	return reload();
}

static void on_ubus_connect(struct ubus_context *ctx)
{
	int err = api_init(ctx, &d.api);

	if (err)
		printf("the ubus object could not be registered: %s\n", ubus_strerror(err));
	else
		printf("ubus object awvoice registered\n");
	sip_guards_run(&d);	/* the name can be looked up now: another owner decides whether the front end may run */
}

/* ---- start-up ------------------------------------------------------------------------------------------------------- */

/* the whole file into a new buffer (the board file is a few hundred bytes) */
static int read_file(const char *path, uint8_t **data, size_t *size)
{
	struct stat st;
	uint8_t *buf;
	size_t got = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || st.st_size <= 0 || st.st_size > BOARD_FILE_MAX) {
		close(fd);
		return -EIO;
	}
	buf = malloc((size_t)st.st_size);
	while (buf && got < (size_t)st.st_size) {
		ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);

		if (n <= 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	if (!buf || got != (size_t)st.st_size) {
		free(buf);
		return -EIO;
	}
	*data = buf;
	*size = got;
	return 0;
}

/* the stage tool's --live path: stages 0..4 with the board's init section in place of the general stage */
static int bring_up(void)
{
	const unsigned int ch = d.cfg.channel;
	int err;

	printf("stage 0: identify\n");
	err = stage0_identify(&d.chip, stdout);
	if (err)
		return err;
	printf("stage 1: script section %s\n", INIT_SECTION);
	err = stage_script(&d.chip, &d.board, INIT_SECTION, stdout);
	if (err)
		return err;
	printf("stage 2: battery converter, channel %u\n", ch);
	err = stage2_converter(&d.chip, &d.board, ch, stdout);
	if (err)
		return err;
	printf("stage 3: calibration round B, channel %u\n", ch);
	err = stage3_calibrate(&d.chip, &d.board, ch, stdout);
	if (err)
		return err;
	printf("stage 4: dc feed preset, both channels\n");
	return stage4_dcfeed(&d.chip, &d.board, stdout);
}

/*
 * Bring the audio path up at another rate. The PCM block's slots, the line chip's own bring-up and the bridge's flag
 * are one layout: the slots cannot be rewritten while the bus runs (the kernel rebuilds the clocks and resets the chip
 * to change them), so the only honest way to move it is the way a service start does it - line down, hardware up, line
 * in service again. The core asks for this only on a line nobody is using, so no call is ever cut by it.
 */
static void layout_rebuild(struct daemon *dm, int want)
{
	struct phone_cfg pc;
	int err = 0;

	printf("the audio layout changes to %d Hz: the line is brought up again\n", want ? 16000 : 8000);
	phone_close(dm->phone, 0);
	dm->phone = NULL;
	if (dm->tap_armed) {
		(void)dev_tap_set(&dm->dev, 0);		/* the kernel disarms it with the bridge; the flag says so here */
		dm->tap_armed = 0;
	}
	dm->hd_layout = want;
	if ((err = dev_prepare(&dm->dev, want)) || (err = bring_up())) {
		printf("the line did not come up at %d kHz (%s): it is brought up narrowband instead\n",
			want ? 16 : 8, strerror(-err));
		dm->hd_layout = 0;
		if (!(err = dev_prepare(&dm->dev, 0)))
			err = bring_up();
	}
	if (!err) {
		phone_cfg_of(&dm->cfg, &pc);
		pc.hd = dm->hd_layout;
		err = phone_open(&dm->phone, &dm->chip, &dm->board, &dm->ops, &pc, &dm->hooks, stdout);
	}
	if (err || !dm->phone) {
		/* the line is down and nothing was left half-brought-up: the service is restarted, which is the same
		   path a failed chip take-down takes, and the log says which layout it died on */
		printf("the line is down after the %d kHz change: the service stops and is restarted (%s)\n",
			dm->hd_layout ? 16 : 8, strerror(-err ? err : -EIO));
		dm->exit_code = 1;
		uloop_end();
		return;
	}
	dm->tap_wideband = dm->hd_layout;		/* until the tap is armed and read again, this is the layout it will report */
	dm->api.phone = dm->phone;
	dm->api.hd = dm->cfg.hd ? (phone_hd_active(dm->phone) ? 2 : 1) : 0;
	dm->api.layout = dm->hd_layout;
	printf("the line is in service at %d Hz\n", tap_rate(dm));
}

static int single_instance(void)
{
	int fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

	if (fd < 0) {
		report(LOCK_PATH, -errno);
		return -1;
	}
	if (flock(fd, LOCK_EX | LOCK_NB)) {
		printf("awvoice-phoned is already running\n");
		close(fd);
		return -1;
	}
	return fd;		/* held until the process ends */
}

/* the module's interface number is not this build's: the one line both start paths say (the remedy follows the side) */
static void abi_problem(unsigned int module_abi)
{
	printf("the kernel module's interface is %u and this build's is %u: %s\n", module_abi, (unsigned)AWFXS_ABI,
		module_abi_check(module_abi, AWFXS_ABI) == MODULE_ABI_OLDER
			? "the loaded module is older; the installed one (/lib/modules) is this daemon's pair"
			: "the module is newer; this daemon is out of date and its package must be updated");
}

/*
 * --check-module: the module's interface number, read before procd starts the daemon, so the init script can replace a
 * module that did not come from the installed package (the preinit loader reads /etc/modules-boot.d from the firmware
 * image before the overlay is mounted, so an upgrade can leave the image's older module loaded while the installed one
 * waits on the overlay). Nothing else is touched: a stopped service leaves the chip in reset, and closing the device
 * puts it back there.
 * Exit codes, as the init script reads them: 0 the module is this build's pair; 1 the module is older (loading the
 * installed module replaces it); 2 the module is newer (only an updated package helps); 3 the device cannot be read.
 * A device another process holds is not a fault: that daemon checked the same number at its own start.
 */
static int check_module(void)
{
	struct awfxs_info info;
	enum module_abi_verdict v;
	int fd = open("/dev/awvoice-fxs0", O_RDWR);

	if (fd < 0) {
		if (errno == EBUSY)
			return 0;
		printf("/dev/awvoice-fxs0: %s\n", strerror(errno));
		return 3;
	}
	memset(&info, 0, sizeof(info));
	if (ioctl(fd, AWFXS_IOC_INFO, &info)) {
		int err = errno;

		close(fd);
		printf("/dev/awvoice-fxs0 did not answer the interface call: %s\n", strerror(err));
		return 3;
	}
	close(fd);
	v = module_abi_check(info.abi, AWFXS_ABI);
	if (v != MODULE_ABI_OK)
		abi_problem(info.abi);
	return (int)v;
}

int main(int argc, char **argv)
{
	struct phone_cfg pc;
	size_t size = 0;
	int err;

	setvbuf(stdout, NULL, _IOLBF, 0);	/* every line reaches the log even if the board resets mid-run */
	if (argc > 1) {
		if (argc == 2 && !strcmp(argv[1], "--check-module"))
			return check_module();
		fprintf(stderr, "usage: %s [--check-module]\n", argv[0]);
		return 1;
	}
	if (single_instance() < 0)
		return 1;

	config_load(&d.cfg, stdout);
	if (!d.cfg.enabled) {
		printf("the phone service is switched off (awvoice.main.enabled)\n");
		return 0;
	}

	machine_init(&d.machine);
	err = fe_open(&d.fe, &fe_sink, 0);	/* the sink carries the rules' events to qmodem_voip, and later its frames */
	if (err) {
		printf("the front end did not open: %s\n", strerror(-err));
		return 1;
	}
	qmsk_server_init(&d.media);
	d.media_fd.fd = -1;		/* no listening socket until the guards allow the line's front end */
	d.ctl_ops = (struct ctl_ops){ .ctx = &d, .answer = ctl_answer, .takeover = ctl_takeover, .dial = ctl_dial, .hangup = ctl_hangup,
		.dtmf = ctl_dtmf, .flash = ctl_flash, .status = ctl_status, .now_ms = ctl_now };
	arbiter_init(&d.arbiter, d.fe, &d.media.bindings, &d.ctl_ops);
	storage_clean_start(d.cfg.record_path, "recordings");
	storage_clean_start(d.cfg.machine_path, "messages");

	err = read_file(d.cfg.board, &d.board_file, &size);
	if (!err)
		err = board_parse(d.board_file, size, &d.board);
	if (err) {
		report(d.cfg.board, err);
		free(d.board_file);
		return 1;
	}
	d.chip = (struct chip){ .ops = &dev_ops, .ctx = &d.dev };
	err = dev_open(&d.dev);
	if (err) {
		if (err == -EPROTO)
			abi_problem(d.dev.abi);		/* the module is loaded and answers, it is just not this build's */
		else
			report("/dev/awvoice-fxs0", err);
		free(d.board_file);
		return 1;
	}
	err = dev_prepare(&d.dev, 0);		/* every start comes up narrowband: the wideband layout is taken up only
						   after the modem has answered with that rate, and it moves the PCM slots,
						   the chip's bring-up and the bridge together (layout_rebuild) */
	if (!err) {
		d.hd_layout = 0;
		err = bring_up();
	}
	if (err) {
		printf("the line chip did not come up: %s (closing the device holds the chip in reset)\n", strerror(-err));
		dev_close(&d.dev);
		free(d.board_file);
		return 1;
	}

	d.ans.fd = -1;
	call_ops_of(&d);
	modem_find(mono_ms());		/* a missing port is not a fault: the line is served and the port is looked for again */

	/* the numbering plan the line starts with: a named setting from the first moment, auto from the shipped plan until
	 * the network reports its code (region_follow takes the change when it does) */
	d.region = number_region_find(region_choice(d.cfg.region, "", NULL)) ?: number_region_default();
	/* the HD state: the settings' choice, and whether the chip's wideband mode is on it */
	d.api.hd = d.cfg.hd ? (d.phone && phone_hd_active(d.phone) ? 2 : 1) : 0;
	d.api.layout = d.hd_layout;
	config_load_book(&d.book, d.region, stdout);
	config_load_blocking(&d.block, d.region, stdout);
	dnd_reload(&d);
	config_load_alarms(&d.alarms, stdout);
	config_load_forward(&d.forward, stdout);
	calllog_init(&d.calls, CALLS_RAM_PATH, CALLS_FLASH_PATH, mono_ms());
	if (!calllog_load(&d.calls, stdout))
		printf("the call log was loaded\n");
	d.hooks = (struct phone_hooks){ .ctx = &d, .bridge_start = hook_bridge, .modem_level = hook_level, .event = hook_event, .call_ended = hook_call_ended, .wall_clock = hook_wall_clock };
	snprintf(d.web_reason, sizeof(d.web_reason), "off");
	d.api = (struct api_ctx){ .my_number = d.cfg.my_number, .channel = d.cfg.channel, .at_interface = d.cfg.at_interface,
		.port = d.ans.port, .bridge_stats = api_bridge_stats, .reload = api_reload, .ctx = &d, .hd = d.cfg.hd, .region = d.region->name,
		.record_path = d.cfg.record_path, .message_path = d.cfg.machine_path,
		.machine = &d.machine, .machine_delay_s = d.cfg.machine_delay_s, .arbiter = &d.arbiter,
		.greeting_up = &d.greeting_up, .greeting_may_swap = greeting_may_swap, .greeting_applied = greeting_applied,
		.web = d.web, .web_port = d.cfg.web_port, .sip_web_audio = d.web_reason };
	sip_guard_init(&d.guard);
	d.api.guard = &d.guard;
	d.api.sip_mode = sip_mode_name(sip_mode_of(d.cfg.sip_mode));
	d.api.sip_refusal = d.arbiter.last_refusal;
	sip_guards_run(&d);		/* the line and the object follow the guards and the mode, now that both are known */
	storage_measure(&d);		/* both folders made and proven before the first call, and the health reply told */
	/* the greeting is read before the core is set up: a switched-on machine answers the first call after a restart
	 * instead of waiting for someone to save the settings, and the page's card has an answer before any reload at all.
	 * The line is not up yet, so the quiet test a reload needs is nothing to wait for here. Whatever an earlier run
	 * left staged goes with it. */
	machine_upload_clean(MACHINE_GREETING_DIR);
	{
		unsigned long long t0 = mono_ms();

		if (!machine_greeting_load(&d.machine, MACHINE_GREETING_PATH, d.cfg.machine_enabled ? stdout : NULL) && d.cfg.machine_enabled)
			printf("the answering machine is on (the greeting: %u frames at 8000 Hz, %u at 16000; loaded in %llu ms)\n",
				d.machine.frames[0], d.machine.frames[1], mono_ms() - t0);
	}
	phone_cfg_of(&d.cfg, &pc);	/* after the folders: the machine answers only when a message could be kept */
	{
		struct phone_cfg wish = pc;		/* the settings' wish, kept while the line is opened at the layout in force */

		pc.hd = d.hd_layout;
		indicator_init(&d.ind);
		indicator_apply(&d.ind, d.cfg.led, 0, 0, stdout);
		d.api.ind = &d.ind;
		d.api.dnd = &d.dnd;		/* the health reply reads the same quiet hours the line judges against */
		err = phone_open(&d.phone, &d.chip, &d.board, &d.ops, &pc, &d.hooks, stdout);
		if (err) {
			printf("the voice path did not start: %s\n", strerror(-err));
			answer_close(&d.ans);
			dev_close(&d.dev);
			free(d.board_file);
			return 1;
		}
		if (wish.hd != d.hd_layout && phone_set_cfg(d.phone, &wish))	/* the wish enters as a settings change:
										   the core asks the modem and
										   owes the bring-up on its answer */
			printf("  channel %u: the HD voice setting could not be handed to the line; it is taken up at the next save\n",
				d.cfg.channel);
	}

	d.api.phone = d.phone;
	d.api.calls = &d.calls;
	d.api.book = &d.book;
	d.api.start_ms = mono_ms();
	uloop_init();
	d.loop_up = 1;
	d.guard_timer.cb = guard_recheck;
	uloop_timeout_set(&d.guard_timer, GUARD_RECHECK_MS);
	d.provision_timer.cb = provision_recheck;
	uloop_timeout_set(&d.provision_timer, PROVISION_SWEEP_MS);
	sip_apply(&d);		/* the media socket needs the loop: the guards' word is applied again now that it is up */
	err = signals_start(&d.signal_fd);
	if (!err)
		err = timer_start(&d.timer_fd);
	if (err) {
		report("event loop", err);
		d.exit_code = 1;
	} else {
		d.ubus.cb = on_ubus_connect;		/* a ubusd that is not up yet is waited for: the line works without the object */
		ubus_auto_connect(&d.ubus);
		printf("the phone line is in service\n");
		uloop_run();
		if (d.api.connected)
			ubus_auto_shutdown(&d.ubus);
	}
	web_close(&d);				/* the page's audio goes before the loop it was added to */
	media_close(&d);			/* the media socket goes before the loop it was added to */
	uloop_done();

	indicator_close(&d.ind);		/* the LED dark before the chip goes into reset */
	if (d.rec.open)
		recorder_abort(&d.rec);		/* the call died with the daemon: a half recording cannot be finished */
	if (d.tap_armed)
		dev_tap_set(&d.dev, 0);		/* the tap serves every sink: one disarm covers the recorder and the message */
	if (d.play_armed)
		dev_play_set(&d.dev, 0);	/* the playback ring stops before the chip goes into reset */
	machine_free(&d.machine);		/* a half-written message cannot be finished: its tmp goes */
	err = phone_close(d.phone, d.exit_code ? -EIO : 0);
	if (calllog_persist(&d.calls, mono_ms(), 1))
		printf("the call log could not be saved\n");
	if (err && !d.exit_code) {
		printf("the line was taken out of service with an error: %s\n", strerror(-err));
		d.exit_code = 1;
	}
	if (d.ans_open)
		answer_close(&d.ans);
	dev_close(&d.dev);
	fe_close(d.fe);
	free(d.board_file);
	printf("stopped (closing the device holds the chip in reset)\n");
	return d.exit_code;
}
