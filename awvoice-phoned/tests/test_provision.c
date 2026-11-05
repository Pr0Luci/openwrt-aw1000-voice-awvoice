// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The scanned code's file rules on real files under /tmp: make -f Makefile.host test */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "provision.h"

#define NOW		1700000000
#define TOKEN		"0123456789abcdef0123456789abcdef"

static void touch(const char *dir, const char *name, time_t mtime)
{
	char path[512];
	struct timespec when[2] = { { mtime, 0 }, { mtime, 0 } };
	int fd;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(close(fd) == 0);
	assert(utimensat(AT_FDCWD, path, when, 0) == 0);
}

static int there(const char *dir, const char *name)
{
	char path[512];
	struct stat st;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return stat(path, &st) == 0;
}

static void mkfile(const char *dir, const char *name)	/* the test's own tidy-up, one file at a time */
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	assert(remove(path) == 0);
}

static void mkdirs(const char *dir, const char *name)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	assert(mkdir(path, 0755) == 0);
}

static void rmdirs(const char *dir, const char *name)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	assert(rmdir(path) == 0);
}

static void test_names(void)
{
	char far[80];

	assert(provision_name_ok("awvoice-" TOKEN ".xml"));
	assert(provision_name_ok("awvoice-" TOKEN ".xml.tmp"));
	assert(provision_name_ok("awvoice-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.xml"));
	/* not this feature's name at all */
	assert(!provision_name_ok(""));
	assert(!provision_name_ok("awvoice.xml"));
	assert(!provision_name_ok("awvoice-.xml"));
	assert(!provision_name_ok("other-" TOKEN ".xml"));
	assert(!provision_name_ok("awvoice-" TOKEN));			/* no extension */
	assert(!provision_name_ok("awvoice-" TOKEN ".xmlx"));
	assert(!provision_name_ok("awvoice-" TOKEN ".wav"));
	assert(!provision_name_ok("awvoice-" TOKEN ".xml.tmp.tmp"));
	assert(!provision_name_ok("awvoice-0123456789abcdef0123456789abcde.xml"));	/* one short */
	assert(!provision_name_ok("awvoice-0123456789abcdef0123456789abcdef0.xml"));	/* one long */
	assert(!provision_name_ok("awvoice-0123456789abcdef0123456789abcdeg.xml"));	/* not hex */
	assert(!provision_name_ok("awvoice-0123456789ABCDEF0123456789ABCDEF.xml"));	/* the writer draws lowercase */
	assert(!provision_name_ok("awvoice-" TOKEN ".xml/../../etc/passwd"));
	strcpy(far, "awvoice-" TOKEN ".xml");
	assert(provision_name_ok(far));
}

static void test_age(void)
{
	assert(!provision_older_than(NOW - PROVISION_TTL_SEC, NOW, PROVISION_TTL_SEC));	/* exactly at it: kept */
	assert(provision_older_than(NOW - PROVISION_TTL_SEC - 1, NOW, PROVISION_TTL_SEC));	/* one past: gone */
	assert(!provision_older_than(NOW, NOW, PROVISION_TTL_SEC));
	assert(!provision_older_than(NOW + 60, NOW, PROVISION_TTL_SEC));			/* the clock moved back */
	assert(provision_older_than(0, NOW, PROVISION_TTL_SEC));
	assert(provision_older_than(NOW - 5, NOW, 0));					/* a lifetime of zero: everything behind now is past it */
	assert(!provision_older_than(NOW, NOW, 0));
}

static void test_prune(const char *root)
{
	char dir[512];
	int n;

	/* nothing to do: the folder the page has never used, and the folder that is not there */
	snprintf(dir, sizeof(dir), "%s/gone", root);
	assert(provision_prune(dir, NOW, PROVISION_TTL_SEC) == 0);
	assert(provision_prune("", NOW, PROVISION_TTL_SEC) == 0);
	assert(provision_prune(NULL, NOW, PROVISION_TTL_SEC) == 0);

	snprintf(dir, sizeof(dir), "%s/live", root);
	assert(mkdir(dir, 0755) == 0);
	assert(provision_prune(dir, NOW, PROVISION_TTL_SEC) == 0);

	touch(dir, "awvoice-" TOKEN ".xml", NOW - 30);						/* still within its lifetime */
	touch(dir, "awvoice-" TOKEN ".xml.tmp", NOW - PROVISION_TTL_SEC);			/* the writer's tmp, exactly at it */
	touch(dir, "awvoice-ffffffffffffffffffffffffffffffff.xml", NOW - PROVISION_TTL_SEC - 1);	/* one past */
	touch(dir, "awvoice-00000000000000000000000000000000.xml.tmp", NOW - 99999);		/* a tmp from a run that died */
	touch(dir, "index.html", NOW - 99999);							/* the web server's own file */
	touch(dir, "awvoice-xyz.xml", NOW - 99999);							/* someone else's name */
	touch(dir, "awvoice-" TOKEN ".php", NOW - 99999);
	mkdirs(dir, "awvoice-eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee.xml");				/* a folder wearing the name */

	n = provision_prune(dir, NOW, PROVISION_TTL_SEC);
	assert(n == 2);
	assert(there(dir, "awvoice-" TOKEN ".xml"));							/* the fresh one stays */
	assert(!there(dir, "awvoice-ffffffffffffffffffffffffffffffff.xml"));
	assert(!there(dir, "awvoice-00000000000000000000000000000000.xml.tmp"));
	assert(there(dir, "awvoice-" TOKEN ".xml.tmp"));						/* exactly at the lifetime: kept */
	assert(there(dir, "awvoice-eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee.xml"));			/* the folder wearing the name stays */
	assert(there(dir, "index.html"));
	assert(there(dir, "awvoice-xyz.xml"));
	assert(there(dir, "awvoice-" TOKEN ".php"));

	rmdirs(dir, "awvoice-eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee.xml");
	mkfile(dir, "index.html");
	mkfile(dir, "awvoice-xyz.xml");
	mkfile(dir, "awvoice-" TOKEN ".php");
	mkfile(dir, "awvoice-" TOKEN ".xml.tmp");
	mkfile(dir, "awvoice-" TOKEN ".xml");
	assert(rmdir(dir) == 0);
}

int main(void)
{
	char root[] = "/tmp/awvoice-provision-XXXXXX";

	assert(mkdtemp(root));
	test_names();
	test_age();
	test_prune(root);
	rmdir(root);
	puts("provision: all tests passed");
	return 0;
}
