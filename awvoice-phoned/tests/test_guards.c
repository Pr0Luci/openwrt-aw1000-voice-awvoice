/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
/*
 * The two guards against a temporary tree that mimics /proc and a pty for the exclusive open: the holder scan by
 * descriptor, the command name, the self skip, and the truth table of the mode, the object name and the port.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "port.h"
#include "sip_guard.h"

static char root[64];

static void make_tree(void)
{
	snprintf(root, sizeof(root), "/tmp/aw-guard-test-XXXXXX");
	assert(mkdtemp(root) != NULL);
}

static int remove_one(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
	(void)sb;
	(void)type;
	(void)ftw;
	return remove(path);
}

static void drop_tree(void)
{
	assert(nftw(root, remove_one, 16, FTW_DEPTH | FTW_PHYS) == 0);
}

static void make_dir(const char *rel)
{
	char path[1024];

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	if (mkdir(path, 0700)) {
		perror(path);
		abort();
	}
}

static void make_link(const char *rel, const char *target)
{
	char path[1024];

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	if (symlink(target, path)) {
		perror(path);
		abort();
	}
}

static void make_text(const char *rel, const char *text)
{
	char path[1024];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	f = fopen(path, "w");
	assert(f != NULL);
	fputs(text, f);
	fclose(f);
}

/* one process that has `target` open at descriptor 3, with a command name */
static void make_holder(const char *pid, const char *target, const char *comm)
{
	char rel[64];

	make_dir(pid);
	snprintf(rel, sizeof(rel), "%s/fd", pid);
	make_dir(rel);
	snprintf(rel, sizeof(rel), "%s/fd/3", pid);
	make_link(rel, target);
	snprintf(rel, sizeof(rel), "%s/comm", pid);
	make_text(rel, comm);
}

static void test_holder(void)
{
	char comm[24];
	int pid = 0;

	make_tree();
	make_dir("1");						/* pid 1 is another process, not this one */
	make_dir("1/fd");
	make_link("1/fd/0", "/dev/null");
	make_holder("4242", "/dev/ttyUSB2", "voipd\n");
	make_holder("5151", "/dev/ttyUSB3", "cat\n");
	make_dir("self");
	make_dir("self/fd");
	make_link("self/fd/9", "/dev/ttyUSB2");			/* a name that is not a pid is never a holder */
	assert(port_holder(root, "/dev/ttyUSB2", getpid(), &pid, comm, sizeof(comm)) == 1);
	assert(pid == 4242 && !strcmp(comm, "voipd"));
	assert(port_holder(root, "/dev/ttyUSB3", getpid(), &pid, comm, sizeof(comm)) == 1);
	assert(pid == 5151 && !strcmp(comm, "cat"));
	assert(port_holder(root, "/dev/ttyUSB2", 4242, &pid, comm, sizeof(comm)) == 0);	/* this process's own descriptor: not a conflict */
	assert(port_holder(root, "/dev/ttyUSB9", getpid(), &pid, comm, sizeof(comm)) == 0);
	assert(port_holder(root, "/dev/ttyUSB2", getpid(), NULL, NULL, 0) == 1);		/* the names are optional */
	drop_tree();
}

static void test_holder_edges(void)
{
	char comm[24];
	int pid = 1234;

	make_tree();
	make_holder("77", "/dev/ttyUSB2 (deleted)", "voipd\n");	/* a path that only looks like this program's */
	assert(port_holder(root, "/dev/ttyUSB2", getpid(), &pid, comm, sizeof(comm)) == 0);
	make_dir("88");						/* a process without an fd directory */
	make_text("88/comm", "x\n");
	assert(port_holder(root, "/dev/ttyUSB2", getpid(), &pid, comm, sizeof(comm)) == 0);
	comm[0] = 'x';
	make_holder("99", "/dev/ttyUSB2", "");			/* a holder whose comm cannot be read is still a holder */
	assert(port_holder(root, "/dev/ttyUSB2", getpid(), &pid, comm, sizeof(comm)) == 1);
	assert(pid == 99 && !comm[0]);
	drop_tree();
	assert(port_holder(root, "/dev/ttyUSB2", getpid(), &pid, comm, sizeof(comm)) == -ENOENT);
	assert(port_holder(root, "", getpid(), &pid, comm, sizeof(comm)) == -EINVAL);
}

