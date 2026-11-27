#!/bin/bash
# tools/ios26/boot-image.sh [ROOT]: the small image a Darwin 25 system starts from under QEMU (userland/boot26.apfs, the
# payload's RAM disk, md0): userland/src/init26.c as its /sbin/launchd and the directories it mounts on; and, with the
# IPSW's root volume mounted (or unpacked) at ROOT, the launchd jobs the system is started with instead of all of its
# own (userland/mkjobs.py: those named here, and the rest started only when asked for).
#   env: JOBS="label ..."   the jobs started at boot (default: the ones below)    ALL_JOBS=1  no choice: launchd's own lists
#        MORE_JOBS="label ..."  more of them, beside the default ones
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
	# Mesa, when it has been built for these programs (MESA_TARGET=arm64e-apple-ios16.5 MESA_PREFIX=/private/preboot/boot
	# tools/mk-mesa.sh): the OpenGL ES library that draws with the phone's GPU, and gl_test, a triangle by it
	# (userland/src/gl_test.c). RUN="PROGRAM ARGUMENT..." is a job that runs one of this image's programs as the system starts.
	M=../xnu-deps/build/mesa/out-arm64e-apple-ios16.5
	if [ -f $M/libOSMesa.8.dylib ]; then
		/usr/bin/xcrun strip -x -o boot26/lib/libOSMesa.8.dylib $M/libOSMesa.8.dylib
		$E -framework CoreFoundation -framework IOKit $M/libOSMesa.8.dylib -o boot26/bin/gl_test src/gl_test.c
		# ... and the OpenGL ES that CoreAnimation asks an EAGLContext for, by that library (src/eagl.c): the window
		# server's, when the kernel has the GPU (src/inject.c, opengl_es_25()); GL=0 leaves it out of the image
		if [ "${GL:-1}" != 0 ]; then
			python3 mkeagl.py src/eagl_slots.txt $M/gl.xml $M/exported-gl.txt src/eagl.c > obj/eagl_gen26.h
			mkdir -p obj/gen26 && cp obj/eagl_gen26.h obj/gen26/eagl_gen.h
			$E -dynamiclib -install_name /private/preboot/boot/lib/libhakios.gl.dylib -Iobj/gen26 -framework CoreFoundation \
				-framework IOKit -framework IOSurface -lobjc $M/libOSMesa.8.dylib -o boot26/lib/libhakios.gl.dylib \
				src/eagl.c src/gl_osmesa.c
			/usr/bin/codesign -f -s - boot26/lib/libhakios.gl.dylib
			obj/pac_lower -q boot26/lib/libhakios.gl.dylib
		fi
		for f in boot26/lib/libOSMesa.8.dylib boot26/bin/gl_test; do
			/usr/bin/codesign -f -s - "$f"
			obj/pac_lower -q "$f"
		done
	fi
else
	echo "boot-image.sh: no SDK at $SDK: no library for the window server (no touch screen)" >&2
fi
# The GPU's firmware, the phone's own (the other line's ../fw-gpu, pulled from its Android; FW_GPU=DIR, FW_GPU= for none):
# the kernel starts the Adreno with it (boot-arg gpu=1, drivers/gpu/hakios.a630.c: fw/NAME of the image that came with it).
FW_GPU=${FW_GPU-$(cd ../.. && pwd)/hakios-xnu-kernel/fw-gpu}
if [ -n "$FW_GPU" ] && [ -d "$FW_GPU" ]; then
	mkdir -p boot26/fw && cp "$FW_GPU"/a630_* boot26/fw/
