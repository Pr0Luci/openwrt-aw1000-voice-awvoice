// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <string.h>

#include "config.h"
#include "hookwatch.h"
#include "machine.h"		/* the greeting's one path, which is no longer a setting */
#include "numbers.h"
#include "phone.h"
#include "region_follow.h"
#include "storage.h"

#define DEFAULT_BOARD		"/etc/awvoice/board.bin"
#define DEFAULT_AT_INTERFACE	2
#define DEFAULT_DIAL_PAUSE_S	6
#define FLASH_GAP_MIN_MS	100	/* flash_max_ms >= flash_min_ms + this (hookwatch_cfg_parse) */
#define GAIN_MAX		65535
#define VOLUME_MAX		255
#define DEFAULT_HOTLINE_DELAY_S	10
#define HOTLINE_LEN_MAX		20	/* what the dial buffer holds (DIAL_NUMBER_MAX) */
#define DEFAULT_MACHINE_DELAY_S	10
#define DEFAULT_RECORD_PATH	"/etc/awvoice/recordings"
#define DEFAULT_MACHINE_PATH	"/etc/awvoice/messages"
#define DEFAULT_WEB_PORT	7443
#define DEFAULT_WEB_IFACE	"lan"
#define DEFAULT_WEB_CERT	"/etc/uhttpd.crt"
#define DEFAULT_WEB_KEY		"/etc/uhttpd.key"
#define LED_NAME_MAX		63	/* a name of /sys/class/leds */
#define RING_RATE_DEFAULT_MS	300	/* the fast blink: an incoming call */
#define RING_RATE_MAX_MS	3000
#define OFFHOOK_RATE_DEFAULT_MS	1000	/* the slow blink: the handset up */
#define OFFHOOK_RATE_MAX_MS	10000

void config_defaults(struct aw_config *c)
{
	memset(c, 0, sizeof(*c));
	c->enabled = 1;
	c->channel = 1;
	snprintf(c->board, sizeof(c->board), "%s", DEFAULT_BOARD);
	c->at_interface = DEFAULT_AT_INTERFACE;
	c->flash_min_ms = HOOKWATCH_FLASH_MIN_DEFAULT_MS;
	c->flash_max_ms = HOOKWATCH_FLASH_MAX_DEFAULT_MS;
	c->dial_pause_s = DEFAULT_DIAL_PAUSE_S;
	c->cid_kind = AW_CID_DTMF;
	c->ringback = AW_RINGBACK_AUTO;
	c->earpiece_volume = AW_CONFIG_UNSET;
	c->modem_rx_gain = AW_CONFIG_UNSET;
	/* the numbering plan follows the network the line registers on; a name in the table overrides that */
	snprintf(c->region, sizeof(c->region), "%s", REGION_AUTO);
	c->hotline_delay_s = DEFAULT_HOTLINE_DELAY_S;
	c->call_waiting = 1;
	c->ring_rate_ms = RING_RATE_DEFAULT_MS;
	c->offhook_rate_ms = OFFHOOK_RATE_DEFAULT_MS;
	c->record_max_mb = 50;
	snprintf(c->record_path, sizeof(c->record_path), "%s", DEFAULT_RECORD_PATH);
	c->machine_delay_s = DEFAULT_MACHINE_DELAY_S;
	snprintf(c->machine_path, sizeof(c->machine_path), "%s", DEFAULT_MACHINE_PATH);
	c->machine_max_mb = 50;
	c->machine_max_s = 120;
	snprintf(c->sip_mode, sizeof(c->sip_mode), "%s", "web");
	c->web_port = DEFAULT_WEB_PORT;
	snprintf(c->web_iface, sizeof(c->web_iface), "%s", DEFAULT_WEB_IFACE);
	snprintf(c->web_cert, sizeof(c->web_cert), "%s", DEFAULT_WEB_CERT);
	snprintf(c->web_key, sizeof(c->web_key), "%s", DEFAULT_WEB_KEY);
}

/* a whole decimal number in [low, high]: no sign, no space, no trailing text */
static int number_in(const char *text, unsigned int low, unsigned int high, unsigned int *out)
{
	unsigned long v = 0;

	if (!*text)
		return 0;
	for (; *text; text++) {
		if (*text < '0' || *text > '9')
			return 0;
		v = v * 10 + (unsigned long)(*text - '0');
		if (v > high)
			return 0;
	}
	if (v < low)
		return 0;
	*out = (unsigned int)v;
	return 1;
}

