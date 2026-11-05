/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_SIP_GUARD_H
#define AW_PHONED_SIP_GUARD_H

#include "front_end.h"	/* enum fe_refusal */

/*
 * The two guards that keep a second front-end from fighting this line: the voice AT port (another process has the tty
 * open) and the object name qmodem_voip (voipd, or anything else, already registered it, or would at its next start).
 * Both are checks, not negotiation: nothing here opens, closes, stops or configures the other owner.
 *
 * They answer two different questions and must not be mixed: the port decides whether the line may run the daemon's
 * front end at all (the page's phone and its audio, the media socket, the call arbiter), because a holder of the voice
 * port is driving the same line; the name decides only whether the SIP feature may offer qmodem_voip, because that
 * name is the SIP feature's own. A name held elsewhere never stops the page or the handset.
 *
 * The checks are re-run rather than run once, because the other owner can appear at any time: at start, on a UCI
 * reload, on hotplug, and on the slow re-check the daemon keeps. The caller gathers the facts (ubus, UCI, /proc) and
 * hands them in; this module decides, and holds the words the health reply publishes.
 */

/*
 * The two sides of the line's one switch, as three words. `web` leaves the line to the page's phone and the handset;
 * `interop` and `baresip` both put a network phone beside them and differ only in what serves that side. The switch
 * asks whether a network phone is wanted; which of the two words serves it is the build's business, not the choice.
 */
enum sip_mode { SIP_MODE_WEB, SIP_MODE_INTEROP, SIP_MODE_BARESIP };

/* the configuration word: web, interop, baresip */
const char *sip_mode_name(enum sip_mode m);

/* the mode a configuration word names; a word this build does not know is the side that touches nothing, because it
 * is not a mode it runs - so a settings file holding any other word comes out on the side that asks nothing */
enum sip_mode sip_mode_of(const char *name);

/* whether the mode puts a SIP service on this line: interop and baresip do; web is the line's own, with no service and
 * no qmodem_voip object, while the page's own phone runs the same in every mode */
static inline int sip_mode_feature(enum sip_mode m)
{
	return m == SIP_MODE_INTEROP || m == SIP_MODE_BARESIP;
}

/* Whether a slow re-check has anything to fix: the line's guard refused it, the SIP feature wants an object it does
 * not have, or the page's audio listener is down. The daemon asks this once a minute; with nothing due it does not
 * touch ubus, /proc or the sockets at all. */
static inline int sip_recheck_due(enum fe_refusal line, enum sip_mode mode, int object_up, int web_up)
{
	return line != FE_REFUSE_NONE || (sip_mode_feature(mode) && !object_up) || !web_up;
}

/* what the caller found out; the fields are filled in every mode, because the line's front end runs in every one */
struct sip_guard_facts {
	int object_owned;		/* 1: something already registered qmodem_voip */
	char object_holder[24];		/* its name, for the log and health; "" when the service below is the reason */
	int service_enabled;		/* 1: the voipd service is enabled, so it owns the name at its next start */
	int port_held;			/* 1: a process other than this one holds the voice port */
	int port_pid;			/* the holder's pid, for the log line */
	char port_comm[24];		/* the holder's command name */
};

struct sip_guard_state {
	enum fe_refusal refusal;	/* the line's answer: FE_REFUSE_NONE when the daemon's front end may run, else FE_REFUSE_PORT */
	int name_taken;			/* 1: the qmodem_voip name belongs to someone else (or would at their next start) */
	const char *object_owner;	/* health sip.objectOwner: "voipd" while name_taken, else "none" */
	const char *port_owner;		/* health at.portOwner: free, busy */
	char object_holder[24];		/* the name found on qmodem_voip, for the log line ("" when none) */
	unsigned int checks;		/* how many times the guards have run */
	unsigned int refusals;		/* how many of those runs refused the line */
};

/*
 * Run both guards. The line's answer is FE_REFUSE_NONE when the voice port is free, FE_REFUSE_PORT when another
 * process holds it; the name's answer is the name_taken fact, which the caller uses for the object and the health,
 * never for the line. A refusal is not a failure of the line: the handset keeps its own path, and the page's front
 * end waits.
 */
enum fe_refusal sip_guard_check(const struct sip_guard_facts *f, struct sip_guard_state *st);

/*
 * The handover of the qmodem_voip name between these two states, as one word: 1 the name went to this daemon, -1 it
 * came back from it, 0 nothing moved. The log line is chosen from it because the order of a switch and a service
 * start is decided by this moment: a service that starts while the name is not yet held binds to nothing, and one
 * that is stopped while it still answers a call cuts that call.
 */
static inline int sip_handover(int was_held, int now_held)
{
	was_held = was_held != 0;
	now_held = now_held != 0;
	return was_held == now_held ? 0 : (now_held ? 1 : -1);
}

/* the state before the first check: nothing owned, nothing checked, the front end may not run yet */
void sip_guard_init(struct sip_guard_state *st);

#endif /* AW_PHONED_SIP_GUARD_H */
