// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
#include <assert.h>
#include <stdio.h>

#include "../src/core/module_abi.h"

int main(void)
{
	/* the module this build ships with */
	assert(module_abi_check(2, 2) == MODULE_ABI_OK);

	/* an interface 1 module (1.1.0's image module, or any 1.0.0 one) is older: loading the installed module is the fix */
	assert(module_abi_check(1, 2) == MODULE_ABI_OLDER);

	/* no number at all is never a match either */
	assert(module_abi_check(0, 2) == MODULE_ABI_OLDER);

	/* a module ahead of this build is refused too, and no reload of the module helps */
	assert(module_abi_check(3, 2) == MODULE_ABI_NEWER);

	/* one number apart in either direction is a mismatch, never "close enough" */
	assert(module_abi_check(1, 1) == MODULE_ABI_OK);
	assert(module_abi_check(2, 1) == MODULE_ABI_NEWER);

	printf("module_abi: all tests passed\n");
	return 0;
}