static void test_exclusive(void)
{
	int master, slave, p[2];
	char *name;

	assert(port_exclusive(-1) == -EBADF);
	/* the kernel's refusal of the second opener belongs to the router; here only the call itself is proven, on the
	 * tty side of a pty (a pty master is not a tty on this host) */
	master = posix_openpt(O_RDWR | O_NOCTTY);
	assert(master >= 0);
	assert(grantpt(master) == 0 && unlockpt(master) == 0);
	name = ptsname(master);
	assert(name != NULL);
	slave = open(name, O_RDWR | O_NOCTTY);
	assert(slave >= 0);
	assert(port_exclusive(slave) == 0);
	close(slave);
	close(master);
	assert(pipe(p) == 0);
	assert(port_exclusive(p[0]) == -ENOTTY);		/* not a tty: refused, and nothing is marked */
	close(p[0]);
	close(p[1]);
}

/*
 * The switch has two sides and the settings hold three words: `web` leaves the line to the page and the handset,
 * `interop` and `baresip` ask for a network phone beside them and differ only in what serves that side. A word this
 * build does not know lands on the side that touches nothing: the safe side, reached by the same rule as any other
 * unknown word rather than by a translation of it. The word a line with no SIP once held is one of those words.
 */
static void test_mode_words(void)
{
	assert(!strcmp(sip_mode_name(SIP_MODE_WEB), "web"));
	assert(!strcmp(sip_mode_name(SIP_MODE_INTEROP), "interop"));
	assert(!strcmp(sip_mode_name(SIP_MODE_BARESIP), "baresip"));
	assert(!strcmp(sip_mode_name((enum sip_mode)99), "web"));	/* a state with no name says the safe side */
	assert(sip_mode_of("web") == SIP_MODE_WEB);
	assert(sip_mode_of("interop") == SIP_MODE_INTEROP);
	assert(sip_mode_of("baresip") == SIP_MODE_BARESIP);
	assert(sip_mode_of("voipd") == SIP_MODE_WEB);
	assert(sip_mode_of("off") == SIP_MODE_WEB);		/* a settings file from an older build lands on the safe side */
	assert(sip_mode_of("") == SIP_MODE_WEB);
	assert(sip_mode_of(NULL) == SIP_MODE_WEB);
	/* the two method words put a SIP service on the line; the page's own word leaves it to the page and the handset */
	assert(!sip_mode_feature(SIP_MODE_WEB));
	assert(sip_mode_feature(SIP_MODE_INTEROP));
	assert(sip_mode_feature(SIP_MODE_BARESIP));
}

static void facts(struct sip_guard_facts *f)
{
	memset(f, 0, sizeof(*f));
}

