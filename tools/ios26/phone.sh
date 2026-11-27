#!/bin/bash
# One run of this tree's kernel on the 6T, in memory only: nothing is written to the phone's storage
# (ufs_write=0 var_store=0 var_hfs=0 are put last on the line, where they win).
#
#   tools/ios26/phone.sh NAME [RAMDISK] [extra boot-args]
#     SECS=25        loader_return_after
#     CHECKPOINT=N   stop at loader_checkpoint(N) and come back (osfmk/arm64/arm_init.c)
#     SYS=FILE       a system image staged into the phone's memory first (oem ramdisk)
#     KERNEL=FILE    default ./hakios-kernel
#
# The flow is devices/fajita/scripts/run-darwin-live.sh's (ABL -> fastboot boot of U-Boot + the loader with its own
# USB -> stage -> oem run), with this tree as the kernel repository and ../root26/out for what it makes, so that
# nothing of the 16.5 line's is replaced. The loader image: HAKIOS_ROOT=../root26 devices/fajita/scripts/mk-usbimg.sh
# Logs: logs/phone/NAME.txt
set -e
K=$(cd "$(dirname "$0")/../.." && pwd); ROOT=$(cd "$K/../root26" && pwd)
NAME=${1:?usage: phone.sh NAME [RAMDISK] [extra boot-args]}; RD=${2:-$K/prebuilt/userland/hello}; EXTRA=${3:-}
KERNEL=${KERNEL:-$K/hakios-kernel}; SECS=${SECS:-25}; USBIMG=$ROOT/out/hakios-fajita-usb.img
LOG=$K/logs/phone/$NAME.txt; mkdir -p "$K/logs/phone" "$K/build"
export PATH=/opt/homebrew/opt/coreutils/libexec/gnubin:$PATH
. "$K/devices/fajita/scripts/lib-args.sh"; resolve_args "$K" "$KERNEL" || exit 1
CMDLINE="loader_return_after=$SECS epoch=$(date +%s) -v $ARGS $SOCARGS $EXTRA ufs_write=0 var_store=0 var_hfs=0"
echo "phone: kernel=$(basename "$KERNEL") ramdisk=$(basename "$RD") checkpoint=${CHECKPOINT:-none}"
echo "phone: cmdline=$CMDLINE"

state() { fastboot devices 2>/dev/null | awk '$1=="xnuloader"{print "loader"; exit} $1 ~ /^\?+$/ {print "uboot"; exit} $2=="fastboot" {print "abl"; exit}'; }
wait_for() { local want=$1 n=${2:-120}; for i in $(seq 1 $n); do [ "$(state)" = "$want" ] && return 0; sleep 1; done; return 1; }

case "$(state)" in
loader) [ -n "$FRESH" ] || { timeout 10 fastboot reboot >/dev/null 2>&1 || true; sleep 3; } ;;
uboot)
	timeout 8 fastboot oem run:"pmic dev pmic@0" >/dev/null 2>&1 && timeout 8 fastboot oem run:"pmic write 0x88f 0x04" >/dev/null 2>&1 &&
		{ timeout 8 fastboot oem run:reset >/dev/null 2>&1 || true; } ||
		{ pkill -f "fastboot oem run" 2>/dev/null || true; echo "phone: U-Boot does not answer"; } ;;
abl) ;;
*) echo "phone: not seen" ;;
esac
if [ -z "$FRESH" ] || [ "$(state)" != loader ]; then
	wait_for abl 120 || { echo "phone: no ABL fastboot"; exit 1; }
	fastboot boot "$USBIMG" >/dev/null 2>&1
	wait_for loader 240 || { echo "phone: the loader's USB did not appear"; exit 1; }
fi
timeout 10 fastboot getvar version 2>&1 | grep -q "version:" || sleep 5

if [ -n "${SYS:-}" ]; then
	T0=$(date +%s)
	want=$("$K/devices/fajita/scripts/image-fnv.sh" "$SYS" 2>/dev/null || true)
	timeout 10 fastboot oem ramdisk 2>&1 | grep -q OKAY || { echo "phone: the loader refuses the system image"; exit 1; }
	timeout 3600 fastboot stage "$SYS" 2>&1 | grep -q OKAY || { echo "phone: the system image did not go over"; exit 1; }
	have=$(timeout 300 fastboot oem ramdisk fnv 2>&1 | sed -n 's/^(bootloader) fnv \([0-9a-f]*\).*/\1/p')
	echo "phone: system image $(( $(stat -f %z "$SYS") / 1048576 )) MB in $(( $(date +%s) - T0 )) s, FNV $have (the file's: $want)"
	[ -z "$want" ] || [ "$have" = "$want" ] || { echo "phone: the system image arrived damaged"; exit 1; }
fi

cd "$K"
python3 xnuloader/mkpayload.py -o build/phone-run.bin kernel="$KERNEL" ramdisk="$RD" --cmdline "$CMDLINE" >/dev/null
timeout 60 fastboot stage build/phone-run.bin 2>&1 | grep -q OKAY || { echo "phone: the payload did not go over"; exit 1; }
timeout 10 fastboot oem run ${CHECKPOINT:-} >/dev/null 2>&1 || true
DEADLINE=$(( $(date +%s) + SECS + 120 )); result=
while [ $(date +%s) -lt $DEADLINE ]; do
	result=$(timeout 15 fastboot getvar result 2>&1 | grep '^result:' || true)
	case "$result" in ""|*running*) ;; *) break ;; esac
	if timeout 15 fastboot oem log >/dev/null 2>&1 && timeout 60 fastboot get_staged "$LOG.live.tmp" >/dev/null 2>&1; then
		mv -f "$LOG.live.tmp" "$LOG.live"
	fi
	sleep 2
done
case "$result" in ""|*running*) echo "phone: no result (the kernel may be stuck)"; exit 2 ;; esac
timeout 120 fastboot get_staged "$LOG" >/dev/null 2>&1 || true
echo "phone: ${result}; $LOG ($(wc -c < "$LOG" 2>/dev/null) bytes)"
