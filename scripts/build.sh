#!/bin/sh
# build.sh - the host entry of a release build.
#
#   scripts/build.sh
#
# Refuses a dirty tree, fetches and verifies the pinned SDK, builds the three
# packages in the builder image from `git archive HEAD` (so a local-only file
# can never enter a package), and leaves the apks, their checksums and
# build-info.txt in dist/. Docker must be running.
set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
CACHE=${AWVOICE_SDK_CACHE:-$HOME/.cache/awvoice-sdk}
IMAGE=awvoice-builder
SDK_BASE=https://downloads.openwrt.org/releases/25.12.5/targets/qualcommax/ipq807x

say() { printf '== %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

sha256_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | awk '{print $1}'
	else
		shasum -a 256 "$1" | awk '{print $1}'
	fi
}

cd "$TOP"

[ -z "$(git status --porcelain)" ] || die "the tree is dirty: a release build uses git archive HEAD, so commit or stash first"
[ "$(git show HEAD:VERSION)" = "$(cat VERSION)" ] || die "VERSION differs from HEAD"
"$TOP/scripts/version.sh" check

read -r want sdkname < "$TOP/scripts/sdk.sha256"
[ -n "${want:-}" ] && [ -n "${sdkname:-}" ] || die "scripts/sdk.sha256 must hold '<sha256>  <file>'"

command -v docker >/dev/null 2>&1 || die "docker is not installed"
docker info >/dev/null 2>&1 || die "the docker daemon is not running (start Docker or colima)"

mkdir -p "$CACHE"
if [ ! -f "$CACHE/$sdkname" ]; then
	say "downloading $sdkname"
	part="$CACHE/$sdkname.part"
	curl -fL --retry 3 --retry-delay 2 -o "$part" "$SDK_BASE/$sdkname" || { rm -f "$part"; die "download failed: $SDK_BASE/$sdkname"; }
	mv "$part" "$CACHE/$sdkname"
fi
got=$(sha256_of "$CACHE/$sdkname")
[ "$got" = "$want" ] || die "the SDK sha256 is $got, expected $want ($CACHE/$sdkname)"

say "building the builder image"
docker build -q -t "$IMAGE" "$TOP/docker" >/dev/null

tmp=$(mktemp -d "${TMPDIR:-/tmp}/awvoice-build.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
mkdir -p "$tmp/out"
git archive --format=tar.gz -o "$tmp/src.tar.gz" HEAD

say "compiling $(cat VERSION) from $(git rev-parse --short HEAD)"
docker run --rm \
	--user "$(id -u):$(id -g)" \
	-e GIT_COMMIT="$(git rev-parse HEAD)" \
	-v "$TOP:/repo:ro" \
	-v "$CACHE:/sdk:ro" \
	-v "$tmp/src.tar.gz:/src.tar.gz:ro" \
	-v "$tmp/out:/out" \
	"$IMAGE" /repo/scripts/build-sdk.sh

say "collecting dist/"
rm -rf "$TOP/dist"
mkdir -p "$TOP/dist"
cp "$tmp/out"/* "$TOP/dist"/

[ -f "$TOP/dist/SHA256SUMS" ] || die "the build wrote no SHA256SUMS"
bad=0
while read -r sum name; do
	[ -n "${name:-}" ] || continue
	got=$(sha256_of "$TOP/dist/$name")
	[ "$got" = "$sum" ] || { printf 'ERROR: %s: %s, expected %s\n' "$name" "$got" "$sum" >&2; bad=1; }
done < "$TOP/dist/SHA256SUMS"
[ "$bad" = 0 ] || die "the artifact checksums do not match"
n=$(find "$TOP/dist" -maxdepth 1 -type f | wc -l | tr -d ' ')
[ "$n" = 5 ] || die "dist/ holds $n files, expected 5 (three apks, SHA256SUMS, build-info.txt)"

say "the artifacts are in dist/"
for f in "$TOP/dist"/*.apk; do printf '   %s\n' "$(basename "$f")"; done
cat "$TOP/dist/build-info.txt"
