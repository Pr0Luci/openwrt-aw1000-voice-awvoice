/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_CONFIG_H
#define AW_PHONED_CONFIG_H

#include <stdio.h>

/*
 * The daemon's settings: UCI package awvoice, section `main` (type phone). The phone page (LuCI app) writes the same
 * package; the option names are shared with it. Every option has a default; a missing option takes
 * it silently, an invalid one takes it with one log line naming the option (never failing the start: a phone must
 * come up even with a bad setting). Ranges follow the phone page and hookwatch.
 *
 * What this build reads:
 *   option            default                    range / values                     what it does
 *   enabled           1                          0 | 1                              0: the init script does not start the daemon
 *   channel           1                          0 | 1                              the line chip's channel of the jack
 *   board             /etc/awvoice/board.bin    a readable file                    the board file
 *   at_interface      2                          0..9                               the modem's USB interface of the AT port
 *   flash_min_ms      80                         20..500                            hook flash limits
 *   flash_max_ms      300                        300..3000, >= flash_min_ms + 100
 *   dial_pause_s      6                          3..15                              the dial goes out this long after the last digit
 *   cid               dtmf                       dtmf | bellcore | etsi | off        caller id to the handset: the DTMF variant is the one verified on this board; the FSK ones carry the date and time and the name (TO-VERIFY on the handset)
 *   cid_when          ring1                      ring1 | prering                    where the FSK message goes: between the first and the second ring burst, or before the first ring (the European placement); DTMF ignores it
 *   ringback          auto                       auto | on | off (1 = on, 0 = off)  the local ringing tone while the remote rings: auto = until the network's own audio is heard, then never again in that call
 *   earpiece_volume   (unset = leave the modem)  the range the modem reports         AT+CLVL (3GPP TS 27.007) at every modem setup
 *   modem_rx_gain     (unset = leave the modem)  0..65535 (8192 = unity)            AT+QRXGAIN at every modem setup
 *   level_trace       0                          0 | 1                              measuring only: log the modem's audio level every 100 ms while a call is set up
 *   my_number         ""                         the number rule                    shown on the status page only; never dialled
 *   clir              network                    network | hide | show              AT+CLIR=0|1|2 at every modem setup (TS 27.007); the read-back is in health.modem.clir
 *   hotline_number    ""                         the number rule, at most 20 chars  dialled when the lifted handset dials nothing for hotline_delay
 *   hotline_delay     10                         3..99 s                            the wait for the hotline (the first key ends it)
 *   call_waiting      1                          0 | 1                              a second incoming call during a call: 1 = the waiting tone and the flash keys, 0 = refused (AT+CHLD=0)
 *   conference        0                          0 | 1                              allows the join of two calls (flash and 3)
 *   region            auto                       auto, or a name of the number table   how national numbers become international (core/numbers.h)
 *   hd                0                          0 | 1                              HD voice (16 kHz) is wanted; the line is brought up at it only once the modem answers with that rate, and until then the health reply reports hdVoice "set"
 *   record_enabled    0                          0 | 1                              1 = calls you answer are recorded as WAV files (16-bit PCM, the call's own rate); a machine-answered call keeps its message instead (no recording after a handset take-over); off by default: a recording law may ask for an announcement
 *   record_max_mb     50                         1..4095                            the recordings directory's budget; when it is passed the oldest files are deleted before a new one starts
 *   record_path       /etc/awvoice/recordings    a directory                        where the WAV files land; a mount can be named for more room; trailing slashes go, the root is refused, and a /mnt folder is used only once its drive is mounted
 *   machine_enabled   0                          0 | 1                              1 = an unanswered incoming call is answered by the answering machine after machine_delay (the cellular leg only); a message folder that cannot take a file keeps the machine from answering, and the handset rings on. The greeting it plays is the one managed file machine.h names: the package ships a default there and the page records or uploads over it, so its path is not a setting
 *   machine_delay     10                         0..60 s                            the wait before the machine answers, counted from the call's first listing; 0 answers at once
 *   machine_path      /etc/awvoice/messages      a directory                        where the messages land; a mount can be named for more room; never the recordings' folder, and a /mnt folder is used only once its drive is mounted
 *   machine_max_mb    50                         1..4095                            the messages directory's budget (the call recordings have their own)
 *   machine_max_s     120                        10..600 s                          the longest message; the call is ended when it is reached
 *   web_port          7443                       1024..65535                        the page's browser-audio listener (WSS, LAN address only); opened while the line is up, whichever side the SIP mode names
 *   web_cert          /etc/uhttpd.crt            a readable PEM file                the certificate of that listener; the router's own HTTPS certificate by default
 *   web_key           /etc/uhttpd.key            a readable PEM key                 its private key
 *
 * Shared with the phone page and read here (listed so the names stay fixed; the page sends the same ranges): mic_gain_db,
 * earpiece_gain_db (the line chip's gains); sections `contact`, `blocking 'blocking'`, `forwarding 'forwarding'`, `alarm`,
 * and the quiet hours `dnd` (config_load_dnd: enabled 0|1, from and to HH:MM, all_day 0|1, days a list of
 * sun|mon|tue|wed|thu|fri|sat, mode silent|machine|reject, repeat_min 0..1440, allow a list of numbers; a contact that
 * rings through quiet hours carries option ring_through 1 and needs nothing here).
 *
 * The indicator (the phone LED): option led ("" = dark, a name of /sys/class/leds),
 * ring_rate_ms (300, 100..3000: the fast blink of an incoming call), offhook_rate_ms (1000, 200..10000: the slow
 * blink while the handset is up). Solid while ready (the modem answers and both registrations hold), dark otherwise.
 */

