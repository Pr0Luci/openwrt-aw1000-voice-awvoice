/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_STORAGE_H
#define AW_PHONED_STORAGE_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/*
 * The two folders the daemon keeps files in — the call recordings and the answering machine's messages — under one
 * set of rules: which names are ours, how a folder is made and proven, how much room it has left, and which of its
 * files the budget takes. The files are called <epoch>-<call id>.wav, both parts decimal: no caller's number ever
 * enters a name, and the daemon never touches a file it did not write, because the two folders can be pointed at a
 * shared USB mount that holds anything else.
 *
 * A folder is only what `storage_prepare` proves at the moment it runs: the directory exists and is one, the
 * filesystem is not read only, and a probe file really could be created and removed. The probe is the honest test
 * of a mount — statvfs alone would call a read-only or a remounted-gone folder ready. And a folder under /mnt is
 * only used when its drive is mounted (storage_mount_of, against /proc/mounts): making it otherwise would put the
 * files on the router's own flash, under a mount point that hides them later.
 *
 * All plain file work on paths and names, so the host tests cover it whole (tests/test_storage.c).
 */
#define STORAGE_NAME_MAX	64	/* the page's rule too: a name above this is not served */
#define STORAGE_PATH_MAX	256	/* a folder path (config.h's 128), a slash and a name */
#define STORAGE_LIST_MAX	256	/* the oldest files one prune looks at */
#define STORAGE_PROBE		".awvoice-probe"
#define STORAGE_MIN_FREE_MB	1	/* under this a call's file would fail mid-way: better refused at the start */

/* exactly <epoch>-<call id>.wav: two non-empty decimal parts and one dash, nothing else */
static inline int storage_name_ok(const char *name)
{
	size_t len = strlen(name), k, dash = 0;

	if (len < 7 || len > STORAGE_NAME_MAX || strcmp(name + len - 4, ".wav"))
		return 0;
	for (k = 0; k < len - 4; k++) {
		if (name[k] == '-') {
			if (dash || k == 0 || k == len - 5)
				return 0;
			dash = k;
		} else if (name[k] < '0' || name[k] > '9') {
			return 0;
		}
	}
	return dash != 0;
}

/* a decimal field that saturates rather than wraps: a 60-digit field is absurd, and it must still compare sanely */
static inline void storage_num_add(unsigned long long *v, unsigned int digit)
{
	if (*v > (ULLONG_MAX - digit) / 10)
		*v = ULLONG_MAX;
	else
		*v = *v * 10 + digit;
}

/* the two numbers of a name; -EINVAL when it is not one of ours */
static inline int storage_name_ids(const char *name, unsigned long long *epoch, unsigned long long *id)
{
	const char *p = name;
	unsigned long long a = 0, b = 0;

	if (!storage_name_ok(name))
		return -EINVAL;
	while (*p >= '0' && *p <= '9')
		storage_num_add(&a, (unsigned int)(*p++ - '0'));
	p++;		/* the dash the rule above found */
	while (*p >= '0' && *p <= '9')
		storage_num_add(&b, (unsigned int)(*p++ - '0'));
	*epoch = a;
	*id = b;
	return 0;
}

/* the order the budget deletes in: the epoch, then the call id, then the text (two saturated fields need a tie-break) */
static inline int storage_name_cmp(const char *a, const char *b)
{
	unsigned long long ea, ia, eb, ib;

	if (storage_name_ids(a, &ea, &ia) || storage_name_ids(b, &eb, &ib))
		return strcmp(a, b);
	if (ea != eb)
		return ea < eb ? -1 : 1;
	if (ia != ib)
		return ia < ib ? -1 : 1;
	return strcmp(a, b);
}

/* a folder setting, in place: absolute, no trailing slashes, one slash between the components, never the root,
 * never a control character (the log is one line per message and the page shows the path); -EINVAL with the text
 * untouched when it is not one */
static inline int storage_path_norm(char *path, size_t size)
{
	size_t len = strlen(path), k, to, from;

	if (!len || len >= size || path[0] != '/')
		return -EINVAL;
	for (k = 0; k < len; k++)
		if ((unsigned char)path[k] < 0x20 || (unsigned char)path[k] == 0x7f)
			return -EINVAL;
	while (len > 1 && path[len - 1] == '/')
		path[--len] = 0;
	/* the doubled slashes a paste can carry go too: one name per component */
	for (to = 1, from = 1; from < len; from++)
		if (!(path[from] == '/' && path[to - 1] == '/'))
			path[to++] = path[from];
	path[to] = 0;
	return to > 1 ? 0 : -EINVAL;	/* "/" is the system's, not a folder of ours */
}

struct storage_state {
	int ready;			/* the folder was proven at the last check */
	int err;			/* why not, an errno value (0 when ready) */
	unsigned long long free_mb;	/* the room left on its filesystem, MiB (0 when not ready) */
	int readonly;			/* the filesystem is mounted read-only */
};

/* make the folder if it is not there (one level, 0755), prove a file can land in it, and measure the room;
 * 0 and ready, or -errno with the reason in st->err */
