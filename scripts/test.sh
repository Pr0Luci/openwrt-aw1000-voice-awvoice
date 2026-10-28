#!/bin/sh
# test.sh - the three suites, natively.
#
#   scripts/test.sh
#
# The sanitizer suites stay out of the build image: the pinned Debian release
# carries gcc-12, whose AddressSanitizer aborts on a kernel with 32-bit mmap
# randomisation. This runs on the machine, or on the CI runner.
set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)

say() { printf '== %s\n' "$*"; }

for p in awvoice-phoned kmod-awvoice-fxs; do
	say "$p host tests"
	make -C "$TOP/$p" -f Makefile.host test
done

say "luci-app-awvoice js test"
node "$TOP/luci-app-awvoice/tests/live.test.js"

say "test: all three suites passed"