#define AW_CONFIG_PATH_MAX	128
#define AW_RINGBACK_AUTO	0	/* the values of struct aw_config.ringback (the same numbers as enum phone_ringback) */
#define AW_RINGBACK_ON		1
#define AW_RINGBACK_OFF		2
#define AW_CONFIG_UNSET		(-1)	/* an optional modem setting that is left as the modem has it */

#define AW_CID_DTMF		0	/* the values of struct aw_config.cid_kind (the caller-id variants of cid.h) */
#define AW_CID_OFF		1
#define AW_CID_BELLCORE		2
#define AW_CID_ETSI		3

struct aw_config {
	int enabled;
	unsigned int channel;
	char board[AW_CONFIG_PATH_MAX];
	unsigned int at_interface;
	unsigned int flash_min_ms, flash_max_ms;
	unsigned int dial_pause_s;
	int cid_kind;			/* AW_CID_*: the caller-id variant to the handset */
	int cid_prering;		/* 1: the FSK message goes out before the first ring burst (the European placement) */
	int ringback;
	int level_trace;
	int earpiece_volume;		/* AW_CONFIG_UNSET or the AT+CLVL level */
	int modem_rx_gain;		/* AW_CONFIG_UNSET or the AT+QRXGAIN value */
	char my_number[33];
	int clir;			/* the <n> of AT+CLIR=<n>: 0 network, 1 hide, 2 show */
	char hotline_number[33];	/* "" = no hotline */
	unsigned int hotline_delay_s;
	int call_waiting;		/* 1 = a second incoming call during a call is offered to the user (AT+CCWA=1 at the modem setup); 0 = refused */
	int conference;			/* 1 = the join of two calls (three-way) is allowed */
	char region[8];			/* "auto" (the plan of the network the line registers on) or a name of the number table (numbers.h); auto by default */
	char led[64];			/* an LED of /sys/class/leds for the line state, "" = dark (the indicator) */
	unsigned int ring_rate_ms;	/* the indicator's fast blink (an incoming call), 100..3000 */
	unsigned int offhook_rate_ms;	/* the indicator's slow blink (the handset up), 200..10000 */
	int hd;				/* 1 = HD voice (16 kHz) is wanted; a wish until the modem answers with that rate and the line is brought up at it (see the table above) */
	int record_enabled;		/* 1 = calls you answer are recorded as WAV files (a machine-answered call keeps its message; no recording after a take-over; opt-in: a recording law may ask for an announcement) */
	unsigned int record_max_mb;	/* the recordings directory's budget in MB; the oldest files go first */
	char record_path[AW_CONFIG_PATH_MAX];	/* where the WAV files land; a mount can be named for more room */
	int machine_enabled;		/* 1 = the answering machine may answer an offered incoming call (machine.h holds the greeting it plays) */
	unsigned int machine_delay_s;	/* the wait before the machine answers, 0..60 s */
	char machine_path[AW_CONFIG_PATH_MAX];	/* where the messages land */
	unsigned int machine_max_mb;	/* the messages directory's budget in MB */
	unsigned int machine_max_s;	/* the longest message in seconds; the call ends when it is reached */
	char sip_mode[16];		/* awvoice.sip.mode: web, interop or baresip. web leaves the line to the page and the handset; interop and baresip ask for a network phone beside them, the two differing only in what serves that side */
	unsigned int web_port;		/* the browser-audio listener's port (awvoice.main.web_port) */
	char web_iface[16];		/* the network whose IPv4 address it binds (awvoice.main.web_iface): never a WAN side */
	char web_cert[AW_CONFIG_PATH_MAX];	/* its certificate (awvoice.main.web_cert) */
	char web_key[AW_CONFIG_PATH_MAX];	/* its private key (awvoice.main.web_key) */
};

