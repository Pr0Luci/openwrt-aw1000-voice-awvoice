/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_CONFIG_H
#define AW_PHONED_CONFIG_H

#include <stdio.h>

/*
 * The daemon's settings: UCI package awvoice, section `main` (type phone). The phone page (LuCI app) writes the same
 * package; the option names are shared with it (DESIGN.md §14). Every option has a default; a missing option takes
 * it silently, an invalid one takes it with one log line naming the option (never failing the start: a phone must
 * come up even with a bad setting). Ranges follow the phone page and hookwatch.
 *
 * Read and used in M4:
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
 *   region            LK                         a region of the number table       how national numbers become international (core/numbers.h)
 *   hd                0                          0 | 1                              HD voice (16 kHz) is wanted; while the wideband line path is not built (while the wideband path is pending) the line stays at 8 kHz and the health reply reports hdVoice "set"
 *
 * Shared with the phone page, read by the daemon in M5 (listed so the names stay fixed; the page's DESIGN.md §2 and §5 give
 * their ranges): mic_gain_db,
 * earpiece_gain_db (the line chip's gains); sections `contact`, `blocking 'blocking'`, `forwarding 'forwarding'`, `alarm`.
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
	char region[8];			/* a name of the number table (numbers.h); LK by default */
	char led[64];			/* an LED of /sys/class/leds for the line state, "" = dark (the indicator) */
	unsigned int ring_rate_ms;	/* the indicator's fast blink (an incoming call), 100..3000 */
	unsigned int offhook_rate_ms;	/* the indicator's slow blink (the handset up), 200..10000 */
	int hd;				/* 1 = HD voice (16 kHz) is wanted; the wideband path is pending (see the table above) */
};

/* the defaults above */
void config_defaults(struct aw_config *c);

/*
 * Set one option from its UCI text (name as in the table). Returns 0 when taken, -EINVAL when the value is
 * invalid (the default stays and one line goes to log, naming the option but never echoing a number), -ENOENT for a
 * name the daemon does not use (ignored by the caller: the M5 options and the page's own options). Pure: no UCI, so
 * the host tests cover every rule.
 */
int config_set(struct aw_config *c, const char *name, const char *value, FILE *log);

/* the cross-option rule (flash_max_ms >= flash_min_ms + 100), applied after all options are set */
void config_finish(struct aw_config *c, FILE *log);

struct phonebook;
struct phone_forward;
struct alarms;
struct blocking;
struct number_region;

/* the `contact` sections of /etc/config/awvoice into the phone book (cleared first): option name, list number '<value>:<type>', option quick.
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

/* read /etc/config/awvoice through libuci: defaults, then config_set for each option, then config_finish */
int config_load(struct aw_config *c, FILE *log);

#endif /* AW_PHONED_CONFIG_H */