static void test_guard(void)
{
	struct sip_guard_facts f;
	struct sip_guard_state st;

	sip_guard_init(&st);
	assert(st.refusal == FE_REFUSE_MODE && st.checks == 0 && st.refusals == 0 && !st.name_taken);
	assert(!strcmp(st.object_owner, "none") && !strcmp(st.port_owner, "free"));

	facts(&f);
	assert(sip_guard_check(&f, &st) == FE_REFUSE_NONE);	/* nothing holds the port */
	assert(st.checks == 1 && st.refusals == 0 && !st.name_taken);
	assert(!strcmp(st.object_owner, "none") && !strcmp(st.port_owner, "free"));

	/* the name is the SIP feature's fact: it never refuses the line */
	facts(&f);
	f.object_owned = 1;
	snprintf(f.object_holder, sizeof(f.object_holder), "voipd");
	assert(sip_guard_check(&f, &st) == FE_REFUSE_NONE);
	assert(st.name_taken && !strcmp(st.object_owner, "voipd") && !strcmp(st.object_holder, "voipd"));
	assert(st.checks == 2 && st.refusals == 0 && !strcmp(st.port_owner, "free"));

	facts(&f);
	f.object_owned = 1;
	snprintf(f.object_holder, sizeof(f.object_holder), "someone");
	assert(sip_guard_check(&f, &st) == FE_REFUSE_NONE);
	assert(st.name_taken && !strcmp(st.object_owner, "voipd"));	/* the health word: the name is held elsewhere */
	assert(!strcmp(st.object_holder, "someone"));			/* the log line names it */

	facts(&f);
	f.service_enabled = 1;					/* not registered yet, but its start would take the name */
	assert(sip_guard_check(&f, &st) == FE_REFUSE_NONE);
	assert(st.name_taken && !strcmp(st.object_owner, "voipd") && !strcmp(st.object_holder, "voipd"));

	/* the port is the line's fact: it refuses the front end and the page audio */
	facts(&f);
	f.port_held = 1;
	f.port_pid = 4242;
	snprintf(f.port_comm, sizeof(f.port_comm), "voipd");
	assert(sip_guard_check(&f, &st) == FE_REFUSE_PORT);
	assert(!strcmp(st.object_owner, "none") && !strcmp(st.port_owner, "busy") && !st.name_taken);
	assert(st.checks == 5 && st.refusals == 1);

	facts(&f);
	f.object_owned = 1;
	snprintf(f.object_holder, sizeof(f.object_holder), "voipd");
	f.port_held = 1;
	assert(sip_guard_check(&f, &st) == FE_REFUSE_PORT);	/* the line waits on the port; the name is reported too */
	assert(st.name_taken && !strcmp(st.object_owner, "voipd") && !strcmp(st.port_owner, "busy"));

	/* the checks are re-run: start, reload, hotplug, the slow re-check */
	facts(&f);
	assert(sip_guard_check(&f, &st) == FE_REFUSE_NONE);
	assert(sip_guard_check(&f, &st) == FE_REFUSE_NONE);
	assert(sip_guard_check(&f, &st) == FE_REFUSE_NONE);
	assert(st.checks == 9 && st.refusals == 2 && !st.name_taken);
	assert(!strcmp(fe_refusal_name(st.refusal), ""));	/* a run that passes clears the refusal */
}

/* the slow re-check is due only when it has something to fix; a mode that serves no service never waits on an object */
static void test_recheck(void)
{
	assert(!sip_recheck_due(FE_REFUSE_NONE, SIP_MODE_WEB, 0, 1));
	assert(sip_recheck_due(FE_REFUSE_NONE, SIP_MODE_WEB, 0, 0));	/* the listener is down */
	assert(sip_recheck_due(FE_REFUSE_PORT, SIP_MODE_WEB, 1, 1));	/* the line waits on the port */
	assert(sip_recheck_due(FE_REFUSE_NONE, SIP_MODE_INTEROP, 0, 1));	/* the object is not registered */
	assert(!sip_recheck_due(FE_REFUSE_NONE, SIP_MODE_INTEROP, 1, 1));
	assert(sip_recheck_due(FE_REFUSE_NONE, SIP_MODE_BARESIP, 0, 1));
	assert(!sip_recheck_due(FE_REFUSE_NONE, SIP_MODE_BARESIP, 1, 1));
}

/* the handover is the moment a switch and a service start are ordered by: nothing moved says nothing, and a name
   that arrived or left is told apart from its opposite */
static void test_handover(void)
{
	assert(sip_handover(0, 0) == 0);
	assert(sip_handover(1, 1) == 0);
	assert(sip_handover(0, 1) == 1);		/* the daemon took the name */
	assert(sip_handover(1, 0) == -1);		/* the daemon let it go */
	assert(sip_handover(0, 2) == 1);		/* any nonzero answer is the held state */
	assert(sip_handover(2, 0) == -1);
}

int main(void)
{
	test_handover();
	test_holder();
	test_holder_edges();
	test_exclusive();
	test_mode_words();
	test_guard();
	test_recheck();
	printf("guards: all tests passed\n");
	return 0;
}