/* the defaults above */
void config_defaults(struct aw_config *c);

/*
 * Set one option from its UCI text (name as in the table). Returns 0 when taken, -EINVAL when the value is
 * invalid (the default stays and one line goes to log, naming the option but never echoing a number), -ENOENT for a
 * name the daemon does not use (ignored by the caller: an option this build does not read, and the page's own). Pure: no UCI, so
 * the host tests cover every rule.
 */
int config_set(struct aw_config *c, const char *name, const char *value, FILE *log);

/* Set the SIP mode from awvoice.sip.mode's text: web, interop or baresip. 0 when taken; -EINVAL for a word
 * this build does not run (the mode it had stays). NULL (no such option) is web, the page's own line. Pure, like
 * config_set. */
int config_set_sip(struct aw_config *c, const char *value, FILE *log);

/* the cross-option rule (flash_max_ms >= flash_min_ms + 100), applied after all options are set */
void config_finish(struct aw_config *c, FILE *log);

struct phonebook;
struct dnd;
struct phone_forward;
struct alarms;
struct blocking;
struct number_region;

/* the `contact` sections of /etc/config/awvoice into the phone book (cleared first): option name, list number '<value>:<type>', option quick,
 * option ring_through (only `1` asks that this caller rings through quiet hours).
 * A contact that is not valid is skipped with one log line that names the reason, never the name or the number. 0, or -ENOMEM. */
int config_load_book(struct phonebook *pb, const struct number_region *region, FILE *log);

/* the (first) section of type `blocking` into the rules (cleared first): option anonymous 0|1, lists incoming, incoming_prefix, outgoing, outgoing_prefix.
 * The section is found by its type: the name `main` belongs to the settings section (UCI names are unique in a package). A rule that is not valid is
 * skipped with one log line that never holds the number. 0, or -ENOMEM. */
int config_load_blocking(struct blocking *b, const struct number_region *region, FILE *log);

/* the sections of type `alarm` into the alarms (cleared first): option label, time (HH:MM), list day (1 = Monday .. 7), option enabled (0|1, default 1).
 * An alarm that is not valid is skipped with one log line that does not echo the label. 0, or -ENOMEM. */
int config_load_alarms(struct alarms *a, FILE *log);

/* the (first) section of type `forwarding`: option enabled 0|1, to (digits, a leading +, at most 20), when (always | busy | noanswer | unreachable | all_conditional),
 * delay (5..30, steps of 5, default 20). A section with an invalid value is not used at all (logged without the number), so that a typing error never
 * erases the forwarding the network holds. configured = 0 when there is no usable section. 0, or -ENOMEM. */
int config_load_forward(struct phone_forward *f, FILE *log);

/* the quiet hours: every section of type `dnd`, in the order the file writes them, and the numbers that ring through */
int config_load_dnd(struct dnd *d, const struct number_region *region, FILE *log);

/* read /etc/config/awvoice through libuci: defaults, then config_set for each option, then config_finish */
int config_load(struct aw_config *c, FILE *log);

#endif /* AW_PHONED_CONFIG_H */
