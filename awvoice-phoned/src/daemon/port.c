// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "port.h"

#define CANDIDATES_MAX	8	/* USB interfaces of the wanted number that carry a tty; more than this is not a router this build serves */
#define NAME_LEN	256
#define DIR_LEN		1024
#define HOLDER_PIDS	1024	/* processes scanned for an open descriptor; a tree past this is beyond what this scan reads */
#define HOLDER_FDS	256	/* descriptors scanned per process, which covers a daemon and a shell */

/* "<bus>-<port>:1.<iface>": the device part starts with the bus number, the interface part is exactly the one asked for */
static int is_interface(const char *name, unsigned int iface)
{
	char tail[16];
	const char *colon = strchr(name, ':');

	if (!colon || !isdigit((unsigned char)name[0]))
		return 0;
	snprintf(tail, sizeof(tail), ":1.%u", iface);
	return !strcmp(colon, tail);
}

/* the tty child of one interface directory ("ttyUSB<n>"), the lowest name when there are several */
static int tty_child(const char *dir, char *name, size_t cap)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int found = 0;

	if (!d)
		return 0;
	while ((e = readdir(d))) {
		const char *digits = e->d_name + 6;

		if (strncmp(e->d_name, "ttyUSB", 6) || !*digits || strspn(digits, "0123456789") != strlen(digits))
			continue;
		if (found && strcmp(e->d_name, name) >= 0)
			continue;
		snprintf(name, cap, "%s", e->d_name);
		found = 1;
	}
	closedir(d);
	return found;
}

int port_find(const char *root, unsigned int iface, char *path, size_t cap)
{
	char seen[CANDIDATES_MAX][NAME_LEN], tty[CANDIDATES_MAX][NAME_LEN];
	static int warned;
	struct dirent *e;
	unsigned int count = 0, k, best = 0;
	DIR *top = opendir(root);

	if (!top)
		return -ENOENT;
	while (count < CANDIDATES_MAX && (e = readdir(top))) {
		char dir[DIR_LEN];

		if (!is_interface(e->d_name, iface) || snprintf(dir, sizeof(dir), "%s/%s", root, e->d_name) >= (int)sizeof(dir))
			continue;
		if (tty_child(dir, tty[count], NAME_LEN)) {
			snprintf(seen[count], NAME_LEN, "%s", e->d_name);
			count++;
		}
	}
	closedir(top);
	if (!count)
		return -ENOENT;
	for (k = 1; k < count; k++)
		if (strcmp(seen[k], seen[best]) < 0)
			best = k;
	if (count > 1 && !warned) {
		warned = 1;
		printf("more than one USB device has a serial port on interface %u: the first in name order is used\n", iface);
	}
	return snprintf(path, cap, "/dev/%s", tty[best]) >= (int)cap ? -ENAMETOOLONG : 0;
}

int port_exclusive(int fd)
{
#ifdef TIOCEXCL
	if (fd < 0)
		return -EBADF;
	if (ioctl(fd, TIOCEXCL) < 0)
		return -errno;
	return 0;
#else
	(void)fd;
	return -ENOTSUP;
#endif
}

static int all_digits(const char *s)
{
	if (!*s)
		return 0;
	for (; *s; s++)
		if (*s < '0' || *s > '9')
			return 0;
	return 1;
}

/* the holder's command name, from its comm; "" when it cannot be read (the descriptor is still the proof) */
static void holder_comm(const char *root, const char *pidname, char *comm, size_t cap)
{
	char path[DIR_LEN], line[64];
	size_t k;
	FILE *f;

	if (!comm || !cap)
		return;
	comm[0] = 0;
	if (snprintf(path, sizeof(path), "%s/%s/comm", root, pidname) >= (int)sizeof(path))
		return;
	f = fopen(path, "r");
	if (!f)
		return;
	if (fgets(line, sizeof(line), f)) {
		k = strlen(line);
		while (k && (line[k - 1] == '\n' || line[k - 1] == '\r'))
			line[--k] = 0;
		snprintf(comm, cap, "%s", line);
	}
	fclose(f);
}

int port_holder(const char *proc_root, const char *path, int self_pid, int *pid, char *comm, size_t cap)
{
	struct dirent *e;
	unsigned int seen = 0;
	DIR *top;

	if (!path || !path[0])
		return -EINVAL;
	top = opendir(proc_root);
	if (!top)
		return -ENOENT;
	while (seen < HOLDER_PIDS && (e = readdir(top))) {
		char pidname[16], fddir[DIR_LEN], fd[DIR_LEN + 32], link[PATH_MAX];
		struct dirent *f;
		unsigned int n;
		DIR *fds;
		int one;

		if (!all_digits(e->d_name) || strlen(e->d_name) >= sizeof(pidname))
			continue;
		one = atoi(e->d_name);
		if (one <= 0 || one == self_pid)
			continue;
		seen++;
		snprintf(pidname, sizeof(pidname), "%s", e->d_name);
		if (snprintf(fddir, sizeof(fddir), "%s/%s/fd", proc_root, pidname) >= (int)sizeof(fddir))
			continue;
		fds = opendir(fddir);
		if (!fds)
			continue;
		for (n = 0; n < HOLDER_FDS && (f = readdir(fds)); n++) {
			ssize_t len;

			if (!all_digits(f->d_name))
				continue;
			if (snprintf(fd, sizeof(fd), "%s/%s", fddir, f->d_name) >= (int)sizeof(fd))
				continue;
			len = readlink(fd, link, sizeof(link) - 1);
			if (len < 0)
				continue;
			link[len] = 0;
			if (strcmp(link, path))
				continue;
			if (pid)
				*pid = one;
			holder_comm(proc_root, pidname, comm, cap);
			closedir(fds);
			closedir(top);
			return 1;
		}
		closedir(fds);
	}
	closedir(top);
	return 0;
}