/* the page's number rule: digits, '*', '#', a leading '+', at most PHONE_NUMBER_MAX characters */
static int number_rule(const char *text)
{
	size_t n = strlen(text), k;

	if (n > PHONE_NUMBER_MAX)
		return 0;
	for (k = 0; k < n; k++)
		if (!((text[k] >= '0' && text[k] <= '9') || text[k] == '*' || text[k] == '#' || (k == 0 && text[k] == '+')))
			return 0;
	return 1;
}

static int bad(FILE *log, const char *name)
{
	if (log)
		fprintf(log, "setting %s is not valid: the default is used\n", name);
	return -EINVAL;
}

/* an interface name as the kernel takes one: letters, digits, '_', '-', '.', and no longer than IFNAMSIZ - 1 */
static int iface_name_ok(const char *s)
{
	const char *p;

	if (!*s)
		return 0;
	for (p = s; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
		      *p == '_' || *p == '-' || *p == '.'))
			return 0;
	return 1;
}

int config_set(struct aw_config *c, const char *name, const char *value, FILE *log)
{
	unsigned int v;

	if (!strcmp(name, "enabled")) {
		if (!number_in(value, 0, 1, &v))
			return bad(log, name);
		c->enabled = (int)v;
	} else if (!strcmp(name, "channel")) {
		if (!number_in(value, 0, 1, &v))
			return bad(log, name);
		c->channel = v;
	} else if (!strcmp(name, "board")) {
		if (value[0] != '/' || strlen(value) >= sizeof(c->board))
			return bad(log, name);
		snprintf(c->board, sizeof(c->board), "%s", value);
	} else if (!strcmp(name, "at_interface")) {
		if (!number_in(value, 0, 9, &v))
			return bad(log, name);
		c->at_interface = v;
	} else if (!strcmp(name, "flash_min_ms")) {
		if (!number_in(value, 20, 500, &v))
			return bad(log, name);
		c->flash_min_ms = v;
	} else if (!strcmp(name, "flash_max_ms")) {
		if (!number_in(value, 300, 3000, &v))
			return bad(log, name);
		c->flash_max_ms = v;
	} else if (!strcmp(name, "dial_pause_s")) {
		if (!number_in(value, 3, 15, &v))
			return bad(log, name);
		c->dial_pause_s = v;
	} else if (!strcmp(name, "cid")) {
		if (!strcmp(value, "dtmf"))
			c->cid_kind = AW_CID_DTMF;
		else if (!strcmp(value, "off"))
			c->cid_kind = AW_CID_OFF;
		else if (!strcmp(value, "bellcore"))
			c->cid_kind = AW_CID_BELLCORE;
		else if (!strcmp(value, "etsi"))
			c->cid_kind = AW_CID_ETSI;
		else
			return bad(log, name);
	} else if (!strcmp(name, "cid_when")) {
		if (!strcmp(value, "ring1"))
			c->cid_prering = 0;
		else if (!strcmp(value, "prering"))
			c->cid_prering = 1;
		else
			return bad(log, name);
	} else if (!strcmp(name, "ringback")) {
		/* auto | on | off; the earlier 1 and 0 are on and off */
		if (!strcmp(value, "auto"))
			c->ringback = AW_RINGBACK_AUTO;
		else if (!strcmp(value, "on") || !strcmp(value, "1"))
			c->ringback = AW_RINGBACK_ON;
		else if (!strcmp(value, "off") || !strcmp(value, "0"))
			c->ringback = AW_RINGBACK_OFF;
		else
			return bad(log, name);
	} else if (!strcmp(name, "level_trace")) {
		if (!number_in(value, 0, 1, &v))
			return bad(log, name);
		c->level_trace = (int)v;
	} else if (!strcmp(name, "earpiece_volume")) {
		if (!*value) {
			c->earpiece_volume = AW_CONFIG_UNSET;
		} else {
			if (!number_in(value, 0, VOLUME_MAX, &v))
				return bad(log, name);
			c->earpiece_volume = (int)v;
		}
	} else if (!strcmp(name, "modem_rx_gain")) {
		if (!*value) {
			c->modem_rx_gain = AW_CONFIG_UNSET;
		} else {
			if (!number_in(value, 0, GAIN_MAX, &v))
				return bad(log, name);
			c->modem_rx_gain = (int)v;
		}
	} else if (!strcmp(name, "my_number")) {
		if (!number_rule(value))
			return bad(log, name);
		snprintf(c->my_number, sizeof(c->my_number), "%s", value);
	} else if (!strcmp(name, "clir")) {
		if (!strcmp(value, "network"))
			c->clir = 0;
		else if (!strcmp(value, "hide"))
			c->clir = 1;
		else if (!strcmp(value, "show"))
			c->clir = 2;
		else
			return bad(log, name);
	} else if (!strcmp(name, "hotline_number")) {
		if (!number_rule(value) || strlen(value) > HOTLINE_LEN_MAX)
			return bad(log, name);
		snprintf(c->hotline_number, sizeof(c->hotline_number), "%s", value);
	} else if (!strcmp(name, "hotline_delay")) {
		if (!number_in(value, 3, 99, &v))
			return bad(log, name);
		c->hotline_delay_s = v;
	} else if (!strcmp(name, "call_waiting")) {
		if (!number_in(value, 0, 1, &v))
			return bad(log, name);
		c->call_waiting = (int)v;
	} else if (!strcmp(name, "conference")) {
		if (!number_in(value, 0, 1, &v))
			return bad(log, name);
		c->conference = (int)v;
	} else if (!strcmp(name, "region")) {
		if (strcmp(value, REGION_AUTO) && !number_region_find(value))
			return bad(log, name);
		snprintf(c->region, sizeof(c->region), "%s", value);
	} else if (!strcmp(name, "led")) {
		size_t k;

		if (strlen(value) >= LED_NAME_MAX)
			return bad(log, name);
		for (k = 0; value[k]; k++)
			if (!((value[k] >= '0' && value[k] <= '9') || (value[k] >= 'a' && value[k] <= 'z') ||
			      (value[k] >= 'A' && value[k] <= 'Z') || value[k] == ':' || value[k] == '_' || value[k] == '-'))
				return bad(log, name);
		snprintf(c->led, sizeof(c->led), "%s", value);
	} else if (!strcmp(name, "ring_rate_ms")) {
		if (!number_in(value, 100, RING_RATE_MAX_MS, &v))
			return bad(log, name);
		c->ring_rate_ms = v;
	} else if (!strcmp(name, "offhook_rate_ms")) {
		if (!number_in(value, 200, OFFHOOK_RATE_MAX_MS, &v))
			return bad(log, name);
		c->offhook_rate_ms = v;
	} else if (!strcmp(name, "hd")) {
		if (!number_in(value, 0, 1, &v))
			return bad(log, name);
		c->hd = (int)v;
	} else if (!strcmp(name, "record_enabled")) {
		if (!number_in(value, 0, 1, &v))
			return bad(log, name);
		c->record_enabled = (int)v;
	} else if (!strcmp(name, "record_max_mb")) {
		if (!number_in(value, 1, 4095, &v))	/* 4096 MB does not fit the byte budget's 32 bits */
			return bad(log, name);
		c->record_max_mb = v;
	} else if (!strcmp(name, "record_path")) {
		char path[sizeof(c->record_path)];

		if (value[0] != '/' || strlen(value) >= sizeof(path))
			return bad(log, name);
		snprintf(path, sizeof(path), "%s", value);
		if (storage_path_norm(path, sizeof(path)))
			return bad(log, name);		/* the root, or a line break in the path */
		snprintf(c->record_path, sizeof(c->record_path), "%s", path);
	} else if (!strcmp(name, "machine_enabled")) {
		if (!number_in(value, 0, 1, &v))
			return bad(log, name);
		c->machine_enabled = (int)v;
	} else if (!strcmp(name, "machine_greeting")) {
		/*
		 * Retired: the greeting is the one file the page manages, at the path machine.h names, and the package puts
		 * its default there when nothing is. A stored path is ignored rather than followed, because a file the
		 * machine plays should not be reachable from a config line the page no longer offers - and the line in the
		 * log says so, so nobody hunts for why their old path stopped working.
		 */
		if (log)
			fprintf(log, "awvoice.main.machine_greeting is not a setting any more: the greeting is %s, recorded or uploaded on the Phone settings page\n",
				MACHINE_GREETING_PATH);
		return -ENOENT;		/* nothing was stored in the config this build reads */
	} else if (!strcmp(name, "machine_delay")) {
		if (!number_in(value, 0, 60, &v))
			return bad(log, name);
		c->machine_delay_s = v;
	} else if (!strcmp(name, "machine_path")) {
		char path[sizeof(c->machine_path)];

		if (value[0] != '/' || strlen(value) >= sizeof(path))
			return bad(log, name);
		snprintf(path, sizeof(path), "%s", value);
		if (storage_path_norm(path, sizeof(path)))
			return bad(log, name);
		snprintf(c->machine_path, sizeof(c->machine_path), "%s", path);
	} else if (!strcmp(name, "machine_max_mb")) {
		if (!number_in(value, 1, 4095, &v))
			return bad(log, name);
		c->machine_max_mb = v;
	} else if (!strcmp(name, "machine_max_s")) {
		if (!number_in(value, 10, 600, &v))
			return bad(log, name);
		c->machine_max_s = v;
	} else if (!strcmp(name, "web_port")) {
		if (!number_in(value, 1024, 65535, &v))
			return bad(log, name);
		c->web_port = v;
	} else if (!strcmp(name, "web_iface")) {
		/* an interface name, not a path or an address: the daemon asks the network for its IPv4 address */
		if (!value[0] || strlen(value) >= sizeof(c->web_iface) || !iface_name_ok(value))
			return bad(log, name);
		snprintf(c->web_iface, sizeof(c->web_iface), "%s", value);
	} else if (!strcmp(name, "web_cert")) {
		if (value[0] != '/' || strlen(value) >= sizeof(c->web_cert))
			return bad(log, name);
		snprintf(c->web_cert, sizeof(c->web_cert), "%s", value);
	} else if (!strcmp(name, "web_key")) {
		if (value[0] != '/' || strlen(value) >= sizeof(c->web_key))
			return bad(log, name);
		snprintf(c->web_key, sizeof(c->web_key), "%s", value);
	} else {
		return -ENOENT;
	}
	return 0;
}

