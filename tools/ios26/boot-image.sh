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
# The library that goes into the window server (userland/src/inject.c, of which Darwin 25 takes the touch screen and
# the buttons: -DHAKIOS_DARWIN25), and rwtouch, which plays touches into the same queue: built arm64e as the system's
# own programs are, against the 16.5 SDK the other line builds with (IOKit's headers are in it; SDK=...), signed ad hoc
# and lowered like them (tools/lower/pac_lower). The root is this image at /private/preboot/boot once init26 has changed
# roots: that is where the kernel is told to find the library (etc/inject.conf -> kern.exec_inject).
#   TOUCH="STEP..."   a job that runs `rwtouch STEP...` (userland/src/rwtouch.c): TOUCH="wait 150 swipe 0.5 0.99 0.5 0.4 250"
SDK=${SDK:-$(cd ../.. && pwd)/hakios-xnu-kernel/xnu-deps/theos-mac/sdks-git/iPhoneOS16.5.sdk}
if [ -d "$SDK" ]; then
	E="/usr/bin/xcrun clang -target arm64e-apple-ios16.5 -isysroot $SDK -O1 -DHAKIOS_DARWIN25 -Wno-unused-function"
	mkdir -p boot26/lib boot26/bin boot26/etc
	$E -dynamiclib -install_name /private/preboot/boot/lib/libhakios.dylib -framework CoreFoundation -framework IOKit -lobjc \
		-o boot26/lib/libhakios.dylib src/inject.c
	# ... and the one that goes into SpringBoard (src/inject_sb26.c: no setup assistant)
	$E -dynamiclib -install_name /private/preboot/boot/lib/libhakios.sb.dylib -lobjc -o boot26/lib/libhakios.sb.dylib src/inject_sb26.c
	$E -o boot26/bin/rwtouch src/rwtouch.c
	cc -O2 -o obj/pac_lower ../tools/lower/pac_lower.c
	for f in boot26/lib/libhakios.dylib boot26/lib/libhakios.sb.dylib boot26/bin/rwtouch; do
		/usr/bin/codesign -f -s - "$f"
		obj/pac_lower -q "$f"
	done
	echo "/private/preboot/boot/lib/libhakios.dylib backboardd;/private/preboot/boot/lib/libhakios.sb.dylib SpringBoard" > boot26/etc/inject.conf
else
	echo "boot-image.sh: no SDK at $SDK: no library for the window server (no touch screen)" >&2
fi
# what init26 puts into the empty /private/var (var_files() there): userland/var's settings, and the note that this
# build's data has been migrated, under the build of the system that is started (ROOT's; 16.5's file names its own)
mkdir -p boot26/var && cp -R var/ boot26/var/
if [ -n "$ROOT" ]; then
	BUILD=$(/usr/libexec/PlistBuddy -c 'Print :ProductBuildVersion' "$ROOT/System/Library/CoreServices/SystemVersion.plist")
	M=boot26/var/mobile/Library/Preferences/com.apple.migration.plist
	/usr/libexec/PlistBuddy -c "Set :LastSystemVersion $BUILD" -c "Set :DMLastMigrationResults:buildVersion $BUILD" "$M" >/dev/null
	echo "  /private/var: migrated as of $BUILD"
fi
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
	if [ -n "${TOUCH:-}" ] && [ -f boot26/bin/rwtouch ]; then
		python3 - $TOUCH <<'PY'
import plistlib, sys
job = {'Label': 'com.hakios.rwtouch', 'ProgramArguments': ['/private/preboot/boot/bin/rwtouch'] + sys.argv[1:],
       'RunAtLoad': True, 'StandardErrorPath': '/dev/console', 'StandardOutPath': '/dev/console'}
plistlib.dump(job, open('boot26/LaunchDaemons/com.hakios.rwtouch.plist', 'wb'), fmt=plistlib.FMT_BINARY)
PY
		echo "  a job that plays touches: rwtouch $TOUCH"
	fi
fi
python3 ../tools/apfs/mkapfs.py -o boot26.apfs Boot=boot26 > /dev/null
ls -l boot26.apfs
