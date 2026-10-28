#!/bin/sh
# build-sdk.sh - inside the builder image: the pinned SDK, the exported tree,
# the three packages, the checks. Any doubt stops the build.
#
# Mounts: /repo (read-only: scripts and pins), /src.tar.gz (git archive HEAD),
# /sdk (read-only cache), /out (the artifacts).
set -eu

REPO=${REPO:-/repo}
SRC=${SRC:-/src.tar.gz}
SDK_CACHE=${SDK_CACHE:-/sdk}
OUT=${OUT:-/out}
WORK=${WORK:-/work}
KERNEL=6.12.94
ARCH=aarch64_cortex-a53

say() { printf '== %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

read -r want sdkname < "$REPO/scripts/sdk.sha256"
[ -n "${want:-}" ] && [ -n "${sdkname:-}" ] || die "scripts/sdk.sha256 must hold '<sha256>  <file>'"
luci_commit=$(awk '$1 == "luci" { print $2 }' "$REPO/scripts/feeds.pin")
[ -n "${luci_commit:-}" ] && [ ${#luci_commit} -eq 40 ] || die "scripts/feeds.pin has no 40-character luci commit"

[ -f "$SDK_CACHE/$sdkname" ] || die "$SDK_CACHE/$sdkname is missing (scripts/build.sh fetches it)"
got=$(sha256sum "$SDK_CACHE/$sdkname" | awk '{print $1}')
[ "$got" = "$want" ] || die "the SDK sha256 is $got, expected $want"

say "extracting the SDK"
rm -rf "$WORK/sdk" "$WORK/src"
mkdir -p "$WORK/sdk" "$WORK/src"
tar --zstd -xf "$SDK_CACHE/$sdkname" -C "$WORK/sdk"
SDK=$(find "$WORK/sdk" -mindepth 1 -maxdepth 1 -type d | head -n 1)
[ -n "$SDK" ] && [ -f "$SDK/rules.mk" ] || die "the SDK tarball holds no build tree"

say "unpacking the source"
tar -xzf "$SRC" -C "$WORK/src"
[ -x "$WORK/src/scripts/version.sh" ] || die "the exported tree has no scripts/version.sh"
"$WORK/src/scripts/version.sh" check
ver=$(cat "$WORK/src/VERSION")

cd "$SDK"
cp feeds.conf.default feeds.conf
sed -i "s|^src-git luci .*|src-git luci https://git.openwrt.org/project/luci.git^$luci_commit|" feeds.conf
printf 'src-link awvoice %s\n' "$WORK/src" >> feeds.conf
base_pin=$(awk '/^src-git .* base / { n = split($NF, a, "^"); print a[n] }' feeds.conf)

say "fetching the pinned base and luci feeds"
./scripts/feeds update base luci
[ "$(git -C feeds/luci rev-parse HEAD)" = "$luci_commit" ] || die "feeds/luci is not at the pinned commit $luci_commit"

say "installing the base libraries and the three packages"
./scripts/feeds install libubox ubus uci
./scripts/feeds update awvoice
./scripts/feeds install -a -p awvoice
libubox_d=$(find package/feeds/base -mindepth 1 -maxdepth 3 -type l -name libubox | head -n 1)
ubus_d=$(find package/feeds/base -mindepth 1 -maxdepth 3 -type l -name ubus | head -n 1)
uci_d=$(find package/feeds/base -mindepth 1 -maxdepth 3 -type l -name uci | head -n 1)
[ -n "$libubox_d" ] || die "the base feed did not install libubox"
[ -n "$ubus_d" ] || die "the base feed did not install ubus"
[ -n "$uci_d" ] || die "the base feed did not install uci"

make defconfig

say "building the target libraries"
make -j"$(nproc)" "$libubox_d/compile"
make -j"$(nproc)" "$ubus_d/compile" "$uci_d/compile"

for p in awvoice-phoned kmod-awvoice-fxs luci-app-awvoice; do
	pkgdir=$(find package/feeds/awvoice -maxdepth 2 -type l -name "$p" | head -n 1)
	[ -n "$pkgdir" ] || die "the awvoice feed did not install $p"
	say "building $p"
	make "$pkgdir/clean"
	make -j"$(nproc)" "$pkgdir/compile"
done

say "checking the packages"
# kernel modules land in bin/targets/<target>/kmods, the other two in bin/packages
list_apks() { find bin -type f -name '*.apk' | sort; }
count=$(find bin \( -name 'awvoice-phoned-*.apk' -o -name 'kmod-awvoice-fxs-*.apk' -o -name 'luci-app-awvoice-*.apk' \) -type f | wc -l | tr -d ' ')
[ "$count" = 3 ] || { list_apks >&2; die "bin holds $count of the three apks, expected 3"; }
apk_phoned=$(find bin -type f -name "awvoice-phoned-$ver-r1.apk" | head -n 1)
apk_kmod=$(find bin -type f -name "kmod-awvoice-fxs-$KERNEL.$ver-r1.apk" | head -n 1)
apk_luci=$(find bin -type f -name "luci-app-awvoice-$ver-r1.apk" | head -n 1)
[ -n "$apk_phoned" ] || { list_apks >&2; die "no awvoice-phoned-$ver-r1.apk was built"; }
[ -n "$apk_kmod" ] || { list_apks >&2; die "no kmod-awvoice-fxs-$KERNEL.$ver-r1.apk was built"; }
[ -n "$apk_luci" ] || { list_apks >&2; die "no luci-app-awvoice-$ver-r1.apk was built"; }

ko=$(find build_dir -type f -name 'awvoice_fxs.ko' | head -n 1)
[ -n "$ko" ] || die "the built awvoice_fxs.ko is missing"
magic=$(modinfo -F vermagic "$ko")
case "$magic" in
"$KERNEL "*) ;;
*) die "the module's vermagic is '$magic', expected $KERNEL" ;;
esac
file "$ko" | grep -q 'ARM aarch64' || die "the module is not an aarch64 object"

dbin=$(find build_dir -type f -path '*/.pkgdir/awvoice-phoned/usr/sbin/awvoice-phoned' | head -n 1)
[ -n "$dbin" ] || die "the packaged awvoice-phoned binary is missing"
file "$dbin" | grep -q 'ARM aarch64' || die "the daemon is not an aarch64 object"
needed=$(readelf -d "$dbin" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p')
[ -n "$needed" ] || die "the daemon has no NEEDED entries"
for lib in $needed; do
	case "$lib" in
	libc.so|libc.so.*|libubox.so|libubox.so.*|libubus.so|libubus.so.*|libuci.so|libuci.so.*|libgcc_s.so|libgcc_s.so.*) ;;
	*) die "the daemon needs $lib, outside the allowed set" ;;
	esac
done

say "writing the artifacts"
mkdir -p "$OUT"
rm -f "$OUT"/*.apk "$OUT"/SHA256SUMS "$OUT"/build-info.txt
cp "$apk_phoned" "$apk_kmod" "$apk_luci" "$OUT"/
( cd "$OUT" && sha256sum ./*.apk | sed 's|\./||' > SHA256SUMS )
cat > "$OUT/build-info.txt" <<INFO
version: $ver-r1
commit: ${GIT_COMMIT:-unknown}
sdk: $sdkname
sdk-sha256: $want
feeds: base $base_pin, luci $luci_commit
kernel: $KERNEL
target: qualcommax/ipq807x ($ARCH)
built: $(date -u '+%Y-%m-%dT%H:%M:%SZ')
INFO

say "the three packages are built and checked"
