// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The daemon's settings (config_set / config_finish are pure): make -f Makefile.host test */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

static char *log_buf;
static size_t log_len;

static FILE *log_open(void)
{
	free(log_buf);
	log_buf = NULL;
	return open_memstream(&log_buf, &log_len);
}

static const char *log_text(FILE *f)
{
	fflush(f);
	return log_buf ? log_buf : "";
}

/* a number option: the edges are taken, one step outside either edge is refused and leaves the default */
static void edge(const char *name, unsigned int low, unsigned int high, unsigned int (*get)(const struct aw_config *))
{
	struct aw_config c, ref;
	char text[16];
	FILE *f = log_open();

	config_defaults(&ref);
	config_defaults(&c);
	snprintf(text, sizeof(text), "%u", low);
	assert(config_set(&c, name, text, f) == 0 && get(&c) == low);
	snprintf(text, sizeof(text), "%u", high);
	assert(config_set(&c, name, text, f) == 0 && get(&c) == high);
	assert(!*log_text(f));
	config_defaults(&c);
	if (low) {
		snprintf(text, sizeof(text), "%u", low - 1);
		assert(config_set(&c, name, text, f) == -EINVAL && get(&c) == get(&ref));
	}
	snprintf(text, sizeof(text), "%u", high + 1);
	assert(config_set(&c, name, text, f) == -EINVAL && get(&c) == get(&ref));
	assert(config_set(&c, name, "", f) == -EINVAL && get(&c) == get(&ref));
	assert(config_set(&c, name, "12x", f) == -EINVAL && get(&c) == get(&ref));
	assert(config_set(&c, name, "-1", f) == -EINVAL && get(&c) == get(&ref));
	assert(config_set(&c, name, " 7", f) == -EINVAL && get(&c) == get(&ref));
	assert(config_set(&c, name, "99999999999999999999", f) == -EINVAL && get(&c) == get(&ref));
	/* one line per refusal, naming the option and never the value */
	assert(strstr(log_text(f), name) && !strstr(log_text(f), "12x") && !strstr(log_text(f), "99999"));
	fclose(f);
}

static unsigned int get_enabled(const struct aw_config *c) { return (unsigned int)c->enabled; }
static unsigned int get_channel(const struct aw_config *c) { return c->channel; }
static unsigned int get_at(const struct aw_config *c) { return c->at_interface; }
static unsigned int get_fmin(const struct aw_config *c) { return c->flash_min_ms; }
static unsigned int get_fmax(const struct aw_config *c) { return c->flash_max_ms; }
static unsigned int get_pause(const struct aw_config *c) { return c->dial_pause_s; }
static unsigned int get_trace(const struct aw_config *c) { return (unsigned int)c->level_trace; }

static void test_defaults(void)
{
	struct aw_config c;

	memset(&c, 0xa5, sizeof(c));
	config_defaults(&c);
	assert(c.enabled == 1 && c.channel == 1);
	assert(!strcmp(c.board, "/etc/awvoice/board.bin"));
	assert(c.at_interface == 2);
	assert(c.flash_min_ms == 80 && c.flash_max_ms == 300);
	assert(c.dial_pause_s == 6);
	assert(c.cid_kind == AW_CID_DTMF && !c.cid_prering && c.ringback == AW_RINGBACK_AUTO && c.level_trace == 0);
	assert(c.earpiece_volume == AW_CONFIG_UNSET && c.modem_rx_gain == AW_CONFIG_UNSET);
	assert(c.my_number[0] == 0);
	assert(!strcmp(c.region, "LK"));
	assert(c.clir == 0 && !c.hotline_number[0] && c.hotline_delay_s == 10);
}

static void test_ranges(void)
{
	edge("enabled", 0, 1, get_enabled);
	edge("channel", 0, 1, get_channel);
	edge("at_interface", 0, 9, get_at);
	edge("flash_min_ms", 20, 500, get_fmin);
	edge("flash_max_ms", 300, 3000, get_fmax);
	edge("dial_pause_s", 3, 15, get_pause);
	edge("level_trace", 0, 1, get_trace);
}

