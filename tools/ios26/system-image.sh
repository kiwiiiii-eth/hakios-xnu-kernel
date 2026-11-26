#!/bin/bash
# tools/ios26/system-image.sh: the system volume of a Darwin 25 (userland 26) system for a processor without pointer
# authentication, as one APFS image: the IPSW's root volume as it is, its programs lowered (tools/lower), the OS cryptex
# (and the App cryptex) where the root volume's links expect them, the dyld shared cache lowered.
#
#   tools/ios26/system-image.sh WORK ROOT CRYPTEX [APPCRYPTEX]
#       WORK        a directory for what this makes: sys-over (the lowered programs and what else is laid over the
#                   root volume), dyld-lowered (the shared cache, lowered in place: a copy of the cryptex's), system.apfs
#       ROOT        the IPSW's root volume, mounted (or unpacked);  CRYPTEX, APPCRYPTEX: the same of its cryptexes
#   env: FRESH=1    make sys-over again (after tools/lower changed); otherwise only what is missing
#
# The image is for qemu/run-ios26.sh (a disk of the host). Nothing of Apple's is in the repository: all of it comes
# from your own IPSW (sysimage/aea-key.swift for its encrypted disk images).
set -e
cd "$(dirname "$0")/../.."
WORK=$1 ROOT=$2 CRYPTEX=$3 APP=${4:-}
CACHE=System/Library/Caches/com.apple.dyld
[ -d "$WORK" ] && [ -f "$ROOT/sbin/launchd" ] && [ -f "$CRYPTEX/$CACHE/dyld_shared_cache_arm64e" ] ||
	{ sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1; }
WORK=$(cd "$WORK" && pwd)
mkdir -p logs && cc -O2 -o logs/pac_lower tools/lower/pac_lower.c
OVER=$WORK/sys-over OS=private/preboot/Cryptexes/OS
[ -z "${FRESH:-}" ] || rm -rf "$OVER"
if [ ! -f "$WORK/sys-over.done" ]; then
	rm -rf "$OVER"; mkdir -p "$OVER"
	python3 tools/ios26/lower-tree.py logs/pac_lower "$ROOT" "$OVER"
	# the OS cryptex but for the caches (the system's below; DriverKit's is not used), then its programs
	mkdir -p "$OVER/$OS"
	rsync -a --exclude "/$CACHE" --exclude /System/DriverKit "$CRYPTEX/" "$OVER/$OS/"
	chmod -R u+w "$OVER/$OS"
	python3 tools/ios26/lower-tree.py --skip "$CACHE" --skip System/DriverKit --at "$OS" logs/pac_lower "$CRYPTEX" "$OVER"
	if [ -n "$APP" ]; then
		mkdir -p "$OVER/private/preboot/Cryptexes/App"
		rsync -a "$APP/" "$OVER/private/preboot/Cryptexes/App/"
		chmod -R u+w "$OVER/private/preboot/Cryptexes/App"
		python3 tools/ios26/lower-tree.py --at private/preboot/Cryptexes/App logs/pac_lower "$APP" "$OVER"
	fi
	# dyld looks for the cache in /System/Library/Caches/com.apple.dyld when the boot is one from a RAM disk (rd=md0)
	mkdir -p "$OVER/System/Library/Caches" "$OVER/private/preboot/boot"
	ln -s ../../Cryptexes/OS/$CACHE "$OVER/System/Library/Caches/com.apple.dyld"
	touch "$WORK/sys-over.done"
fi
# the shared cache: copied once, lowered in place (its .symbols file, 1 GB, is never mapped and stays out of the image)
if [ ! -f "$WORK/dyld-lowered/.done" ]; then
	rm -rf "$WORK/dyld-lowered"; mkdir -p "$WORK/dyld-lowered"
	cp "$CRYPTEX/$CACHE"/dyld_shared_cache_arm64e* "$WORK/dyld-lowered/"
	chmod u+w "$WORK/dyld-lowered"/*
	touch "$WORK/dyld-lowered/.done"
fi
logs/pac_lower --cache "$WORK/dyld-lowered/dyld_shared_cache_arm64e"
rm -rf "$OVER/$OS/$CACHE"; mkdir -p "$OVER/$OS/$CACHE"
for f in "$WORK/dyld-lowered"/dyld_shared_cache_arm64e*; do
	case $f in *.symbols) ;; *) ln "$f" "$OVER/$OS/$CACHE/" ;; esac
done
python3 tools/apfs/mkapfs.py -z --plain "$OS/$CACHE/" -o "$WORK/system.apfs" "System=$ROOT,over=$OVER"
ls -l "$WORK/system.apfs"
