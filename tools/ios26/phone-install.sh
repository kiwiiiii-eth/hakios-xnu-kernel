#!/bin/bash
# tools/ios26/phone-install.sh SYSTEM.apfs: a system image too large for the phone's memory (tools/ios26/system-image.sh:
# 12.9 GB) into the 6T's userdata partition, from its first block, where the kernel mounts it as it lies
# (init26_dev=ufs_userdata: the block device). THIS ERASES ANDROID'S DATA; darwin, and the system in it, are not touched.
#
#   USERDATA_WRITE=yes   write. Without it: the plan, and the loader's read-only answers (userdata found, the FNV of
#                        its first megabyte, the write refused without its word) -- nothing is written.
#   PIECE_MB=1024        the size of a piece      FIRST=N   begin at piece N (0 is the first): after a run cut short
#
# The loader's side: xnuloader/loader.c, "A system too large for memory, in userdata". A piece is staged in the phone's
# memory, its FNV-1a compared with the file's (a download is not checked on its way), then written, every 8 MB read
# back; at the end the whole image is read from the flash and its FNV-1a compared with the file's.
# Logs: logs/phone/install-DATE.log
set -e
K=$(cd "$(dirname "$0")/../.." && pwd); ROOT=$(cd "$K/../root26" && pwd)
IMG=${1:?usage: phone-install.sh SYSTEM.apfs}; [ -f "$IMG" ] || { echo "install: no such file: $IMG"; exit 1; }
PIECE_MB=${PIECE_MB:-1024}; FIRST=${FIRST:-0}; USBIMG=$ROOT/out/hakios-fajita-usb.img; TMP=$ROOT/out/piece.bin
export PATH=/opt/homebrew/opt/coreutils/libexec/gnubin:$PATH
mkdir -p "$K/logs/phone"; LOG=$K/logs/phone/install-$(date +%m%d-%H%M%S).log
say() { echo "install: $*" | tee -a "$LOG"; }
FNV=${TMPDIR:-/tmp}/hakios-fnv64-$(id -u); SRC=$K/devices/fajita/scripts/fnv64.c
[ -x "$FNV" ] && [ "$FNV" -nt "$SRC" ] || cc -O2 -o "$FNV" "$SRC"
SIZE=$(/usr/bin/stat -f %z "$IMG"); PIECES=$(( (SIZE + (PIECE_MB << 20) - 1) / (PIECE_MB << 20) ))
say "$(basename "$IMG"): $SIZE bytes, $PIECES pieces of $PIECE_MB MB, into userdata from its first block"

state() { fastboot devices 2>/dev/null | awk '$1=="xnuloader"{print "loader"; exit} $1 ~ /^\?+$/ {print "uboot"; exit} $2=="fastboot" {print "abl"; exit}'; }
wait_for() { local want=$1 n=${2:-120}; for i in $(seq 1 $n); do [ "$(state)" = "$want" ] && return 0; sleep 1; done; return 1; }
oem() { local t=$1; shift; timeout "$t" fastboot oem "$@" 2>&1 | tee -a "$LOG"; }

case "$(state)" in
loader) timeout 10 fastboot reboot >/dev/null 2>&1 || true; sleep 3 ;;
abl) ;;
*) say "the phone is not in fastboot"; exit 1 ;;
esac
wait_for abl 120 || { say "no ABL fastboot"; exit 1; }
fastboot boot "$USBIMG" >/dev/null 2>&1
wait_for loader 240 || { say "the loader's USB did not appear"; exit 1; }
timeout 10 fastboot getvar version 2>&1 | grep -q "version:" || sleep 5

if [ "${USERDATA_WRITE:-}" != yes ]; then
	say "USERDATA_WRITE is not yes: nothing is written. The loader's answers:"
	oem 60 userfnv 1048576 | grep -E "fnv|FAIL|OKAY" || true
	oem 20 userput 0 not-the-word | grep -E "FAIL|OKAY" || true
	timeout 10 fastboot reboot >/dev/null 2>&1 || true
	exit 0
fi

WANT=$("$FNV" "$IMG"); say "the image's FNV-1a: $WANT"
T0=$(date +%s)
for i in $(seq "$FIRST" $((PIECES - 1))); do
	/bin/dd if="$IMG" of="$TMP" bs=1048576 skip=$((i * PIECE_MB)) count="$PIECE_MB" 2>/dev/null
	want=$("$FNV" "$TMP")
	for try in 1 2 3; do
		oem 10 ramdisk | grep -q OKAY || { say "piece $i: the loader refuses a download"; exit 1; }
		timeout 1800 fastboot stage "$TMP" 2>&1 | grep -q OKAY || { say "piece $i: the download failed"; exit 1; }
		have=$(oem 300 ramdisk fnv | sed -n 's/^(bootloader) fnv \([0-9a-f]*\).*/\1/p')
		[ "$have" = "$want" ] && break
		say "piece $i arrived damaged ($have, not $want): again ($try)"
		[ "$try" -lt 3 ] || { say "piece $i: damaged three times: nothing of it was written"; exit 1; }
	done
	oem 1800 userput $((i * PIECE_MB)) erase-android-data | grep -q OKAY || { say "piece $i: NOT WRITTEN (fastboot oem log); FIRST=$i goes on from here"; exit 1; }
	say "piece $i of $PIECES written ($(( $(date +%s) - T0 )) s)"
done
rm -f "$TMP"
have=$(oem 1800 userfnv "$SIZE" | sed -n 's/^(bootloader) fnv \([0-9a-f]*\).*/\1/p')
if [ "$have" = "$WANT" ]; then
	say "the image in userdata reads back whole: FNV $have, $(( $(date +%s) - T0 )) s"
else
	say "THE IMAGE IN USERDATA IS NOT THE FILE: it reads $have, the file is $WANT"
	exit 1
fi
timeout 10 fastboot reboot >/dev/null 2>&1 || true
