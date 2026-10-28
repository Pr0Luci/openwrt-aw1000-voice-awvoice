/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Pr0Luci */
#ifndef AW_MODULE_ABI_H
#define AW_MODULE_ABI_H

/*
 * The kernel module's interface number (AWFXS_ABI, read through AWFXS_IOC_INFO) against the one this build was compiled
 * with. Only an exact match is driven: the daemon sends the module's commands by number, an older module answers an
 * unknown one with ENOTTY long after the start (an interface 1 module has no capture tap and no playback ring, so the
 * first recorded call and the first answering-machine call fail), and a newer module may lay out a structure this
 * build fills. Both directions are refused.
 *
 * The verdict keeps the two apart because their remedies differ: an older module is replaced by loading the installed
 * one (/lib/modules/<release>/awvoice_fxs.ko), a newer module means this daemon is the old half and reloading the
 * module cannot help. Plain comparison on purpose: it is tested on the host (tests/test_module_abi.c), and it needs
 * neither the kernel's UAPI header nor libc.
 */
enum module_abi_verdict {
	MODULE_ABI_OK = 0,	/* the module is this build's companion */
	MODULE_ABI_OLDER = 1,	/* the module is behind this build: load the installed module instead */
	MODULE_ABI_NEWER = 2,	/* the module is ahead of this build: this daemon (its package) is out of date */
};

static inline enum module_abi_verdict module_abi_check(unsigned int module_abi, unsigned int expected)
{
	if (module_abi == expected)
		return MODULE_ABI_OK;
	return module_abi < expected ? MODULE_ABI_OLDER : MODULE_ABI_NEWER;
}

#endif
