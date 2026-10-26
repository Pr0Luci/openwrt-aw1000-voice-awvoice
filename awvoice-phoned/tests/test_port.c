// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The AT port finder against a temporary tree that mimics sysfs: make -f Makefile.host test */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "port.h"

static char root[64];

static void make_dir(const char *rel)
{
	char path[1024];

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	if (mkdir(path, 0700)) { perror(path); abort(); }
}

static void make_tree(void)
{
	snprintf(root, sizeof(root), "/tmp/aw-port-test-XXXXXX");
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

static void test_one_match(void)
{
	char path[64];

	make_tree();
	make_dir("usb1");					/* the bus itself: no colon, never a match */
	make_dir("1-1");
	make_dir("1-1.2:1.0");					/* another interface of the modem, with its own tty */
	make_dir("1-1.2:1.0/ttyUSB0");
	make_dir("1-1.2:1.2");
	make_dir("1-1.2:1.2/ttyUSB2");
	make_dir("1-1.2:1.20");					/* a different interface whose number starts with ours */
	make_dir("1-1.2:1.20/ttyUSB9");
	make_dir("1-1.2:1.3");
	make_dir("1-1.2:1.3/ttyUSB3");
	assert(port_find(root, 2, path, sizeof(path)) == 0 && !strcmp(path, "/dev/ttyUSB2"));
	assert(port_find(root, 0, path, sizeof(path)) == 0 && !strcmp(path, "/dev/ttyUSB0"));
	assert(port_find(root, 3, path, sizeof(path)) == 0 && !strcmp(path, "/dev/ttyUSB3"));
	assert(port_find(root, 20, path, sizeof(path)) == 0 && !strcmp(path, "/dev/ttyUSB9"));
	drop_tree();
}

static void test_none(void)
{
	char path[64] = "unchanged";

	make_tree();
	assert(port_find(root, 2, path, sizeof(path)) == -ENOENT);	/* an empty tree */
	make_dir("1-1.2:1.2");						/* the interface exists but carries no tty */
	make_dir("1-1.2:1.2/driver");
	make_dir("1-1.2:1.2/ttyUSBx");					/* not a number */
	make_dir("1-1.2:1.2/ttyUSB");
	make_dir("1-1.2:1.2/tty");
	assert(port_find(root, 2, path, sizeof(path)) == -ENOENT);
	make_dir("1-1.2:1.5");
	make_dir("1-1.2:1.5/ttyUSB5");
	assert(port_find(root, 2, path, sizeof(path)) == -ENOENT);	/* another interface's tty does not count */
	assert(!strcmp(path, "unchanged"));
	drop_tree();
	assert(port_find(root, 2, path, sizeof(path)) == -ENOENT);	/* no tree at all */
	assert(port_find("/nonexistent/aw-port-test", 2, path, sizeof(path)) == -ENOENT);
}

static void test_two_matches(void)
{
	char path[64];

	make_tree();
	make_dir("2-1:1.2");
	make_dir("2-1:1.2/ttyUSB3");
	make_dir("1-1:1.2");
	make_dir("1-1:1.2/ttyUSB5");
	make_dir("1-1:1.2/ttyUSB4");
	/* the first interface in name order, and the lowest tty name inside it */
	assert(port_find(root, 2, path, sizeof(path)) == 0 && !strcmp(path, "/dev/ttyUSB4"));
	drop_tree();
}

static void test_symlinked_interface(void)
{
	char path[64], target[128], link[160];

	/* sysfs lists the interfaces as links into the device tree */
	make_tree();
	make_dir("devices");
	make_dir("devices/real");
	make_dir("devices/real/ttyUSB7");
	snprintf(target, sizeof(target), "%s/devices/real", root);
	snprintf(link, sizeof(link), "%s/1-1.4:1.2", root);
	assert(symlink(target, link) == 0);
	assert(port_find(root, 2, path, sizeof(path)) == 0 && !strcmp(path, "/dev/ttyUSB7"));
	drop_tree();
}

static void test_long_names(void)
{
	char path[64], name[300], rel[400];

	make_tree();
	make_dir("1-1.2:1.2");
	make_dir("1-1.2:1.2/ttyUSB2");
	/* the answer does not fit the caller's buffer */
	assert(port_find(root, 2, path, 12) == -ENAMETOOLONG);		/* "/dev/ttyUSB2" needs 13 */
	assert(port_find(root, 2, path, 13) == 0 && !strcmp(path, "/dev/ttyUSB2"));
	assert(port_find(root, 2, path, 1) == -ENAMETOOLONG);
	drop_tree();

	/* a very long tty name is refused, not cut */
	make_tree();
	make_dir("1-1.2:1.2");
	memcpy(name, "ttyUSB", 6);
	memset(name + 6, '1', 200);
	name[206] = 0;
	snprintf(rel, sizeof(rel), "1-1.2:1.2/%s", name);
	make_dir(rel);
	assert(port_find(root, 2, path, sizeof(path)) == -ENAMETOOLONG);
	drop_tree();

	/* a very long interface name beside the real one: both are matches, the first in name order is taken */
	make_tree();
	memset(name, '1', 250);
	memcpy(name + 250, ":1.2", 5);
	make_dir(name);
	snprintf(rel, sizeof(rel), "%s/ttyUSB8", name);
	make_dir(rel);
	make_dir("1-1.2:1.2");
	make_dir("1-1.2:1.2/ttyUSB2");
	assert(port_find(root, 2, path, sizeof(path)) == 0 && !strcmp(path, "/dev/ttyUSB2"));	/* '-' sorts before '1' */
	drop_tree();
}

int main(void)
{
	test_one_match();
	test_none();
	test_two_matches();
	test_symlinked_interface();
	test_long_names();
	printf("port: all tests passed\n");
	return 0;
}
