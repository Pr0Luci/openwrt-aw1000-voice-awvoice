// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * awvoice-phoned: the phone service. One thread, one event loop: a periodic 5 ms clock calls the call
 * core's phone_step(); the modem is pumped inside that step, so nothing else may block. The start-up is the stage
 * tool's --live path (the board's init section, stages 0..4, then the voice path); the settings come from UCI.
 *
 * Exit codes: 0 = asked to stop, or the service is switched off, or the line has to be set up again from the start
 * (a changed channel or board); 1 = a fault (procd starts the service again).
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
#include "phone.h"
#include "port.h"
#include "recorder.h"
#include "recording.h"
#include "script.h"
#include "slic_map.h"
#include "stages.h"

#define LOCK_PATH	"/var/run/awvoice-phoned.lock"
#define CALLS_RAM_PATH	"/var/run/awvoice-phoned/calls.json"
#define CALLS_FLASH_PATH "/etc/awvoice/calls.json"
#define INIT_SECTION	"init_nopatch"
#define BOARD_FILE_MAX	(64 * 1024)	/* far above a real board file; a larger file is not one */
#define RECORD_ENTRIES_MAX	256	/* the most recordings one prune looks at */

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
		struct awfxs_tap_frame frames[32];
		struct awfxs_tap_read meta;
		uint16_t samples[64];
		unsigned int got = dev_tap_read(&d.dev, frames, 32, &meta), k, n = 0;

		if (meta.generation != d.rec_generation) {
			/* the bridge restarted under the recording (an HD switch changes the rate with it):
			 * the part so far is kept and a new file starts at the new rate */
			long long saved = recorder_finish(&d.rec);

			if (saved >= 0)
				printf("the recording goes on in a new file after the rate change (%lld bytes kept)\n", saved);
			d.rec_rate = phone_hd_active(d.phone) && d.cfg.hd ? 16000 : 8000;
			snprintf(d.rec_name, sizeof(d.rec_name), "%lld-%u", api_epoch_s(now, now), d.rec_call_id);
			if (recorder_start(&d.rec, d.cfg.record_path, d.rec_name, (u32)d.rec_rate)) {
				dev_tap_set(&d.dev, 0);
				d.rec_armed = 0;
			}
		}
		if (d.rec.open && d.rec_armed) {
			for (k = 0; k < got; k++) {
				samples[n++] = frames[k].modem;			/* the far end: what a recording is for */
				if (meta.wideband && d.rec_rate == 16000)
					samples[n++] = frames[k].modem2;	/* the wideband second sample: the same voice, twice as often */
			}
			if (n && recorder_feed(&d.rec, samples, n)) {
				dev_tap_set(&d.dev, 0);	/* the failed feed discarded its tmp: the tap goes quiet with it */
				d.rec_armed = 0;
			}
		}
	}
	api_tick(&d.api);
	if (d.calls.ram_dirty || d.calls.flash_dirty) {
		struct phone_status st;

		phone_status(d.phone, &st);		/* files are written only while nobody uses the line: a flash write may take a moment */
		if (st.line == PHONE_LINE_IDLE && !st.off_hook && calllog_persist(&d.calls, now, 0))
			printf("the call log could not be saved\n");
	}
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
	d.api.hd = now.hd ? (d.phone && phone_hd_active(d.phone) ? 2 : 1) : 0;
	indicator_apply(&d.ind, now.led, 0, 0, stdout);	/* only a change acts */
	config_load_blocking(&d.block, number_region_find(now.region), stdout);
	config_load_alarms(&d.alarms, stdout);
	config_load_forward(&d.forward, stdout);
	config_load_book(&d.book, number_region_find(now.region), stdout);	/* between two steps: the core sees the new book whole */
	d.api.at_interface = now.at_interface;
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

	calllog_add(&dm->calls, rec, api_epoch_s(rec->start_ms, mono_ms()));
}

/* ---- call recording (awvoice.record.*): the files' names hold an epoch and the call's id, never a number ------------- */

