/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_PHONED_PORT_H
#define AW_PHONED_PORT_H

#include <stddef.h>

/*
 * The modem's AT port, found by USB interface number and never by tty name (tty numbers change after a modem reset:
 * ). The sysfs layout on this router:
 *   /sys/bus/usb/devices/<bus>-<port>:1.<iface>/ttyUSB<n>      ->  /dev/ttyUSB<n>
 * The modem is the device whose interface <iface> carries a ttyUSB child; with more than one match the first in
 * name order is taken and the others are logged once.
 */

#define PORT_SYSFS_ROOT		"/sys/bus/usb/devices"
#define PORT_RETRY_MS		2000	/* while the port is missing, look again this often */
#define PORT_MISSING_LOG_MS	60000	/* and say so in the log at most this often */

/*
 * Find the tty of USB interface iface under root (PORT_SYSFS_ROOT, or a test tree). Writes "/dev/ttyUSB<n>" to
 * path. 0, -ENOENT when there is none, -ENAMETOOLONG. Reads directory names only; opens nothing.
 */
int port_find(const char *root, unsigned int iface, char *path, size_t cap);

/*
 * The voice port's other opener. phoned takes the tty with TIOCEXCL, so that a second opener is refused with EBUSY at
 * once; a holder that did not set the flag is found by its open descriptor and named. Both checks run at start and
 * on hotplug, never per step.
 */

/* Set TIOCEXCL on an open tty: 0, or -errno when the tty will not take it. */
int port_exclusive(int fd);

/* 1 and the holder's pid and command name (its comm, at most 15 characters) when a process other than self_pid has
 * `path` open; 0 when none does. Reads <proc_root>/<pid>/fd links and <proc_root>/<pid>/comm only. -errno when the
 * tree cannot be read, which is not a proof that the port is free. */
int port_holder(const char *proc_root, const char *path, int self_pid, int *pid, char *comm, size_t cap);

#endif /* AW_PHONED_PORT_H */
