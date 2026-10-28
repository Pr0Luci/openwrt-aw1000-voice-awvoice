// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <string.h>

#include "config.h"
#include "hookwatch.h"
#include "numbers.h"
#include "phone.h"

#define DEFAULT_BOARD		"/etc/awvoice/board.bin"
#define DEFAULT_AT_INTERFACE	2
#define DEFAULT_DIAL_PAUSE_S	6
#define FLASH_GAP_MIN_MS	100	/* flash_max_ms >= flash_min_ms + this (hookwatch_cfg_parse) */
#define GAIN_MAX		65535
#define VOLUME_MAX		255
#define DEFAULT_HOTLINE_DELAY_S	10
#define HOTLINE_LEN_MAX		20	/* what the dial buffer holds (DIAL_NUMBER_MAX) */
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
	snprintf(c->region, sizeof(c->region), "%s", number_region_default()->name);
	c->hotline_delay_s = DEFAULT_HOTLINE_DELAY_S;
	c->call_waiting = 1;
	c->ring_rate_ms = RING_RATE_DEFAULT_MS;
	c->offhook_rate_ms = OFFHOOK_RATE_DEFAULT_MS;
	c->record_max_mb = 50;
	snprintf(c->record_path, sizeof(c->record_path), "%s", "/etc/awvoice/recordings");
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
		if (!number_region_find(value))
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
		if (value[0] != '/' || strlen(value) >= sizeof(c->record_path))
			return bad(log, name);
		snprintf(c->record_path, sizeof(c->record_path), "%s", value);
	} else {
		return -ENOENT;
	}
	return 0;
}

void config_finish(struct aw_config *c, FILE *log)
{
	if (c->flash_max_ms >= c->flash_min_ms + FLASH_GAP_MIN_MS)
		return;
	if (log)
		fprintf(log, "flash_max_ms must be at least flash_min_ms + %u: both defaults are used\n", FLASH_GAP_MIN_MS);
	c->flash_min_ms = HOOKWATCH_FLASH_MIN_DEFAULT_MS;
	c->flash_max_ms = HOOKWATCH_FLASH_MAX_DEFAULT_MS;
}
