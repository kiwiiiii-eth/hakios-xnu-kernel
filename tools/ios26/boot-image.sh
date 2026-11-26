#!/bin/bash
# tools/ios26/boot-image.sh [ROOT]: the small image a Darwin 25 system starts from under QEMU (userland/boot26.apfs, the
# payload's RAM disk, md0): userland/src/init26.c as its /sbin/launchd and the directories it mounts on; and, with the
# IPSW's root volume mounted (or unpacked) at ROOT, the launchd jobs the system is started with instead of all of its
# own (userland/mkjobs.py: those named here, and the rest started only when asked for).
#   env: JOBS="label ..."   the jobs started at boot (default: the ones below)    ALL_JOBS=1  no choice: launchd's own lists
#        OSLOG="label ..."  jobs whose os_log goes to the console too ("all": every one)   ALLOW="word ..." see mkjobs.py
# A cryptex's programs are looked for in ROOT/../cryptex and ROOT/../cryptex-app (mkjobs.py): links to where they are mounted.
set -e
ROOT=${1:+$(cd "$1" && pwd)}
cd "$(dirname "$0")/../../userland"
export PATH="$PWD/../xnu-deps/host/bin:$PATH"
CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
mkdir -p obj
$CC -Ilibmini -c libmini/mini.c -o obj/mini.o
$CC -c libmini/start.S -o obj/start.o
$CC -Ilibmini $LD -o obj/init26 src/init26.c obj/mini.o obj/start.o
rm -rf boot26 && mkdir -p boot26/sbin boot26/dev boot26/flash
cp obj/init26 boot26/sbin/launchd
JOBS=${JOBS-"
	com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.logd
	com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd
	com.apple.containermanagerd com.apple.containermanagerd.system com.apple.runningboardd
	com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc com.apple.lsd
	com.apple.powerd com.apple.analyticsd com.apple.logd_helper
	com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
	com.apple.SpringBoard
"}
if [ -n "$ROOT" ] && [ -z "${ALL_JOBS:-}" ]; then
	mkdir -p boot26/LaunchDaemons boot26/empty boot26/xpc
	OSLOG=${OSLOG-com.apple.backboardd} python3 mkjobs.py --rest "$ROOT" boot26/LaunchDaemons $JOBS > boot26.jobs
	tail -1 boot26.jobs; grep NOT boot26.jobs || true
	for f in "$ROOT"/System/Library/xpc/*; do
		case $f in */launchd.plist|*/launchd.plist.sig) ;; *) cp "$f" boot26/xpc/ ;; esac
	done
fi
python3 ../tools/apfs/mkapfs.py -o boot26.apfs Boot=boot26 > /dev/null
ls -l boot26.apfs