static inline int storage_prepare(const char *dir, struct storage_state *st)
{
	char path[STORAGE_PATH_MAX];
	struct statvfs vfs;
	struct stat sb;
	int fd, err;

	memset(st, 0, sizeof(*st));
	if (!dir || !*dir)
		return -(st->err = EINVAL);
	if (mkdir(dir, 0755) && errno != EEXIST)
		return -(st->err = errno);
	if (stat(dir, &sb)) {
		err = errno;
	} else if (!S_ISDIR(sb.st_mode)) {
		err = ENOTDIR;		/* a file carries the folder's name: no file of ours can land beside it */
	} else if (statvfs(dir, &vfs)) {
		err = errno;
	} else if (vfs.f_flag & ST_RDONLY) {
		st->readonly = 1;
		err = EROFS;
	} else if (snprintf(path, sizeof(path), "%s/%s", dir, STORAGE_PROBE) >= (int)sizeof(path)) {
		err = ENAMETOOLONG;
	} else if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) < 0) {
		err = errno;
	} else {
		close(fd);
		err = unlink(path) && errno != ENOENT ? errno : 0;
	}
	if (err)
		return -(st->err = err);
	st->ready = 1;
	st->free_mb = (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize / (1024 * 1024);
	return 0;
}

/* whether a call could start a file in the folder now: ready, and room for more than a moment of audio */
static inline int storage_can_write(const struct storage_state *st)
{
	return st->ready && st->free_mb >= STORAGE_MIN_FREE_MB;
}

/* the daemon's own tmp files of a killed write; another program's .tmp is never ours to remove. A probe left between
 * its creation and its removal (a killed check) is litter of ours too and goes, uncounted. Returns how many tmps */
static inline unsigned int storage_tmp_clean(const char *dir)
{
	char path[STORAGE_PATH_MAX], base[STORAGE_NAME_MAX + 1];
	struct dirent *e;
	unsigned int removed = 0;
	DIR *dp;

	if (!dir || !*dir)
		return 0;
	if (snprintf(path, sizeof(path), "%s/%s", dir, STORAGE_PROBE) < (int)sizeof(path))
		(void)unlink(path);
	dp = opendir(dir);		/* a folder that is not there has no tmp files */
	while (dp && (e = readdir(dp)) != NULL) {
		size_t len = strlen(e->d_name);

		if (len <= 4 || len - 4 > STORAGE_NAME_MAX || strcmp(e->d_name + len - 4, ".tmp"))
			continue;
		memcpy(base, e->d_name, len - 4);
		base[len - 4] = 0;
		if (!storage_name_ok(base))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) < (int)sizeof(path) && !unlink(path))
			removed++;
	}
	if (dp)
		closedir(dp);
	return removed;
}

/* the mount point of `mounts` that carries `dir`: the longest one that is a prefix at a component boundary, or NULL
 * when even the root does not match. A folder under a mount point that is not in the list would be made on whatever
 * carries its parent — on a router, the flash under an unmounted USB — so the caller refuses it instead */
static inline const char *storage_mount_of(const char *dir, const char *const *mounts, unsigned int n)
{
	const char *best = NULL;
	size_t best_len = 0;
	unsigned int k;

	if (!dir || dir[0] != '/')
		return NULL;
	for (k = 0; k < n; k++) {
		size_t len;

		if (!mounts[k] || !mounts[k][0])
			continue;
		len = strlen(mounts[k]);
		if (len <= best_len)
			continue;
		if (len == 1 && mounts[k][0] == '/') {	/* the root carries everything under it */
			best = mounts[k];
			best_len = 1;
			continue;
		}
		if (strncmp(dir, mounts[k], len) || (dir[len] && dir[len] != '/'))
			continue;
		best = mounts[k];
		best_len = len;
	}
	return best;
}

struct storage_entry {
	char name[STORAGE_NAME_MAX + 1];
	unsigned int bytes;
};

static inline void storage_entry_insert(struct storage_entry *list, unsigned int at, const char *name, unsigned int bytes)
{
	while (at > 0 && storage_name_cmp(list[at - 1].name, name) > 0) {
		list[at] = list[at - 1];
		at--;
	}
	memcpy(list[at].name, name, strlen(name) + 1);
	list[at].bytes = bytes;
}

/* the folder's own finished files, oldest first. All entries are read, so the oldest are the ones kept even in a
 * folder that grew past `max`; the caller is told how many it got and prunes those (the next prune takes the rest) */
static inline unsigned int storage_list(const char *dir, struct storage_entry *list, unsigned int max, int *more)
{
	char path[STORAGE_PATH_MAX];
	struct dirent *e;
	struct stat sb;
	unsigned int n = 0;
	DIR *dp;

	if (more)
		*more = 0;
	if (!max || !dir || !*dir)
		return 0;
	dp = opendir(dir);
	while (dp && (e = readdir(dp)) != NULL) {
		unsigned int bytes;

		if (!storage_name_ok(e->d_name))
			continue;
		if (n == max && storage_name_cmp(e->d_name, list[max - 1].name) >= 0) {
			if (more)
				*more = 1;	/* newer than every file kept: not one of the oldest, but it is there */
			continue;
		}
		if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path))
			continue;
		if (stat(path, &sb) || !S_ISREG(sb.st_mode))
			continue;
		bytes = sb.st_size < 0 ? 0 : sb.st_size > 0xffffffffll ? 0xffffffffu : (unsigned int)sb.st_size;
		storage_entry_insert(list, n == max ? max - 1 : n, e->d_name, bytes);
		if (n < max)
			n++;
		else if (more)
			*more = 1;
	}
	if (dp)
		closedir(dp);
	return n;
}

#endif /* AW_PHONED_STORAGE_H */