/* when the directory is past its budget the oldest recordings go, before a new one starts (the names sort by time) */
static void record_prune(const char *dir, unsigned int budget_mb)
{
	char names[256][64];
	u32 sizes[256];
	unsigned long long budget = (unsigned long long)budget_mb * 1024 * 1024;
	unsigned int n = 0, k, delete;
	struct dirent *e;
	DIR *d = opendir(dir);

	if (!d)
		return;
	while ((e = readdir(d)) != NULL && n < RECORD_ENTRIES_MAX) {
		char path[AW_CONFIG_PATH_MAX + 80];
		struct stat st;
		size_t len = strlen(e->d_name);

		if (len < 5 || strcmp(e->d_name + len - 4, ".wav") ||
		    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path) ||
		    stat(path, &st) || !S_ISREG(st.st_mode))
			continue;
		memcpy(names[n], e->d_name, len + 1);
		sizes[n] = (u32)st.st_size;
		n++;
	}
	closedir(d);
	delete = recording_prune_count(sizes, n, budget <= 0xffffffffu ? (u32)budget : 0xffffffffu);
	for (k = 0; k < delete; k++) {
		char path[AW_CONFIG_PATH_MAX + 80];

		if (snprintf(path, sizeof(path), "%s/%s", dir, names[k]) < (int)sizeof(path))
			(void)unlink(path);
	}
}

/* the tap frames of one call: on answered, arm the kernel tap and open the file; on the end, close and keep it */
static void record_call_event(struct daemon *dm, const struct phone_event *ev)
{
	struct awfxs_tap_frame frames[32];
	struct awfxs_tap_read meta;
	int err;

	if (!dm->cfg.record_enabled)
		return;
	if (ev->call == CALL_ACTIVE && !dm->rec.open) {
		record_prune(dm->cfg.record_path, dm->cfg.record_max_mb);
		dm->rec_rate = phone_hd_active(dm->phone) && dm->cfg.hd ? 16000 : 8000;
		snprintf(dm->rec_name, sizeof(dm->rec_name), "%lld-%u",
			api_epoch_s(ev->at_ms, mono_ms()), ev->call_id);
		dm->rec_call_id = ev->call_id;
		err = dev_tap_set(&dm->dev, 1);
		if (!err)
			err = recorder_start(&dm->rec, dm->cfg.record_path, dm->rec_name, (u32)dm->rec_rate);
		if (err) {
			dev_tap_set(&dm->dev, 0);
			printf("the call was not recorded: %s\n", strerror(-err));
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
		else
			printf("the recording failed: %s\n", strerror((int)-saved));
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

int main(void)
{
	struct phone_cfg pc;
	size_t size = 0;
	int err;

	setvbuf(stdout, NULL, _IOLBF, 0);	/* every line reaches the log even if the board resets mid-run */
	if (single_instance() < 0)
		return 1;

	config_load(&d.cfg, stdout);
	if (!d.cfg.enabled) {
		printf("the phone service is switched off (awvoice.main.enabled)\n");
		return 0;
	}

	/* a restart in the middle of a recording left a tmp file: it can never be finished, so it goes */
	{
		DIR *dir = opendir(d.cfg.record_path);
		struct dirent *e;

		while (dir && (e = readdir(dir)) != NULL) {
			char path[AW_CONFIG_PATH_MAX + 80];
			size_t len = strlen(e->d_name);

			if (len > 4 && !strcmp(e->d_name + len - 4, ".tmp") &&
			    snprintf(path, sizeof(path), "%s/%s", d.cfg.record_path, e->d_name) < (int)sizeof(path))
				(void)unlink(path);
		}
		if (dir)
			closedir(dir);
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
	phone_cfg_of(&d.cfg, &pc);
	d.hooks = (struct phone_hooks){ .ctx = &d, .bridge_start = hook_bridge, .modem_level = hook_level, .event = hook_event, .call_ended = hook_call_ended, .wall_clock = hook_wall_clock };
	d.api = (struct api_ctx){ .my_number = d.cfg.my_number, .channel = d.cfg.channel, .at_interface = d.cfg.at_interface,
		.port = d.ans.port, .bridge_stats = api_bridge_stats, .reload = api_reload, .ctx = &d, .hd = d.cfg.hd };
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
