// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The storage rules on real files under /tmp: make -f Makefile.host test */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage.h"

static void test_names(void)
{
	char long_name[STORAGE_NAME_MAX + 2];
	unsigned long long epoch = 0, id = 0;

	assert(storage_name_ok("1700000000-1.wav"));
	assert(storage_name_ok("7-7.wav"));			/* the shortest ours can be */
	assert(storage_name_ok("0-0.wav"));
	/* another program's file, or one that never came from the recorder */
	assert(!storage_name_ok("123.wav"));
	assert(!storage_name_ok("1.wav"));
	assert(!storage_name_ok("-1.wav"));
	assert(!storage_name_ok("1700-.wav"));
	assert(!storage_name_ok("17-1-2.wav"));
	assert(!storage_name_ok("17a-1.wav"));
	assert(!storage_name_ok("1700000000-1.WAV"));
	assert(!storage_name_ok("1700000000-1.wavx"));
	assert(!storage_name_ok("1700000000-1.wav.tmp"));
	assert(!storage_name_ok("1700000000-1.mp3"));
	assert(!storage_name_ok(""));
	memset(long_name, '9', sizeof(long_name));
	memcpy(long_name + STORAGE_NAME_MAX - 6, "-1.wav", 6);	/* 58 nines and "-1.wav": exactly the cap */
	long_name[STORAGE_NAME_MAX] = 0;
	assert(storage_name_ok(long_name));
	memcpy(long_name + STORAGE_NAME_MAX - 5, "-1.wav", 7);	/* one more nine with its terminator: one over it */
	assert(!storage_name_ok(long_name));

	assert(storage_name_ids("123.wav", &epoch, &id) == -EINVAL);
	assert(!storage_name_ids("1-1.wav", &epoch, &id) && epoch == 1 && id == 1);
	assert(!storage_name_ids("1700000000-42.wav", &epoch, &id) && epoch == 1700000000ull && id == 42);
	/* a field longer than 64 bits saturates instead of wrapping: the order stays usable */
	assert(!storage_name_ids("999999999999999999999999999999-1.wav", &epoch, &id) && epoch == ULLONG_MAX && id == 1);
}

static void test_order(void)
{
	assert(storage_name_cmp("9-99.wav", "10-1.wav") < 0);		/* the epoch is a number, not text */
	assert(storage_name_cmp("10-2.wav", "10-10.wav") < 0);		/* and so is the call id */
	assert(storage_name_cmp("10-10.wav", "10-10.wav") == 0);
	assert(storage_name_cmp("10-10.wav", "11-1.wav") < 0);
	assert(storage_name_cmp("11-1.wav", "10-10.wav") > 0);
	/* saturated fields tie on both numbers: the text decides, and it is still a total order */
	assert(storage_name_cmp("999999999999999999999999999999-1.wav", "999999999999999999999999999998-1.wav") > 0);
}

static void test_paths(void)
{
	char p[64];

	snprintf(p, sizeof(p), "%s", "/mnt/sda1/awvoice/");
	assert(storage_path_norm(p, sizeof(p)) == 0 && !strcmp(p, "/mnt/sda1/awvoice"));
	snprintf(p, sizeof(p), "%s", "/mnt/sda1///");
	assert(storage_path_norm(p, sizeof(p)) == 0 && !strcmp(p, "/mnt/sda1"));
	snprintf(p, sizeof(p), "%s", "/a");
	assert(storage_path_norm(p, sizeof(p)) == 0 && !strcmp(p, "/a"));
	snprintf(p, sizeof(p), "%s", "/mnt//sda1/awvoice");
	assert(storage_path_norm(p, sizeof(p)) == 0 && !strcmp(p, "/mnt/sda1/awvoice"));	/* a pasted double slash */
	snprintf(p, sizeof(p), "%s", "//mnt");
	assert(storage_path_norm(p, sizeof(p)) == 0 && !strcmp(p, "/mnt"));
	snprintf(p, sizeof(p), "%s", "/mnt/sda1////");
	assert(storage_path_norm(p, sizeof(p)) == 0 && !strcmp(p, "/mnt/sda1"));
	snprintf(p, sizeof(p), "%s", "/");
	assert(storage_path_norm(p, sizeof(p)) == -EINVAL);		/* the root is never a folder of ours */
	snprintf(p, sizeof(p), "%s", "//");
	assert(storage_path_norm(p, sizeof(p)) == -EINVAL);
	snprintf(p, sizeof(p), "%s", "mnt/sda1");
	assert(storage_path_norm(p, sizeof(p)) == -EINVAL);
	snprintf(p, sizeof(p), "%s", "");
	assert(storage_path_norm(p, sizeof(p)) == -EINVAL);
	snprintf(p, sizeof(p), "%s", "/a\nb");
	assert(storage_path_norm(p, sizeof(p)) == -EINVAL);		/* a line break would split the log */
	snprintf(p, sizeof(p), "%s", "/a\tb");
	assert(storage_path_norm(p, sizeof(p)) == -EINVAL);
	snprintf(p, sizeof(p), "%s", "/a/");
	assert(storage_path_norm(p, 3) == -EINVAL);			/* it did not fit its buffer */
}

