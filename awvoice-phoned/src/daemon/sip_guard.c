/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#include <stdio.h>
#include <string.h>

#include "sip_guard.h"

const char *sip_mode_name(enum sip_mode m)
{
	switch (m) {
	case SIP_MODE_WEB:
		return "web";
	case SIP_MODE_INTEROP:
		return "interop";
	case SIP_MODE_BARESIP:
		return "baresip";
	}
	return "web";		/* a state with no name is not one the line runs: say the side that touches nothing */
}

enum sip_mode sip_mode_of(const char *name)
{
	if (!name)
		return SIP_MODE_WEB;
	if (!strcmp(name, "web"))
		return SIP_MODE_WEB;
	if (!strcmp(name, "interop"))
		return SIP_MODE_INTEROP;
	if (!strcmp(name, "baresip"))
		return SIP_MODE_BARESIP;
	return SIP_MODE_WEB;		/* a word this build does not know: the side that asks nothing of any other program */
}

void sip_guard_init(struct sip_guard_state *st)
{
	memset(st, 0, sizeof(*st));
	st->refusal = FE_REFUSE_MODE;
	st->object_owner = "none";
	st->port_owner = "free";
}

enum fe_refusal sip_guard_check(const struct sip_guard_facts *f, struct sip_guard_state *st)
{
	st->checks++;
	st->object_holder[0] = 0;
	/* the name is the SIP feature's question: an object that is already there is the more visible owner, and a
	 * service that is merely enabled owns the name at its next start, so it is answered the same way rather than
	 * raced. it says who holds the name; it does not stop the line */
	st->name_taken = f->object_owned || f->service_enabled;
	if (st->name_taken) {
		snprintf(st->object_holder, sizeof(st->object_holder), "%s",
			f->object_owned && f->object_holder[0] ? f->object_holder : "voipd");
		st->object_owner = "voipd";	/* the health word: the name is held elsewhere, whoever holds it */
	} else {
		st->object_owner = "none";
	}
	/* the port is the line's question: a holder is driving the same line, so the daemon's front end and the page
	 * audio wait. the mode takes no part: the front end runs in every mode */
	if (f->port_held) {
		st->port_owner = "busy";
		st->refusal = FE_REFUSE_PORT;
		st->refusals++;
		return st->refusal;
	}
	st->port_owner = "free";
	st->refusal = FE_REFUSE_NONE;
	return st->refusal;
}
