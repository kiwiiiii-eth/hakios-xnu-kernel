#!/bin/bash
# tools/lower/dk-test.sh: lowered arm64e code through the real dyld and a real shared cache, under QEMU, in a minute.
#
# The system's shared cache is 4.6 GB; the DriverKit runtime's -- its own libSystem, for the same dyld -- is 22 MB. This
# makes a root volume of three files (a test program built for DriverKit as /sbin/launchd, /usr/lib/dyld and the DriverKit
# cache, all three lowered with pac_lower), boots it as the RAM disk of the open kernel, and looks for the program's
# lines: it got to main() through dyld, malloc() worked, libSystem called back into it, and its atexit handler ran.
#
#   tools/lower/dk-test.sh ROOT CRYPTEX     ROOT: the mounted (or unpacked) root volume of the IPSW, for usr/lib/dyld
#                                           CRYPTEX: the same of its OS cryptex, for System/DriverKit/System/Library/dyld
#
# Needs: the open kernel as ./hakios-kernel.open (make defconfig; tools/make-kernel.sh; cp hakios-kernel hakios-kernel.open),
# the loader with the identity of the IPSW's iPhone (xnuloader/mkidentity.sh DeviceTree.im4p; xnuloader/build.sh: dyld's
# "ignition" asks the kernel for hw.target), Xcode with the DriverKit SDK, qemu-system-aarch64 and what qemu/run.sh needs.
set -e
cd "$(dirname "$0")/../.."
ROOT=$1 CRYPTEX=$2
[ -f "$ROOT/usr/lib/dyld" ] && [ -f "$CRYPTEX/System/DriverKit/System/Library/dyld/dyld_shared_cache_arm64e" ] ||
	{ sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1; }
[ -f hakios-kernel.open ] || { echo "dk-test.sh: no ./hakios-kernel.open (see this script's head)" >&2; exit 1; }
W=${DK_WORK:-logs/dk-test}
rm -rf "$W" && mkdir -p "$W/tree/sbin" "$W/tree/usr/lib" "$W/tree/System/DriverKit/System/Library/dyld" "$W/tree/dev" "$W/tree/private/var" "$W/tree/tmp"
cc -O2 -o "$W/pac_lower" tools/lower/pac_lower.c
"$W/pac_lower" --selftest
DK=$(xcrun --sdk driverkit --show-sdk-path)
# (-client_name: libSystem is the DriverKit framework's to link; -nostdlib, -lSystem: and not the framework itself)
clang -target arm64e-apple-driverkit25.4 -isysroot "$DK" -O1 -fno-inline -fno-builtin -nostdlib -Wl,-client_name,clang \
	-L"$DK/System/DriverKit/usr/lib" -lSystem -o "$W/tree/sbin/launchd" userland/src/dk_hello.c
cp "$ROOT/usr/lib/dyld" "$W/tree/usr/lib/dyld"
cp "$CRYPTEX/System/DriverKit/System/Library/dyld/dyld_shared_cache_arm64e" "$W/tree/System/DriverKit/System/Library/dyld/"
chmod -R u+w "$W/tree"
chmod 755 "$W/tree/sbin/launchd" "$W/tree/usr/lib/dyld"
"$W/pac_lower" "$W/tree/sbin/launchd" "$W/tree/usr/lib/dyld"
"$W/pac_lower" --cache "$W/tree/System/DriverKit/System/Library/dyld/dyld_shared_cache_arm64e"
python3 tools/apfs/mkapfs.py -o "$W/mini.apfs" System="$W/tree"
xnuloader/mkpayload.py -o qemu/payload.bin kernel=hakios-kernel.open ramdisk="$W/mini.apfs" \
	--cmdline "cpus=1 exec_driverkit_plain=1 ${DK_ARGS:-}" > /dev/null
qemu/run.sh "${DK_SECS:-90}" > "$W/qemu.log" 2>&1 || true
LC_ALL=C sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g' "$W/qemu.log" | LC_ALL=C tr -d '\r' | sed -n '/KERNEL LOG/,$p' > "$W/kernel.txt"
grep '^dk_hello: ' "$W/kernel.txt" || true
if [ "$(grep -c '^dk_hello: ' "$W/kernel.txt")" = 4 ] && grep -q '^dk_hello: atexit handler ran' "$W/kernel.txt"; then
	echo "dk-test: passed (the log: $W/kernel.txt)"
else
	echo "dk-test: FAILED: not the program's four lines (the log: $W/kernel.txt)"
	grep -m1 '^panic(' "$W/kernel.txt" | cut -c1-300
	exit 1
fi