static void test_choices(void)
{
	struct aw_config c;
	FILE *f = log_open();

	config_defaults(&c);
	assert(config_set(&c, "ringback", "on", f) == 0 && c.ringback == AW_RINGBACK_ON);
	assert(config_set(&c, "ringback", "off", f) == 0 && c.ringback == AW_RINGBACK_OFF);
	assert(config_set(&c, "ringback", "auto", f) == 0 && c.ringback == AW_RINGBACK_AUTO);
	assert(config_set(&c, "ringback", "1", f) == 0 && c.ringback == AW_RINGBACK_ON);	/* the earlier numbers still mean on and off */
	assert(config_set(&c, "ringback", "0", f) == 0 && c.ringback == AW_RINGBACK_OFF);
	assert(config_set(&c, "ringback", "2", f) == -EINVAL && c.ringback == AW_RINGBACK_OFF);
	assert(config_set(&c, "ringback", "", f) == -EINVAL && config_set(&c, "ringback", "AUTO", f) == -EINVAL);
	config_defaults(&c);
	assert(config_set(&c, "cid", "off", f) == 0 && c.cid_kind == AW_CID_OFF);
	assert(config_set(&c, "cid", "dtmf", f) == 0 && c.cid_kind == AW_CID_DTMF);
	assert(config_set(&c, "cid", "bellcore", f) == 0 && c.cid_kind == AW_CID_BELLCORE);
	assert(config_set(&c, "cid", "etsi", f) == 0 && c.cid_kind == AW_CID_ETSI);
	assert(config_set(&c, "cid", "off", f) == 0 && c.cid_kind == AW_CID_OFF);
	assert(config_set(&c, "cid", "fsk", f) == -EINVAL && c.cid_kind == AW_CID_OFF);	/* a refusal leaves what was there */
	assert(config_set(&c, "cid", "", f) == -EINVAL);
	assert(config_set(&c, "cid", "DTMF", f) == -EINVAL);
	config_defaults(&c);
	assert(config_set(&c, "cid_when", "prering", f) == 0 && c.cid_prering == 1);
	assert(config_set(&c, "cid_when", "ring1", f) == 0 && c.cid_prering == 0);
	assert(config_set(&c, "cid_when", "first", f) == -EINVAL && c.cid_prering == 0);
	fclose(f);
}

static void test_modem_levels(void)
{
	struct aw_config c;
	FILE *f = log_open();

	config_defaults(&c);
	assert(config_set(&c, "modem_rx_gain", "0", f) == 0 && c.modem_rx_gain == 0);
	assert(config_set(&c, "modem_rx_gain", "65535", f) == 0 && c.modem_rx_gain == 65535);
	assert(config_set(&c, "modem_rx_gain", "65536", f) == -EINVAL && c.modem_rx_gain == 65535);
	assert(config_set(&c, "modem_rx_gain", "", f) == 0 && c.modem_rx_gain == AW_CONFIG_UNSET);	/* empty = leave the modem as it is */
	assert(config_set(&c, "earpiece_volume", "0", f) == 0 && c.earpiece_volume == 0);
	assert(config_set(&c, "earpiece_volume", "255", f) == 0 && c.earpiece_volume == 255);
	assert(config_set(&c, "earpiece_volume", "256", f) == -EINVAL && c.earpiece_volume == 255);
	assert(config_set(&c, "earpiece_volume", "", f) == 0 && c.earpiece_volume == AW_CONFIG_UNSET);
	assert(config_set(&c, "earpiece_volume", "loud", f) == -EINVAL);
	fclose(f);
}