fi
# The rest of the phone's own firmware, as the other line has it beside its tree (never in a repository): the modem's and
# the DSPs' (fw-modem: modem.mdt and its pieces, mba.mbn, image/ -- the phone's /vendor/firmware_mnt/image, which the modem
# reads over TFTP and the audio DSP is started from -- and ath10k/ for the Wi-Fi radio), Bluetooth's patch and settings
# (fw-bt), and the cameras' tables made of the vendor's module files (fw-camera). 170 MB: more than the loader takes with
# a kernel (64 MB), so an image with these goes over as the resident one (SYS=userland/boot26.apfs tools/ios26/phone.sh).
# The drivers read some from this image's memory and the others from /fw of the root, which init26 makes of this fw/.
# FW_PHONE=DIR: where fw-modem and the others are; FW_PHONE= for none (QEMU).
FW_PHONE=${FW_PHONE-$(cd ../.. && pwd)/hakios-xnu-kernel}
rm -rf boot26/fw/image boot26/fw/ath10k; rm -f boot26/fw/modem.* boot26/fw/mba.mbn boot26/fw/cr* boot26/fw/camera-*
if [ -n "$FW_PHONE" ] && [ -f "$FW_PHONE/fw-modem/modem.mdt" ]; then
	mkdir -p boot26/fw
	cp "$FW_PHONE"/fw-modem/modem.mdt "$FW_PHONE"/fw-modem/modem.b[0-9][0-9] boot26/fw/
	[ -f "$FW_PHONE/fw-modem/mba.mbn" ] && cp "$FW_PHONE/fw-modem/mba.mbn" boot26/fw/
	[ -d "$FW_PHONE/fw-modem/image" ] && cp -R "$FW_PHONE/fw-modem/image" boot26/fw/image
	[ -d "$FW_PHONE/fw-modem/ath10k" ] && cp -R "$FW_PHONE/fw-modem/ath10k" boot26/fw/ath10k
	for f in "$FW_PHONE"/fw-bt/crbtfw*.tlv "$FW_PHONE"/fw-bt/crnv*.bin "$FW_PHONE"/fw-haptic/*.bin "$FW_PHONE"/fw-audio/aw882xx_acf.bin; do
		[ -f "$f" ] && cp "$f" boot26/fw/
	done
	for module in "$FW_PHONE"/fw-camera/com.qti.sensormodule.*.bin; do
		[ -f "$module" ] || continue
		sensor=$(basename "$module" .bin | sed 's/^com\.qti\.sensormodule\.[a-z]*_\([a-z0-9]*\).*/\1/')
		../tools/camsensor.py "$module" "boot26/fw/camera-$sensor.tab" >/dev/null
	done
	for tuned in "$FW_PHONE"/fw-camera/com.qti.tuned.*.bin; do
		[ -f "$tuned" ] || continue
		case "$(basename "$tuned")" in com.qti.tuned.*_fat*.bin|com.qti.tuned.default.bin) continue ;; esac
		sensor=$(basename "$tuned" .bin | sed 's/^com\.qti\.tuned\.[a-z]*_\([a-z0-9]*\).*/\1/')
		../tools/camtuning.py "$tuned" "boot26/fw/camera-$sensor.isp" >/dev/null
	done
fi
# what init26 puts into the empty /private/var (var_files() there): userland/var's settings, and the note that this
# build's data has been migrated, under the build of the system that is started (ROOT's; 16.5's file names its own)
mkdir -p boot26/var && cp -R var/ boot26/var/
# ... and two settings of Accessibility's that 16.5 has no need of: less transparency and less motion. Darwin 25 draws
# its glass -- blurs and refraction behind every bar and sheet -- and the window server here draws with the processor.
A=boot26/var/mobile/Library/Preferences/com.apple.Accessibility.plist
/usr/libexec/PlistBuddy -c 'Add :ReduceMotionEnabled bool true' -c 'Add :EnhancedBackgroundContrastEnabled bool true' \
	-c 'Add :ReduceMotionAutoplayMessagesEffectsEnabled bool false' "$A" >/dev/null
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
# MORE_JOBS="label ...": jobs started at boot beside those (and let in whatever mkjobs.py's DENY says of them)
JOBS="$JOBS ${MORE_JOBS:-}"
# (com.apple.dmd, which 16.5's list keeps out for crashing there: Darwin 25's runs, and without it ScreenTimeAgent,
# appprotectiond and IntelligencePlatformComputeService each ask launchd for it without a pause -- 4943 failed lookups in
# a minute, three processors busy with nothing and launchd half of a fourth)
export ALLOW="${ALLOW-com.apple.dmd}"
if [ -n "$ROOT" ] && [ -z "${ALL_JOBS:-}" ]; then
	mkdir -p boot26/LaunchDaemons boot26/empty boot26/xpc
	OSLOG=${OSLOG-com.apple.backboardd} python3 mkjobs.py --rest "$ROOT" boot26/LaunchDaemons $JOBS > boot26.jobs
	tail -1 boot26.jobs; grep NOT boot26.jobs || true
	for f in "$ROOT"/System/Library/xpc/*; do
		case $f in */launchd.plist|*/launchd.plist.sig) ;; *) cp "$f" boot26/xpc/ ;; esac
	done
	if [ -n "${RUN:-}" ]; then
		python3 - $RUN <<'PY'
import plistlib, sys
job = {'Label': 'com.hakios.run', 'ProgramArguments': sys.argv[1:], 'RunAtLoad': True,
       'StandardErrorPath': '/dev/console', 'StandardOutPath': '/dev/console'}
plistlib.dump(job, open('boot26/LaunchDaemons/com.hakios.run.plist', 'wb'), fmt=plistlib.FMT_BINARY)
PY
		echo "  a job that runs: $RUN"
	fi
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
