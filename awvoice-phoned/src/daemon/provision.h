/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_PROVISION_H
#define AW_PHONED_PROVISION_H

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * The account file a scanned code points a soft phone at. The page's door asks the file writer for one, the writer
 * names it `awvoice-<32 hex>.xml`, and the file carries the account's password. The door tells the admin the address
 * stops being served when its lifetime is through, and that promise needs a keeper: the writer has run and exited by
 * then, and the web server that serves the folder knows nothing of lifetimes. So the daemon's slow round retires what
 * has outlived its own, and the promise holds whether or not anyone asks for another code.
 *
 * Only the names this feature writes are ever removed, because the folder is inside the web server's document root
 * and anything else that appears there belongs to someone else. A name that does not match exactly is left where it
 * is; so is a file that cannot be read, or a folder, or one whose time is not yet through.
 *
 * The folder and the lifetime are the door's own constants (`PROV_DIR`, `PROV_TTL` in the rpcd script) written out
 * again here, because the two halves of this feature live in two languages. If they drift apart this sweep stops
 * retiring files - it can never retire something else - and the door's own run still prunes what it finds.
 */
#define PROVISION_DIR		"/www/awvoice-provision"
#define PROVISION_TTL_SEC	600
#define PROVISION_TOKEN		32	/* hex characters the writer draws at random */
#define PROVISION_PREFIX	"awvoice-"

static inline int provision_name_ok(const char *name)
{
	const char *rest = name + sizeof(PROVISION_PREFIX) - 1;
	size_t k;

	if (strncmp(name, PROVISION_PREFIX, sizeof(PROVISION_PREFIX) - 1))
		return 0;
	for (k = 0; k < PROVISION_TOKEN; k++) {
		char c = rest[k];
		if ((c < '0' || c > '9') && (c < 'a' || c > 'f'))
			return 0;	/* a name this feature never writes, so never a file to retire */
	}
	return !strcmp(rest + PROVISION_TOKEN, ".xml") || !strcmp(rest + PROVISION_TOKEN, ".xml.tmp");
}

/* the file's own clock against the present: exactly at the lifetime it is kept, one second past it is gone */
static inline int provision_older_than(time_t mtime, time_t now, long ttl)
{
	return now > mtime && (unsigned long)(now - mtime) > (unsigned long)ttl;
}

/* one sweep of one folder: the files this feature wrote that have outlived their lifetime are removed, and the
   count of them is what comes back. A folder that is not there - the page not installed, or nothing scanned yet -
   is nothing to do. */
static inline int provision_prune(const char *dir, time_t now, long ttl)
{
	char path[PATH_MAX];
	struct dirent *e;
	struct stat st;
	DIR *d;
	int removed = 0;

	if (!dir || !*dir)
		return 0;
	d = opendir(dir);
	if (!d)
		return 0;
	while ((e = readdir(d)) != NULL) {
		if (!provision_name_ok(e->d_name))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path))
			continue;
		if (stat(path, &st) || !S_ISREG(st.st_mode))
			continue;	/* unreadable, or a folder wearing the name: not this daemon's to remove */
		if (!provision_older_than(st.st_mtime, now, ttl))
			continue;
		if (unlink(path) == 0)
			removed++;
	}
	closedir(d);
	return removed;
}

#endif /* AW_PHONED_PROVISION_H */
