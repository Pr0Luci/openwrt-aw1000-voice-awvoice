// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <uci.h>

#include "config.h"
#include "alarm.h"
#include "blocking.h"
#include "dnd.h"
#include "forward.h"
#include "phonebook.h"

#define PACKAGE		"awvoice"
#define SECTION		"main"

int config_load(struct aw_config *c, FILE *log)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *pkg = NULL;
	struct uci_section *sec;
	struct uci_element *e;

	config_defaults(c);
	if (!ctx)
		return -ENOMEM;
	if (uci_load(ctx, PACKAGE, &pkg) || !pkg) {
		/* no file: every default stands, which is a valid configuration */
		uci_free_context(ctx);
		return 0;
	}
	sec = uci_lookup_section(ctx, pkg, SECTION);
	if (sec) {
		uci_foreach_element(&sec->options, e) {
			struct uci_option *o = uci_to_option(e);

			if (o->type == UCI_TYPE_STRING)
				config_set(c, o->e.name, o->v.string, log);
		}
	}
	/* the SIP mode has a section of its own: awvoice.sip.mode */
	sec = uci_lookup_section(ctx, pkg, "sip");
	if (sec)
		config_set_sip(c, uci_lookup_option_string(ctx, sec, "mode"), log);
	config_finish(c, log);
	uci_free_context(ctx);
	return 0;
}

int config_load_book(struct phonebook *pb, const struct number_region *region, FILE *log)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *pkg = NULL;
	struct uci_element *e;

	phonebook_clear(pb, region);
	if (!ctx)
		return -ENOMEM;
	if (uci_load(ctx, PACKAGE, &pkg) || !pkg) {
		uci_free_context(ctx);
		return 0;
	}
	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *sec = uci_to_section(e);
		const char *name = uci_lookup_option_string(ctx, sec, "name");
		const char *quick = uci_lookup_option_string(ctx, sec, "quick");
		const char *through;		/* the contact's own quiet-hours ask: only `1` is one */
		struct uci_option *list = uci_lookup_option(ctx, sec, "number");
		const char *numbers[PB_NUMBERS_MAX + 1];
		unsigned int count = 0;
		struct uci_element *v;

		if (strcmp(sec->type, "contact"))
			continue;
		through = uci_lookup_option_string(ctx, sec, "ring_through");
		if (list && list->type == UCI_TYPE_LIST) {
			uci_foreach_element(&list->v.list, v) {
				if (count > PB_NUMBERS_MAX)
					break;
				numbers[count++] = v->name;
			}
		} else if (list && list->type == UCI_TYPE_STRING) {
			numbers[count++] = list->v.string;
		}
		(void)phonebook_add(pb, name, numbers, count, quick, through && !strcmp(through, "1"), log);
	}
	uci_free_context(ctx);
	return 0;
}

int config_load_blocking(struct blocking *b, const struct number_region *region, FILE *log)
{
	static const char *const list_name[BLOCK_LISTS] = { "incoming", "incoming_prefix", "outgoing", "outgoing_prefix" };
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *pkg = NULL;
	struct uci_element *e;

	blocking_clear(b, region);
	if (!ctx)
		return -ENOMEM;
	if (uci_load(ctx, PACKAGE, &pkg) || !pkg) {
		uci_free_context(ctx);
		return 0;
	}
	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *sec = uci_to_section(e);
		const char *anon;
		int k;

		if (strcmp(sec->type, "blocking"))
			continue;
		anon = uci_lookup_option_string(ctx, sec, "anonymous");
		b->anonymous = anon && !strcmp(anon, "1");
		for (k = 0; k < BLOCK_LISTS; k++) {
			struct uci_option *o = uci_lookup_option(ctx, sec, list_name[k]);
			struct uci_element *v;

			if (o && o->type == UCI_TYPE_LIST) {
				uci_foreach_element(&o->v.list, v)
					(void)blocking_add(b, (enum block_list)k, v->name, log);
			} else if (o && o->type == UCI_TYPE_STRING) {
				(void)blocking_add(b, (enum block_list)k, o->v.string, log);
			}
		}
		break;			/* the first section of the type */
	}
	uci_free_context(ctx);
	return 0;
}

int config_load_alarms(struct alarms *a, FILE *log)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *pkg = NULL;
	struct uci_element *e;

	alarms_clear(a);
	if (!ctx)
		return -ENOMEM;
	if (uci_load(ctx, PACKAGE, &pkg) || !pkg) {
		uci_free_context(ctx);
		return 0;
	}
	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *sec = uci_to_section(e);
		struct uci_option *list = uci_lookup_option(ctx, sec, "day");
		const char *days[7 + 1];
		unsigned int count = 0;
		struct uci_element *v;

		if (strcmp(sec->type, "alarm"))
			continue;
		if (list && list->type == UCI_TYPE_LIST) {
			uci_foreach_element(&list->v.list, v) {
				if (count > 7)
					break;
				days[count++] = v->name;
			}
		} else if (list && list->type == UCI_TYPE_STRING) {
			days[count++] = list->v.string;
		}
		(void)alarms_add(a, uci_lookup_option_string(ctx, sec, "label"), uci_lookup_option_string(ctx, sec, "time"), days, count,
			uci_lookup_option_string(ctx, sec, "enabled"), log);
	}
	uci_free_context(ctx);
	return 0;
}