static void test_text(void)
{
	struct aw_config c;
	char longpath[200], digits[64];
	FILE *f = log_open();

	config_defaults(&c);
	assert(config_set(&c, "board", "/tmp/board.bin", f) == 0 && !strcmp(c.board, "/tmp/board.bin"));
	assert(config_set(&c, "board", "board.bin", f) == -EINVAL && !strcmp(c.board, "/tmp/board.bin"));	/* absolute paths only */
	assert(config_set(&c, "board", "", f) == -EINVAL);
	memset(longpath, 'a', sizeof(longpath));
	longpath[0] = '/';
	longpath[sizeof(longpath) - 1] = 0;
	assert(config_set(&c, "board", longpath, f) == -EINVAL && !strcmp(c.board, "/tmp/board.bin"));
	memset(longpath, 'a', AW_CONFIG_PATH_MAX - 1);
	longpath[0] = '/';
	longpath[AW_CONFIG_PATH_MAX - 1] = 0;
	assert(config_set(&c, "board", longpath, f) == 0 && strlen(c.board) == AW_CONFIG_PATH_MAX - 1);
	longpath[AW_CONFIG_PATH_MAX] = 0;
	longpath[AW_CONFIG_PATH_MAX - 1] = 'a';
	assert(config_set(&c, "board", longpath, f) == -EINVAL);

	/* the number rule: digits, '*', '#', a leading '+', at most 32 characters */
	assert(config_set(&c, "my_number", "", f) == 0 && !c.my_number[0]);
	assert(config_set(&c, "my_number", "+1234", f) == 0 && !strcmp(c.my_number, "+1234"));
	assert(config_set(&c, "my_number", "*#0", f) == 0);
	assert(config_set(&c, "my_number", "12+3", f) == -EINVAL && !strcmp(c.my_number, "*#0"));
	assert(config_set(&c, "my_number", "12 3", f) == -EINVAL);
	assert(config_set(&c, "my_number", "abc", f) == -EINVAL);
	memset(digits, '1', 32);
	digits[32] = 0;
	assert(config_set(&c, "my_number", digits, f) == 0 && strlen(c.my_number) == 32);
	digits[32] = '1';
	digits[33] = 0;
	assert(config_set(&c, "my_number", digits, f) == -EINVAL && strlen(c.my_number) == 32);
	/* a number never reaches the log, not even in a refusal */
	assert(!strstr(log_text(f), "12 3") && !strstr(log_text(f), "11111111"));
	fclose(f);
}

static void test_region(void)
{
	struct aw_config c;
	FILE *f = log_open();

	config_defaults(&c);
	assert(config_set(&c, "region", "LK", f) == 0 && !strcmp(c.region, "LK"));
	assert(config_set(&c, "region", "lk", f) == -EINVAL && !strcmp(c.region, "LK"));
	assert(config_set(&c, "region", "", f) == -EINVAL);
	assert(config_set(&c, "region", "ZZ", f) == -EINVAL && !strcmp(c.region, "LK"));
	assert(strstr(log_text(f), "region") && !strstr(log_text(f), "ZZ"));
	fclose(f);
}

static void test_clir_hotline(void)
{
	struct aw_config c;
	FILE *f = log_open();
	char longer[40];

	config_defaults(&c);
	assert(config_set(&c, "clir", "hide", f) == 0 && c.clir == 1);
	assert(config_set(&c, "clir", "show", f) == 0 && c.clir == 2);
	assert(config_set(&c, "clir", "network", f) == 0 && c.clir == 0);
	assert(config_set(&c, "clir", "show", f) == 0);
	assert(config_set(&c, "clir", "Hide", f) == -EINVAL && c.clir == 2);
	assert(config_set(&c, "clir", "", f) == -EINVAL && config_set(&c, "clir", "1", f) == -EINVAL);
	assert(config_set(&c, "hotline_number", "0123", f) == 0 && !strcmp(c.hotline_number, "0123"));
	assert(config_set(&c, "hotline_number", "+94123", f) == 0);
	assert(config_set(&c, "hotline_number", "", f) == 0 && !c.hotline_number[0]);	/* empty = no hotline */
	assert(config_set(&c, "hotline_number", "12345678901234567890", f) == 0);	/* 20: the dial buffer's size */
	assert(config_set(&c, "hotline_number", "123456789012345678901", f) == -EINVAL && strlen(c.hotline_number) == 20);
	assert(config_set(&c, "hotline_number", "12a", f) == -EINVAL);
	memset(longer, '5', sizeof(longer) - 1);
	longer[sizeof(longer) - 1] = 0;
	assert(config_set(&c, "hotline_number", longer, f) == -EINVAL);
	assert(config_set(&c, "hotline_delay", "3", f) == 0 && c.hotline_delay_s == 3);
	assert(config_set(&c, "hotline_delay", "99", f) == 0 && c.hotline_delay_s == 99);
	assert(config_set(&c, "hotline_delay", "2", f) == -EINVAL && config_set(&c, "hotline_delay", "100", f) == -EINVAL);
	assert(config_set(&c, "hotline_delay", "0", f) == -EINVAL && c.hotline_delay_s == 99);	/* never 0: the user can always dial first (U3) */
	assert(!strstr(log_text(f), "12a") && !strstr(log_text(f), "555"));
	fclose(f);
}

