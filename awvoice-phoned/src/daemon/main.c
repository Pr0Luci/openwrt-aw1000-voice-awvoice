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
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>

#include <libubox/uloop.h>
#include <libubus.h>

#include "answer.h"
#include "api.h"
#include "alarm.h"
#include "blocking.h"
#include "forward.h"
#include "calllog.h"
#include "phonebook.h"
#include "numbers.h"
#include "board.h"
#include "cid.h"
#include "config.h"
#include "hw.h"
#include "indicator.h"
#include "machine.h"
#include "module_abi.h"
#include "phone.h"
#include "port.h"
#include "recorder.h"
#include "recording.h"
#include "script.h"
#include "slic_map.h"
#include "stages.h"
#include "storage.h"

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
	struct alarms alarms;
	struct phone_forward forward;
	struct indicator ind;		/* the phone LED (indicator.c): the line state, driven through /sys/class/leds */
	unsigned int audio_call;		/* the answered call whose audio counters were noted at its start (0 = none) */
	struct awfxs_bridge_stats audio_base;
	struct recorder rec;		/* the answered call being recorded (awvoice.record.enabled); one at a time */
	int rec_armed;			/* the kernel tap is armed for the open recording */
	u32 rec_generation;		/* the tap generation the open recording follows */
	u32 rec_call_id;		/* the recorded call's id: it names the file with the epoch */
	int rec_rate;			/* the recording's sample rate: the call's own (8000, or 16000 on HD) */
	char rec_name[64];		/* the recording's file name while it writes */
	struct machine machine;		/* the answering machine's greeting and its one message (awvoice.machine.*) */
	int play_armed;			/* the kernel playback ring is armed for the greeting toward the caller */
	u32 play_generation;		/* the playback generation the greeting follows */
	int msg_call;			/* the machine's call whose message is coming (0 = none) */
	unsigned long long msg_at_ms;	/* the call's answer: the message file name's epoch */
	int msg_armed;			/* the capture tap is armed for the machine's message */
	u32 msg_generation;		/* the tap generation the message follows */
	int msg_expected;		/* the greeting is through: the message starts (the ring's count says when) */
	u32 msg_greeting_frames;	/* the ring frames the greeting fills */
	u32 msg_max_frames;		/* the message's cap in mono frames (machine_max_s at the call's rate) */
	int msg_rate;			/* the message's rate: the call's own (8000, or 16000 on HD) */
	int greeting_stale;		/* a reload changed the greeting while a machine call was under way */
	struct storage_state rec_store;	/* the recordings folder: made and proven at start-up, on reload, at a call */
	struct storage_state msg_store;	/* the messages folder, the same way */
	int exit_code;
};

static struct daemon d;

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
		.region = number_region_find(c->region),
		.book = &d.book,
		.block = &d.block,
		.alarms = &d.alarms,
		.forward = d.forward,
		.cid = CID_DTMF,
		.hd = c->hd,
		.machine = c->machine_enabled && d.machine.enabled && storage_can_write(&d.msg_store),
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
}

