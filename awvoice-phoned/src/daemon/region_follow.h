/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
/*
 * Which numbering plan the line reads numbers with. The setting names one plan, or names none and asks the line to
 * follow the network it registers on (`auto`, the shipped default). Pure logic, host-tested: the caller compares the
 * answer with the plan in force and only acts on a change.
 */
#ifndef AW_PHONED_REGION_FOLLOW_H
#define AW_PHONED_REGION_FOLLOW_H

#include <string.h>

#include "numbers.h"

#define REGION_AUTO "auto"	/* the value of the `region` setting that follows the network */

/*
 * The plan to use now, as its table name, given the setting, the network the modem reported ("" while nothing is
 * registered) and the plan in force (NULL before the first choice):
 *
 * - a setting that names a plan wins over the network: the subscriber who chose one is not overruled by a mast;
 * - `auto` with digits the table knows takes that plan;
 * - `auto` with nothing reported, or with digits no row holds, keeps the plan in force: losing signal does not change
 *   the plan a caller's number has to be read with, and a network this table does not carry is no reason to guess one.
 *   The plan in force is on the page, so a subscriber who roams onto an unknown network can name a plan.
 *
 * Never NULL, and never a name the table does not hold.
 */
static inline const char *region_choice(const char *setting, const char *mcc, const char *in_force)
{
	const struct number_region *r;

	if (setting && setting[0] && strcmp(setting, REGION_AUTO))
		return setting;
	if (mcc && mcc[0]) {
		r = number_region_by_mcc(mcc);
		if (r)
			return r->name;
	}
	return in_force ? in_force : number_region_default()->name;
}

/* 1 when the plan called `name` is not the one in force (`was` NULL = nothing in force yet) */
static inline int region_changed(const char *was, const char *name)
{
	return !was || !name || strcmp(was, name) != 0;
}

#endif /* AW_PHONED_REGION_FOLLOW_H */