int config_set_sip(struct aw_config *c, const char *value, FILE *log)
{
	/* the three words this build knows: one for the line's own phones, two for a network phone beside them. Another
	 * word is not a side it has, so the configured mode stays rather than the line going to something that is not
	 * there; an unset option is web, the side that asks nothing of any other program */
	if (!value || !strcmp(value, "web") || !strcmp(value, "interop") || !strcmp(value, "baresip")) {
		snprintf(c->sip_mode, sizeof(c->sip_mode), "%s", value ? value : "web");
		return 0;
	}
	return bad(log, "sip.mode");
}

void config_finish(struct aw_config *c, FILE *log)
{
	/* the two folders: an absolute path without its trailing slashes, never the root, and never the same folder
	 * for both — one prune and one "Delete all" would otherwise act on the other feature's files */
	if (storage_path_norm(c->record_path, sizeof(c->record_path))) {
		if (log)
			fprintf(log, "the recording folder is not a usable path: %s; the default %s is used\n",
				c->record_path, DEFAULT_RECORD_PATH);
		snprintf(c->record_path, sizeof(c->record_path), "%s", DEFAULT_RECORD_PATH);
	}
	if (storage_path_norm(c->machine_path, sizeof(c->machine_path))) {
		if (log)
			fprintf(log, "the message folder is not a usable path: %s; the default %s is used\n",
				c->machine_path, DEFAULT_MACHINE_PATH);
		snprintf(c->machine_path, sizeof(c->machine_path), "%s", DEFAULT_MACHINE_PATH);
	}
	if (!strcmp(c->record_path, c->machine_path)) {
		char was[sizeof(c->machine_path)];

		snprintf(was, sizeof(was), "%s", c->machine_path);
		if (strcmp(DEFAULT_MACHINE_PATH, c->record_path))
			snprintf(c->machine_path, sizeof(c->machine_path), "%s", DEFAULT_MACHINE_PATH);
		else if (strlen(c->record_path) + 10 < sizeof(c->machine_path))
			snprintf(c->machine_path, sizeof(c->machine_path), "%s/.messages", c->record_path);
		if (!strcmp(c->record_path, c->machine_path)) {
			/* no folder can be told apart from the recordings: the machine stays off rather than write there */
			if (log)
				fprintf(log, "the recordings and the messages would share a folder: the answering machine is switched off\n");
			c->machine_enabled = 0;
		} else if (log) {
			fprintf(log, "the recordings and the messages were both set to %s; the messages go to %s\n",
				was, c->machine_path);
		}
	}
	if (c->flash_max_ms >= c->flash_min_ms + FLASH_GAP_MIN_MS)
		return;
	if (log)
		fprintf(log, "flash_max_ms must be at least flash_min_ms + %u: both defaults are used\n", FLASH_GAP_MIN_MS);
	c->flash_min_ms = HOOKWATCH_FLASH_MIN_DEFAULT_MS;
	c->flash_max_ms = HOOKWATCH_FLASH_MAX_DEFAULT_MS;
}