static void modem_close(void)
{
	answer_close(&d.ans);
	d.ans_open = 0;
	d.ans.port[0] = 0;		/* the status shows no port */
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
static void machine_tap_tick(struct daemon *dm);
static void machine_call_start(struct daemon *dm, const struct phone_event *ev);
static void machine_call_end(struct daemon *dm);
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

/* load or drop the greeting for the current settings. -EBUSY while a machine call is under way or the line is busy:
 * the greeting is neither freed under the fill nor read while a call is up (the caller tries again when it is idle). */
static int machine_refresh(struct daemon *dm)
{
	unsigned long long t0;
	int err;

	if (dm->machine.playing || dm->machine.recording)
		return -EBUSY;		/* checked first: a switched-off machine must not free the greeting under the fill */
	if (!dm->cfg.machine_enabled) {
		machine_greeting_drop(&dm->machine);
		return 0;
	}
	if (!line_quiet(dm))
		return -EBUSY;
	t0 = mono_ms();
	err = machine_greeting_load(&dm->machine, dm->cfg.machine_greeting, stdout);
	if (!err)
		printf("the greeting was loaded in %llu ms\n", mono_ms() - t0);
	return err;
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

/* ---- the step ------------------------------------------------------------------------------------------------------- */

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
	err = phone_step(d.phone, now);
	if (err) {
		printf("the line stopped: %s\n", strerror(-err));
		d.exit_code = 1;
		uloop_end();
		return;
	}
	modem_watch(now);
	if (d.phone) {
		struct phone_status st;

		phone_status(d.phone, &st);
		indicator_step(&d.ind, &st, now);	/* the phone LED follows the line (the patterns, indicator.c) */
	}
	if (d.rec.open && d.rec_armed) {
		struct awfxs_tap_frame frames[256];
		struct awfxs_tap_read meta;
		uint16_t samples[1024];
		unsigned int got, k, n, loops = 0;
		int err = 0;

		/* the bridge hands out 8000 frames a second and a tick is 5 ms: a single read of 32 frames drained
		 * only 6400 a second, the ring filled in about five seconds and the file turned to distortion.
		 * Drain until the ring is empty (bounded by the ring itself) before the next tick. */
		do {
			got = dev_tap_read(&d.dev, frames, 256, &meta);
			if (meta.generation != d.rec_generation) {
				/* the bridge restarted under the recording (an HD switch changes the rate with it):
				 * the part so far is kept and a new file starts at the new rate */
				long long saved = recorder_finish(&d.rec);

				if (saved >= 0)
					printf("the recording goes on in a new file after the rate change (%lld bytes kept)\n", saved);
				d.rec_rate = phone_hd_active(d.phone) && d.cfg.hd ? 16000 : 8000;
				snprintf(d.rec_name, sizeof(d.rec_name), "%lld-%u.wav", api_epoch_s(now, now), d.rec_call_id);
				char base2[sizeof(d.rec_name)];

				snprintf(base2, sizeof(base2), "%.*s", (int)strlen(d.rec_name) - 4, d.rec_name);
				err = recorder_start(&d.rec, d.cfg.record_path, base2, (u32)d.rec_rate, 2);
				if (err) {
					printf("the recording stops at the rate change: %s: %s\n", d.cfg.record_path, strerror(-err));
					dev_tap_set(&d.dev, 0);
					d.rec_call_id = 0;
					d.rec_name[0] = 0;	/* the log never names a file that was not written */
					d.rec_armed = 0;
					break;
				}
			}
			if (!got)
				break;
			/* interleaved stereo: the far end (the modem's slot) and the handset (the line chip's slot);
			 * wideband adds the second time sample of each, so the pair is 16 kHz like the call itself */
			for (k = 0, n = 0; k < got; k++) {
				samples[n++] = frames[k].modem;
				samples[n++] = frames[k].line;
				if (meta.wideband && d.rec_rate == 16000) {
					samples[n++] = frames[k].modem2;
					samples[n++] = frames[k].line2;
				}
			}
			err = recorder_feed(&d.rec, samples, n);
			if (err) {
				printf("the recording failed at %s: %s\n", d.cfg.record_path, strerror(-err));
				dev_tap_set(&d.dev, 0);	/* the failed feed discarded its tmp: the tap goes quiet with it */
				d.rec_call_id = 0;
				d.rec_name[0] = 0;		/* the log never names a file that was not written */
				d.rec_armed = 0;
				break;
			}
		} while (++loops < 32);	/* 32 x 256 frames is the whole ring; more cannot arrive inside one tick */
	}
	if (d.msg_call) {
		machine_play_feed(&d);		/* the greeting toward the caller, then the silence */
		machine_tap_tick(&d);		/* the caller's side into the message, and the cap that ends the call */
	}
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

	config_load(&now, stdout);
	if (now.channel != d.cfg.channel || strcmp(now.board, d.cfg.board)) {
		printf("the channel or the board file changed: the line is set up again\n");
		d.exit_code = 0;
		uloop_end();
		return 0;
	}
	if (now.at_interface != d.cfg.at_interface) {
		if (d.ans_open)
			modem_close();
		d.port_next_ms = 0;
	}
	d.cfg = now;
	if (machine_refresh(&d) == -EBUSY) {
		/* the old greeting stays loaded until the machine's call is over and the line is quiet (the timer loads the
		 * new one then) */
		d.greeting_stale = 1;
		printf("the line is busy or the greeting is in use: the new greeting is loaded when the line is idle\n");
	} else {
		d.greeting_stale = 0;
	}
	d.api.hd = now.hd ? (d.phone && phone_hd_active(d.phone) ? 2 : 1) : 0;
	indicator_apply(&d.ind, now.led, 0, 0, stdout);	/* only a change acts */
	config_load_blocking(&d.block, number_region_find(now.region), stdout);
	config_load_alarms(&d.alarms, stdout);
	config_load_forward(&d.forward, stdout);
	config_load_book(&d.book, number_region_find(now.region), stdout);	/* between two steps: the core sees the new book whole */
	d.api.at_interface = now.at_interface;
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
	signal(SIGPIPE, SIG_IGN);		/* a modem port that went away must not end us */
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

		if (!dev || !end || strstr(line, "\\040"))	/* a space in the mount point: never a folder of ours */
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

/* a restart in the middle of a write leaves its tmp: it can never be finished, so it goes (only our own names) */
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
		(void)dev_tap_set(&dm->dev, 0);
		dm->msg_armed = 0;
		machine_no_message(dm, "the message folder cannot take a file");
		return;
	}
	store_prune(dm->cfg.machine_path, dm->cfg.machine_max_mb, "message");
	snprintf(base, sizeof(base), "%lld-%u", (long long)api_epoch_s(dm->msg_at_ms, mono_ms()), (unsigned)dm->msg_call);
	err = machine_message_open(&dm->machine, (unsigned)dm->msg_call, (u32)dm->msg_rate, dm->cfg.machine_path, base);
	if (err) {
		printf("the answering machine's message was not started: %s: %s\n", dm->cfg.machine_path, strerror(-err));
		(void)dev_tap_set(&dm->dev, 0);
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
		(void)dev_play_set(&dm->dev, 0);
		dm->play_armed = 0;
		machine_stop(&dm->machine);
		if (dm->msg_expected)
			machine_message_begin(dm);
		return;
	}
	target = MACHINE_FILL_MS * (u32)dm->msg_rate / 1000;
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
			(void)dev_play_set(&dm->dev, 0);
			dm->play_armed = 0;
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
	struct awfxs_tap_frame frames[32];
	struct awfxs_tap_read meta;
	int err;

	dm->msg_call = (int)ev->call_id;
	dm->msg_at_ms = ev->at_ms;
	dm->msg_rate = phone_hd_active(dm->phone) && dm->cfg.hd ? 16000 : 8000;
	dm->msg_max_frames = dm->cfg.machine_max_s * (u32)dm->msg_rate;
	dm->msg_expected = 0;
	dm->play_armed = 0;
	if (!dm->cfg.machine_enabled || !dm->machine.enabled) {
		/* should not happen: the core's machine flag follows the loaded greeting and the folder's state. If a settings
		 * change raced the answer, say so and end the call: no greeting is played and no message kept */
		dm->msg_call = 0;
		printf("the answering machine answered without a greeting: the call is not recorded and the caller hears the line (check awvoice.main.machine_greeting)\n");
		machine_no_message(dm, "the greeting is not loaded");
		return;
	}
	err = dev_tap_set(&dm->dev, 1);
	if (!err) {
		(void)dev_tap_read(&dm->dev, frames, 32, &meta);	/* the arm reset the ring: take the generation to follow */
		dm->msg_generation = meta.generation;
		dm->msg_armed = 1;
	} else {
		printf("the message cannot be recorded (the capture tap: %s)\n", strerror(-err));
		dm->msg_armed = 0;
	}
	if (machine_start(&dm->machine, (u32)dm->msg_rate)) {
		machine_message_begin(dm);	/* no greeting (a settings change raced the call): the message alone */
		return;
	}
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
	machine_stop(&dm->machine);
	machine_message_begin(dm);
}