/* a whole decimal number, no sign, no text after it */
static int whole(const char *text, unsigned int *out)
{
	char *end;
	unsigned long v;

	if (!text || !*text || *text < '0' || *text > '9')
		return 0;
	v = strtoul(text, &end, 10);
	if (*end || v > 1000)
		return 0;
	*out = (unsigned int)v;
	return 1;
}

int config_load_forward(struct phone_forward *f, FILE *log)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *pkg = NULL;
	struct uci_element *e;

	memset(f, 0, sizeof(*f));
	if (!ctx)
		return -ENOMEM;
	if (uci_load(ctx, PACKAGE, &pkg) || !pkg) {
		uci_free_context(ctx);
		return 0;
	}
	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *sec = uci_to_section(e);
		const char *enabled = uci_lookup_option_string(ctx, sec, "enabled");
		const char *to = uci_lookup_option_string(ctx, sec, "to");
		const char *when = uci_lookup_option_string(ctx, sec, "when");
		const char *delay = uci_lookup_option_string(ctx, sec, "delay");
		struct phone_forward cand;
		unsigned int d = 20;
		char probe[64];

		if (strcmp(sec->type, "forwarding"))
			continue;
		memset(&cand, 0, sizeof(cand));
		cand.enabled = enabled && !strcmp(enabled, "1");
		cand.delay_s = 20;
		if (!(!enabled || !strcmp(enabled, "0") || !strcmp(enabled, "1")) || (to && strlen(to) > FWD_TO_MAX) ||
		    (when && forward_when_parse(when, &cand.when)) || (delay && (!whole(delay, &d) || d < 5 || d > 30 || d % 5))) {
			if (log)
				fprintf(log, "the forwarding settings are not valid: the network is left as it is\n");
			break;
		}
		cand.delay_s = d;
		if (to)
			snprintf(cand.to, sizeof(cand.to), "%s", to);
		cand.configured = 1;
		/* an enabled forwarding needs a target the command can carry */
		if (cand.enabled && forward_register_cmd(&cand, probe, sizeof(probe))) {
			if (log)
				fprintf(log, "the forwarding settings are not valid: the network is left as it is\n");
			break;
		}
		*f = cand;
		break;				/* the first section of the type */
	}
	uci_free_context(ctx);
	return 0;
}

/*
 * The quiet hours: every section of type `dnd`, in the order the file writes them, because the first window that
 * matches decides and an admin must be able to see that order in the file. The always-ring list is the feature's,
 * not one window's - family rings through whichever window is open - so every section's `allow` entries join one
 * list. A window that cannot mean what it is written as is said in the log and dropped; the rest stand.
 */
int config_load_dnd(struct dnd *d, const struct number_region *region, FILE *log)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *pkg = NULL;
	struct uci_element *e;

	dnd_clear(d, region);
	if (!ctx)
		return -ENOMEM;
	if (uci_load(ctx, PACKAGE, &pkg) || !pkg) {
		/* no file: no quiet hours, which is how the router ships */
		uci_free_context(ctx);
		return 0;
	}
	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *sec = uci_to_section(e);
		struct uci_option *o;
		const char *names[7];
		const char *from, *to, *text;
		int n_names = 0, days, mode, repeat = 0, all_day = 0, enabled = 1;

		if (strcmp(sec->type, "dnd"))
			continue;
		text = uci_lookup_option_string(ctx, sec, "enabled");
		if (text && !strcmp(text, "0"))
			enabled = 0;
		from = uci_lookup_option_string(ctx, sec, "from");
		to = uci_lookup_option_string(ctx, sec, "to");
		text = uci_lookup_option_string(ctx, sec, "all_day");
		all_day = text && !strcmp(text, "1");
		mode = dnd_mode_parse(uci_lookup_option_string(ctx, sec, "mode"));
		if (mode < 0) {
			if (log)
				fprintf(log, "a quiet-hours window was not taken: its mode is not silent, machine or reject\n");
			continue;
		}
		o = uci_lookup_option(ctx, sec, "repeat_min");
		if (o && o->type == UCI_TYPE_STRING) {
			char *end;
			long v = strtol(o->v.string, &end, 10);

			if (end == o->v.string || *end) {
				if (log)
					fprintf(log, "a quiet-hours window was not taken: its repeat time is not a number of minutes\n");
				continue;
			}
			repeat = (int)v;
		}
		o = uci_lookup_option(ctx, sec, "days");
		if (o && o->type == UCI_TYPE_LIST) {
			struct uci_element *v;

			uci_foreach_element(&o->v.list, v)
				if (n_names < 7)
					names[n_names++] = v->name;
		} else if (o && o->type == UCI_TYPE_STRING) {
			names[n_names++] = o->v.string;
		}
		days = dnd_days_parse(names, n_names);
		if (days < 0) {
			if (log)
				fprintf(log, "a quiet-hours window was not taken: its day list holds a name that is not a weekday\n");
			continue;
		}
		if (dnd_window_add(d, enabled, from, to, days, all_day, mode, repeat, log))
			continue;
		o = uci_lookup_option(ctx, sec, "allow");
		if (o && o->type == UCI_TYPE_LIST) {
			struct uci_element *v;

			uci_foreach_element(&o->v.list, v)
				(void)dnd_allow_add(d, v->name, log);
		} else if (o && o->type == UCI_TYPE_STRING) {
			(void)dnd_allow_add(d, o->v.string, log);
		}
	}
	uci_free_context(ctx);
	return 0;
}