/* a missing folder is made, proven and measured; the probe never stays */
static void test_prepare(const char *root)
{
	struct storage_state st;
	char dir[300], probe[300];

	snprintf(dir, sizeof(dir), "%s/fresh", root);
	assert(storage_prepare(dir, &st) == 0);
	assert(st.ready && !st.err && st.free_mb > 0 && !st.readonly);
	assert(access(dir, F_OK) == 0);
	snprintf(probe, sizeof(probe), "%s/%s", dir, STORAGE_PROBE);
	assert(access(probe, F_OK) == -1 && errno == ENOENT);
	assert(storage_can_write(&st));
	/* a second check on the folder that is there is fine */
	assert(storage_prepare(dir, &st) == 0 && st.ready);
	rmdir(dir);
}

/* every way the folder cannot be used is said with its reason */
static void test_refusals(const char *root)
{
	struct storage_state st;
	char dir[300], file[300];
	FILE *f;

	/* a file where the folder should be */
	snprintf(file, sizeof(file), "%s/blocked", root);
	f = fopen(file, "w");
	assert(f && fputs("x", f) >= 0);
	fclose(f);
	assert(storage_prepare(file, &st) == -ENOTDIR && !st.ready && st.err == ENOTDIR);
	assert(!storage_can_write(&st));

	/* a folder whose parent is not there: one level is made, never a chain */
	snprintf(dir, sizeof(dir), "%s/no/such/dir", root);
	assert(storage_prepare(dir, &st) == -ENOENT && !st.ready && st.err == ENOENT);

	/* an empty path */
	assert(storage_prepare("", &st) == -EINVAL && !st.ready && st.err == EINVAL);
	unlink(file);
}

/* only the daemon's own tmp names go: the folder may be shared */
static void test_tmp_clean(const char *root)
{
	char dir[300], path[320];

	snprintf(dir, sizeof(dir), "%s/tmpclean", root);
	assert(mkdir(dir, 0755) == 0);
	snprintf(path, sizeof(path), "%s/1700000000-1.wav.tmp", dir);	assert(!close(open(path, O_CREAT | O_WRONLY, 0600)));
	snprintf(path, sizeof(path), "%s/1700000001-9.wav.tmp", dir);	assert(!close(open(path, O_CREAT | O_WRONLY, 0600)));
	snprintf(path, sizeof(path), "%s/notes.tmp", dir);		assert(!close(open(path, O_CREAT | O_WRONLY, 0600)));
	snprintf(path, sizeof(path), "%s/123.wav.tmp", dir);		assert(!close(open(path, O_CREAT | O_WRONLY, 0600)));
	snprintf(path, sizeof(path), "%s/1700000002-2.wav", dir);	assert(!close(open(path, O_CREAT | O_WRONLY, 0600)));
	snprintf(path, sizeof(path), "%s/%s", dir, STORAGE_PROBE);	assert(!close(open(path, O_CREAT | O_WRONLY, 0600)));	/* a killed check's probe */
	assert(storage_tmp_clean(dir) == 2);			/* the probe goes too, and is not counted as a tmp */
	assert(access(path, F_OK) == -1);
	assert(storage_name_ok(".awvoice-probe") == 0);		/* it is litter, never one of our files */
	snprintf(path, sizeof(path), "%s/1700000002-2.wav", dir);
	assert(access(path, F_OK) == 0);				/* a finished file is not a tmp */
	snprintf(path, sizeof(path), "%s/notes.tmp", dir);	assert(access(path, F_OK) == 0);
	snprintf(path, sizeof(path), "%s/123.wav.tmp", dir);	assert(access(path, F_OK) == 0);
	snprintf(path, sizeof(path), "%s/1700000000-1.wav.tmp", dir); assert(access(path, F_OK) == -1);
	snprintf(path, sizeof(path), "%s/1700000001-9.wav.tmp", dir); assert(access(path, F_OK) == -1);
	assert(storage_tmp_clean("/no/such/folder") == 0);
	snprintf(path, sizeof(path), "%s/notes.tmp", dir);	unlink(path);
	snprintf(path, sizeof(path), "%s/123.wav.tmp", dir);	unlink(path);
	snprintf(path, sizeof(path), "%s/1700000002-2.wav", dir); unlink(path);
	rmdir(dir);
}

