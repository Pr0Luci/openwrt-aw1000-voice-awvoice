#!/bin/sh
# version.sh - VERSION is the one version source; the three packages follow it.
#
#   scripts/version.sh check     fail, with the file and line, on any mismatch
#   scripts/version.sh set X.Y.Z rewrite VERSION and the three Makefiles
set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
MAKEFILES="awvoice-phoned/Makefile kmod-awvoice-fxs/Makefile luci-app-awvoice/Makefile"

say() { printf '%s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[ -f "$TOP/VERSION" ] || die "$TOP/VERSION is missing"
VER=$(cat "$TOP/VERSION")
case "$VER" in
[0-9]*.[0-9]*.[0-9]*) ;;
*) die "VERSION holds '$VER', expected x.y.z" ;;
esac

check() {
	bad=0
	for f in $MAKEFILES; do
		vline=$(grep -n '^PKG_VERSION:=' "$TOP/$f" || true)
		rline=$(grep -n '^PKG_RELEASE:=' "$TOP/$f" || true)
		if [ -z "$vline" ]; then
			say "ERROR: $f has no PKG_VERSION line" >&2
			bad=1
		else
			vnum=${vline%%:*}
			vval=${vline#*:PKG_VERSION:=}
			[ "$vval" = "$VER" ] || { say "ERROR: $f:$vnum: PKG_VERSION is '$vval', VERSION is '$VER'" >&2; bad=1; }
		fi
		if [ -z "$rline" ]; then
			say "ERROR: $f has no PKG_RELEASE line" >&2
			bad=1
		else
			rnum=${rline%%:*}
			rval=${rline#*:PKG_RELEASE:=}
			[ "$rval" = "1" ] || { say "ERROR: $f:$rnum: PKG_RELEASE is '$rval', expected 1" >&2; bad=1; }
		fi
	done
	[ "$bad" = 0 ] || exit 1
	say "version: the three packages are $VER-r1"
}

case "${1:-}" in
check)
	check
	;;
set)
	[ $# -eq 2 ] || die "usage: scripts/version.sh set <x.y.z>"
	new=$2
	case "$new" in
	[0-9]*.[0-9]*.[0-9]*) ;;
	*) die "'$new' is not x.y.z" ;;
	esac
	for f in $MAKEFILES; do
		part="$TOP/$f.part"
		sed -e "s/^PKG_VERSION:=.*/PKG_VERSION:=$new/" -e 's/^PKG_RELEASE:=.*/PKG_RELEASE:=1/' "$TOP/$f" > "$part"
		mv "$part" "$TOP/$f"
	done
	printf '%s\n' "$new" > "$TOP/VERSION"
	check
	;;
*)
	die "usage: scripts/version.sh check|set <x.y.z>"
	;;
esac
