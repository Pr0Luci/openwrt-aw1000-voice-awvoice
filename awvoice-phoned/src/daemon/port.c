// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "port.h"

#define CANDIDATES_MAX	8	/* USB interfaces of the wanted number that carry a tty; more than this is not a router we know */
#define NAME_LEN	256
#define DIR_LEN		1024

/* "<bus>-<port>:1.<iface>": the device part starts with the bus number, the interface part is exactly ours */
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