/* the machine's call is over (the handset took it, or the remote ended it): the message is finished and kept. The call
 * recording does not start at a takeover: the message is this call's record, and it holds the caller up to the lift —
 * the classic answering-machine behaviour. */
static void machine_call_end(struct daemon *dm)
{
	long long bytes;

	if (dm->play_armed) {
		(void)dev_play_set(&dm->dev, 0);
		dm->play_armed = 0;
	}
	machine_stop(&dm->machine);
	if (dm->msg_armed) {
		(void)dev_tap_set(&dm->dev, 0);
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

/* the tap frames of the machine's call: discarded while the greeting plays, then the caller's side is the message */
static void machine_tap_tick(struct daemon *dm)
{
	struct awfxs_tap_frame frames[256];
	struct awfxs_tap_read meta;
	int16_t samples[512];
	unsigned int got, k, n, loops = 0;
	int err;

	if (!dm->msg_armed)
		return;			/* the tap is not on the caller (never armed, or its message is already closed) */
	do {
		got = dev_tap_read(&dm->dev, frames, 256, &meta);
		if (meta.generation != dm->msg_generation) {
			printf("the capture tap was reset under the message: it is kept and the recording stops\n");
			(void)dev_tap_set(&dm->dev, 0);
			dm->msg_armed = 0;
			if (dm->machine.recording && machine_message_finish(&dm->machine) > 0)
				printf("the answering machine's message was saved\n");
			machine_no_message(dm, "the capture tap was reset");
			return;
		}
		if (!got)
			break;
		if (!dm->machine.recording)
			continue;		/* the greeting is still going: the caller's words now are its echo */
		for (k = 0, n = 0; k < got; k++) {
			samples[n++] = (int16_t)frames[k].modem;
			if (meta.wideband && dm->msg_rate == 16000)
				samples[n++] = (int16_t)frames[k].modem2;
		}
		err = machine_message_feed(&dm->machine, samples, n);
		if (err) {
			printf("the answering machine's message failed: %s\n", strerror(-err));
			(void)dev_tap_set(&dm->dev, 0);
			dm->msg_armed = 0;
			machine_no_message(dm, "the message could not be written");
			return;
		}
		if (dm->machine.rec_frames >= dm->msg_max_frames) {
			/* the caller had the time the setting allows: the message is closed and the call ends here */
			long long bytes = machine_message_finish(&dm->machine);

			if (bytes > 0)
				printf("the answering machine's message was saved: %lld bytes\n", bytes);
			(void)dev_tap_set(&dm->dev, 0);
			dm->msg_armed = 0;
			if (phone_hangup(dm->phone))
				printf("the call could not be ended at the message's limit\n");
			else
				printf("the answering machine's message reached its limit (%u s): the call is ended\n", dm->cfg.machine_max_s);
			return;
		}
	} while (++loops < 32);
}

/* the tap frames of one call: on answered, arm the kernel tap and open the file; on the end, close and keep it */
static void record_call_event(struct daemon *dm, const struct phone_event *ev)
{
	struct awfxs_tap_frame frames[32];
	struct awfxs_tap_read meta;
	int err;

	if (!dm->cfg.record_enabled)
		return;
	if (ev->call == CALL_ACTIVE && ev->machine != PHONE_MACHINE_NONE)
		return;			/* the machine's call: its message is this call's recording and the handset hears nothing.
					 * A lift later (PHONE_MACHINE_TAKEN) does not re-arm this recorder: the message is the call's
					 * record up to the lift, and one call keeps one record. The policy is deliberate for F2. */
	if (ev->call == CALL_ACTIVE && !dm->rec.open) {
		int ok = storage_startable("recordings", dm->cfg.record_path, "the call is not recorded", &dm->rec_store);

		dm->api.record_store = reply_store_of(&dm->rec_store);
		if (!ok)
			return;
		store_prune(dm->cfg.record_path, dm->cfg.record_max_mb, "recording");
		dm->rec_rate = phone_hd_active(dm->phone) && dm->cfg.hd ? 16000 : 8000;
		snprintf(dm->rec_name, sizeof(dm->rec_name), "%lld-%u.wav",
			api_epoch_s(ev->at_ms, mono_ms()), ev->call_id);
		dm->rec_call_id = ev->call_id;
		err = dev_tap_set(&dm->dev, 1);
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
			dev_tap_set(&dm->dev, 0);
			dm->rec_call_id = 0;
			dm->rec_name[0] = 0;		/* the log never names a file that was not written */
			return;
		}
		(void)dev_tap_read(&dm->dev, frames, 32, &meta);	/* the arm reset the ring: take the generation to follow */
		dm->rec_generation = meta.generation;
		dm->rec_armed = 1;
		printf("the call is being recorded at %d Hz\n", dm->rec_rate);
		return;
	}
	if ((ev->call == CALL_GONE || ev->call == CALL_NONE) && dm->rec.open) {
		long long saved;

		dev_tap_set(&dm->dev, 0);
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
	}
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
	storage_clean_start(d.cfg.record_path, "recordings");
	storage_clean_start(d.cfg.machine_path, "messages");
	if (d.cfg.machine_enabled) {
		unsigned long long t0 = mono_ms();

		if (!machine_greeting_load(&d.machine, d.cfg.machine_greeting, stdout))
			printf("the answering machine is on (the greeting: %u frames at 8000 Hz, %u at 16000; loaded in %llu ms)\n",
				d.machine.frames[0], d.machine.frames[1], mono_ms() - t0);
	}

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
	err = dev_prepare(&d.dev, d.cfg.hd);
	if (!err)
		err = bring_up();
	if (err) {
		printf("the line chip did not come up: %s (closing the device holds the chip in reset)\n", strerror(-err));
		dev_close(&d.dev);
		free(d.board_file);
		return 1;
	}

	d.ans.fd = -1;
	call_ops_of(&d);
	modem_find(mono_ms());		/* a missing port is not a fault: the line is served and the port is looked for again */

	/* the HD state: the settings' choice, and whether the chip's wideband mode is on it */
	d.api.hd = d.cfg.hd ? (d.phone && phone_hd_active(d.phone) ? 2 : 1) : 0;
	config_load_book(&d.book, number_region_find(d.cfg.region), stdout);
	config_load_blocking(&d.block, number_region_find(d.cfg.region), stdout);
	config_load_alarms(&d.alarms, stdout);
	config_load_forward(&d.forward, stdout);
	calllog_init(&d.calls, CALLS_RAM_PATH, CALLS_FLASH_PATH, mono_ms());
	if (!calllog_load(&d.calls, stdout))
		printf("the call log was loaded\n");
	d.hooks = (struct phone_hooks){ .ctx = &d, .bridge_start = hook_bridge, .modem_level = hook_level, .event = hook_event, .call_ended = hook_call_ended, .wall_clock = hook_wall_clock };
	d.api = (struct api_ctx){ .my_number = d.cfg.my_number, .channel = d.cfg.channel, .at_interface = d.cfg.at_interface,
		.port = d.ans.port, .bridge_stats = api_bridge_stats, .reload = api_reload, .ctx = &d, .hd = d.cfg.hd,
		.record_path = d.cfg.record_path, .message_path = d.cfg.machine_path,
		.machine = &d.machine, .machine_delay_s = d.cfg.machine_delay_s };
	storage_measure(&d);		/* both folders made and proven before the first call, and the health reply told */
	phone_cfg_of(&d.cfg, &pc);	/* after the folders: the machine answers only when a message could be kept */
	indicator_init(&d.ind);
	indicator_apply(&d.ind, d.cfg.led, 0, 0, stdout);
	d.api.ind = &d.ind;
	err = phone_open(&d.phone, &d.chip, &d.board, &d.ops, &pc, &d.hooks, stdout);
	if (err) {
		printf("the voice path did not start: %s\n", strerror(-err));
		answer_close(&d.ans);
		dev_close(&d.dev);
		free(d.board_file);
		return 1;
	}

	d.api.phone = d.phone;
	d.api.calls = &d.calls;
	d.api.book = &d.book;
	d.api.start_ms = mono_ms();
	uloop_init();
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
	uloop_done();

	indicator_close(&d.ind);		/* the LED dark before the chip goes into reset */
	if (d.rec.open)
		recorder_abort(&d.rec);		/* the call died with the daemon: a half recording cannot be finished */
	if (d.rec_armed)
		dev_tap_set(&d.dev, 0);
	if (d.play_armed)
		dev_play_set(&d.dev, 0);	/* the greeting stops before the chip goes into reset */
	if (d.msg_armed)
		dev_tap_set(&d.dev, 0);
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
	free(d.board_file);
	printf("stopped (closing the device holds the chip in reset)\n");
	return d.exit_code;
}