static void test_waiting_options(void)
{
	struct aw_config c;
	FILE *f = log_open();

	config_defaults(&c);
	assert(c.call_waiting == 1 && c.conference == 0);
	assert(config_set(&c, "call_waiting", "0", f) == 0 && c.call_waiting == 0);
	assert(config_set(&c, "call_waiting", "1", f) == 0 && c.call_waiting == 1);
	assert(config_set(&c, "call_waiting", "2", f) == -EINVAL && c.call_waiting == 1);
	assert(config_set(&c, "call_waiting", "", f) == -EINVAL && config_set(&c, "call_waiting", "on", f) == -EINVAL);
	assert(config_set(&c, "conference", "1", f) == 0 && c.conference == 1);
	assert(config_set(&c, "conference", "x", f) == -EINVAL && c.conference == 1);
	fclose(f);
}

static void test_unknown_names(void)
{
	struct aw_config c, ref;
	FILE *f = log_open();

	config_defaults(&c);
	config_defaults(&ref);
	/* the M5 options and the page's own options: ignored by the caller, nothing logged, nothing changed
	 * (led, ring_rate_ms and offhook_rate_ms are settings since the phone LED: test_indicator) */
	assert(config_set(&c, "mic_gain_db", "3", f) == -ENOENT);
	assert(config_set(&c, "mic_gain_db", "3", f) == -ENOENT);
	assert(config_set(&c, "", "1", f) == -ENOENT);
	assert(config_set(&c, "enable", "1", f) == -ENOENT);		/* near misses are unknown too */
	assert(config_set(&c, "enabled ", "1", f) == -ENOENT);
	assert(!memcmp(&c, &ref, sizeof(c)) && !*log_text(f));
	fclose(f);
}

static void test_cross_rule(void)
{
	struct aw_config c;
	FILE *f = log_open();

	/* max >= min + 100 holds: nothing changes */
	config_defaults(&c);
	assert(config_set(&c, "flash_min_ms", "200", f) == 0 && config_set(&c, "flash_max_ms", "300", f) == 0);
	config_finish(&c, f);
	assert(c.flash_min_ms == 200 && c.flash_max_ms == 300 && !*log_text(f));
	/* the edge itself */
	assert(config_set(&c, "flash_min_ms", "500", f) == 0 && config_set(&c, "flash_max_ms", "600", f) == 0);
	config_finish(&c, f);
	assert(c.flash_min_ms == 500 && c.flash_max_ms == 600 && !*log_text(f));
	/* one below the edge: both go back to the defaults with one line */
	assert(config_set(&c, "flash_max_ms", "599", f) == 0);
	config_finish(&c, f);
	assert(c.flash_min_ms == 80 && c.flash_max_ms == 300);
	assert(strstr(log_text(f), "flash_max_ms") != NULL);
	/* the default maximum is too low for a high minimum even though each is valid alone */
	config_defaults(&c);
	assert(config_set(&c, "flash_min_ms", "450", f) == 0);
	config_finish(&c, NULL);			/* a missing log is allowed */
	assert(c.flash_min_ms == 80 && c.flash_max_ms == 300);
	/* every setting is also taken without a log */
	assert(config_set(&c, "dial_pause_s", "x", NULL) == -EINVAL);
	fclose(f);
}

int main(void)
{
	test_defaults();
	test_ranges();
	test_choices();
	test_modem_levels();
	test_text();
	test_region();
	test_clir_hotline();
	test_waiting_options();
	test_unknown_names();
	test_cross_rule();
	free(log_buf);
	printf("config: all tests passed\n");
	return 0;
}