/* the mount point that carries a folder: the longest at a component boundary, so /mnt/sda1 and /mnt/sda1x differ */
static void test_mount_of(void)
{
	static const char *const mounts[] = { "/", "/mnt", "/mnt/sda1", "/run/overlay" };

	assert(!strcmp(storage_mount_of("/etc/awvoice/recordings", mounts, 4), "/"));
	assert(!strcmp(storage_mount_of("/mnt/sda1/awvoice", mounts, 4), "/mnt/sda1"));
	assert(!strcmp(storage_mount_of("/mnt/sda1", mounts, 4), "/mnt/sda1"));		/* the mount point itself */
	assert(!strcmp(storage_mount_of("/mnt/sda1x/rec", mounts, 4), "/mnt"));		/* a component, not a prefix */
	assert(!strcmp(storage_mount_of("/mnt/sda2/rec", mounts, 4), "/mnt"));		/* an unmounted drive: the parent */
	assert(!strcmp(storage_mount_of("/mnt", mounts, 4), "/mnt"));
	assert(!strcmp(storage_mount_of("/run/overlay/aws", mounts, 4), "/run/overlay"));
	assert(storage_mount_of("relative/path", mounts, 4) == NULL);
	assert(storage_mount_of("", mounts, 4) == NULL);
	assert(storage_mount_of("/etc", NULL, 0) == NULL);					/* nothing mounted: nothing carries it */
	assert(storage_mount_of("/etc", mounts, 0) == NULL);
}

/* the list is the folder's own files, oldest first, whatever order the filesystem hands them out in */
static void test_list(const char *root)
{
	struct storage_entry list[8];
	char dir[300], path[320];
	unsigned int n;
	int more;

	snprintf(dir, sizeof(dir), "%s/list", root);
	assert(mkdir(dir, 0755) == 0);
	snprintf(path, sizeof(path), "%s/1700000300-1.wav", dir); assert(!close(open(path, O_CREAT | O_WRONLY, 0600)));
	snprintf(path, sizeof(path), "%s/1700000100-9.wav", dir); assert(!close(open(path, O_CREAT | O_WRONLY, 0600)));
	truncate(path, 100);
	snprintf(path, sizeof(path), "%s/1700000200-2.wav", dir); assert(!close(open(path, O_CREAT | O_WRONLY, 0600)));
	truncate(path, 200);
	snprintf(path, sizeof(path), "%s/1700000200-10.wav", dir); assert(!close(open(path, O_CREAT | O_WRONLY, 0600)));
	truncate(path, 50);
	snprintf(path, sizeof(path), "%s/1700000400-1.wav.tmp", dir); assert(!close(open(path, O_CREAT | O_WRONLY, 0600)));
	snprintf(path, sizeof(path), "%s/notes.wav", dir); assert(!close(open(path, O_CREAT | O_WRONLY, 0600)));
	snprintf(path, sizeof(path), "%s/123.wav", dir); assert(!close(open(path, O_CREAT | O_WRONLY, 0600)));

	n = storage_list(dir, list, 8, &more);
	assert(n == 4 && !more);
	assert(!strcmp(list[0].name, "1700000100-9.wav") && list[0].bytes == 100);
	assert(!strcmp(list[1].name, "1700000200-2.wav") && list[1].bytes == 200);
	assert(!strcmp(list[2].name, "1700000200-10.wav") && list[2].bytes == 50);
	assert(!strcmp(list[3].name, "1700000300-1.wav") && list[3].bytes == 0);

	/* a cap smaller than the folder keeps the oldest of them, and says that more are there */
	n = storage_list(dir, list, 2, &more);
	assert(n == 2 && more);
	assert(!strcmp(list[0].name, "1700000100-9.wav"));
	assert(!strcmp(list[1].name, "1700000200-2.wav"));

	n = storage_list(dir, list, 1, NULL);
	assert(n == 1 && !strcmp(list[0].name, "1700000100-9.wav"));
	assert(storage_list("/no/such/folder", list, 8, &more) == 0 && !more);

	snprintf(path, sizeof(path), "%s/1700000100-9.wav", dir); unlink(path);
	snprintf(path, sizeof(path), "%s/1700000200-2.wav", dir); unlink(path);
	snprintf(path, sizeof(path), "%s/1700000200-10.wav", dir); unlink(path);
	snprintf(path, sizeof(path), "%s/1700000300-1.wav", dir); unlink(path);
	snprintf(path, sizeof(path), "%s/1700000400-1.wav.tmp", dir); unlink(path);
	snprintf(path, sizeof(path), "%s/notes.wav", dir); unlink(path);
	snprintf(path, sizeof(path), "%s/123.wav", dir); unlink(path);
	rmdir(dir);
}

int main(void)
{
	char root[128];

	snprintf(root, sizeof(root), "/tmp/awvoice-storage-test.%ld", (long)getpid());
	assert(mkdir(root, 0755) == 0 || errno == EEXIST);
	test_names();
	test_order();
	test_paths();
	test_prepare(root);
	test_refusals(root);
	test_tmp_clean(root);
	test_mount_of();
	test_list(root);
	rmdir(root);
	puts("storage: all tests passed");
	return 0;
}
