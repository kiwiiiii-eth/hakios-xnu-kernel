#!/bin/bash
# Everything the kernel build needs that this repository does not carry: open sources of
# Apple's and others' that are fetched and built here, and the SDK the kernel is compiled
# against. What is already there is left alone, so this can be run again at any time.
#
#   xnu-deps/setup.sh             both stages (the second only once the kernel tree exists)
#   xnu-deps/setup.sh --stage1    sources, host tools, the SDK
#   xnu-deps/setup.sh --stage2    the two static libraries that are compiled against the
#                                 kernel's own exported headers (needs xnu/apply.sh first)
#   xnu-deps/setup.sh --rebuild-libs   stage 2 again from scratch (after the kernel headers changed)
#
# Needs on the machine: git curl tar xz make cc clang flex bison m4 perl python3 autoconf
# automake, and theos ($THEOS, default ~/theos) with its Linux toolchain
# (toolchain/linux/iphone: the cctools linker) and sdks/iPhoneOS26.5.sdk.
#
# What ends up where (all under xnu-deps/, none of it committed):
#   AvailabilityVersions libdispatch libpthread     Apple open source, at the releases of Darwin userland 26.5
#   mm_bootstrap_cmds tcsh unifdef                  sources of mig, csh and unifdef
#   MacOSX26.1.sdk                                  only Kernel.framework: the DriverKit headers
#                                                   that Apple's closed "iig" would generate
#   host/bin                                        mig, migcom, unifdef, tcsh, csh, unzbm, and links
#                                                   to the cctools (next to our own wrappers)
#   sdk/iPhoneOS26.5.xnu.sdk                        the theos SDK (as links) plus usr/local: what
#                                                   Apple's internal SDK has and the public one lacks
set -e -o pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../xnu-env.sh"
cd "$XNUDEPS"
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}
CCTOOLS=$THEOS/toolchain/linux/iphone/bin
DARWIN_SDK=$THEOS/sdks/iPhoneOS26.5.sdk
MACOS_SDK_URL=https://github.com/joseluisq/macosx-sdks/releases/download/26.1/MacOSX26.1.sdk.tar.xz

log() { echo "setup: $*"; }
die() { echo "setup: $*" >&2; exit 1; }

# fetch DIR URL COMMIT: one commit of a repository, without its history
fetch() {
	# there already? (its own repository, not the one this directory is part of)
	[ -d "$1" ] && git -C "$1" rev-parse -q --verify HEAD >/dev/null 2>&1 &&
		[ "$(git -C "$1" rev-parse --show-toplevel)" = "$(cd "$1" && pwd -P)" ] && return 0
	log "fetching $1"
	rm -rf "$1"
	git init -q "$1"
	git -C "$1" remote add origin "$2"
	git -C "$1" fetch -q --depth 1 origin "$3" || die "cannot fetch $3 from $2"
	git -C "$1" -c advice.detachedHead=false checkout -q FETCH_HEAD
}

# link TARGET NAME: a symbolic link, made only when it is not there already
link() {
	[ "$(readlink "$2" 2>/dev/null)" = "$1" ] || ln -sfn "$1" "$2"
}

# replace FILE with what is on standard input, unless it is the same already (make looks at time stamps)
update() {
	cat > "$1.new"
	if cmp -s "$1.new" "$1"; then rm "$1.new"; else mv "$1.new" "$1"; fi
}

# macOS: no theos, but Xcode has the same cctools and the SDK can be had from github.com/theos/sdks.
# theos-mac/ is laid out like theos so that the rest of this script does not care.
theos_mac() {
	local tool
	mkdir -p "$THEOS/toolchain/linux/iphone/bin" "$THEOS/sdks"
	for tool in ld nm otool strip lipo libtool nmedit ctf_insert codesign_allocate install_name_tool; do
		link "$(/usr/bin/xcrun -f $tool)" "$THEOS/toolchain/linux/iphone/bin/$tool"
	done
	# (github.com/theos/sdks stops at iPhoneOS 16.5: for the Darwin 25 kernel the SDK is Xcode's own, whatever its number)
	local xcode_sdk
	xcode_sdk=$(/usr/bin/xcrun --sdk iphoneos --show-sdk-path) && [ -d "$xcode_sdk/usr/include" ] || die "Xcode has no iPhoneOS SDK"
	link "$xcode_sdk" "$THEOS/sdks/iPhoneOS26.5.sdk"
}

# linktree FROM TO: TO becomes a tree of links to the files of FROM (cp -rs, which BSD cp lacks)
linktree() {
	if [ "$XNU_HOST_OS" = Darwin ]; then
		python3 - "$1" "$2" <<-'PY'
		import os, sys
		src, dst = sys.argv[1], sys.argv[2]
		for root, dirs, files in os.walk(src):
		    rel = os.path.relpath(root, src)
		    out = dst if rel == '.' else os.path.join(dst, rel)
		    os.makedirs(out, exist_ok=True)
		    for name in files:
		        os.symlink(os.path.join(root, name), os.path.join(out, name))
		PY
	else
		cp -rs "$1" "$2"
	fi
}

stage1() {
	local missing= tool
	local tools="git curl tar xz make cc clang clang++ flex bison m4 perl python3 autoreconf automake"
	[ "$XNU_HOST_OS" = Darwin ] && { theos_mac; tools="git curl tar make cc clang clang++ flex bison m4 perl python3"; }
	for tool in $tools; do
		command -v $tool >/dev/null || missing="$missing $tool"
	done
	[ -z "$missing" ] || die "not installed:$missing"
	[ -x "$CCTOOLS/ld" ] || die "no cctools in $CCTOOLS: install theos with its Linux toolchain (or set THEOS)"
	[ -d "$DARWIN_SDK/usr/include" ] || die "no $DARWIN_SDK (the SDKs of github.com/theos/sdks go to \$THEOS/sdks)"

	# --- sources -------------------------------------------------------------------------------
	fetch AvailabilityVersions https://github.com/apple-oss-distributions/AvailabilityVersions.git \
		refs/tags/AvailabilityVersions-157.2
	fetch libdispatch https://github.com/apple-oss-distributions/libdispatch.git \
		refs/tags/libdispatch-1542.100.32
	fetch libpthread https://github.com/apple-oss-distributions/libpthread.git \
		refs/tags/libpthread-539.100.4
	fetch mm_bootstrap_cmds https://github.com/markmentovai/bootstrap_cmds \
		f337e600741638896fb6056ce87c6479f74b89a3        # cross_platform/bootstrap_cmds-138
	fetch tcsh https://github.com/tcsh-org/tcsh.git b7db07931546178c4cb2542b5fc850c483628ed5
	fetch unifdef https://github.com/fanf2/unifdef.git 08d12cd3059fe072929ea001ad9833f5487fd355

	# Apple's iig (the DriverKit interface generator) is closed. host/bin/iig hands out the headers it
	# would generate, which the macOS SDK of the same XNU release ships ready-made.
	if [ ! -d MacOSX26.1.sdk/System/Library/Frameworks/Kernel.framework/Headers/DriverKit ]; then
		log "downloading the macOS 26.1 SDK for its DriverKit headers"
		curl -L --fail --retry 3 -s -o macos-sdk.tar.xz "$MACOS_SDK_URL" || die "download failed: $MACOS_SDK_URL"
		local wild=--wildcards; [ "$XNU_HOST_OS" = Darwin ] && wild=        # bsdtar: patterns as they are
		tar -xJf macos-sdk.tar.xz $wild '*MacOSX26.1.sdk/System/Library/Frameworks/Kernel.framework*' ||
			die "cannot unpack the macOS SDK"
		rm -f macos-sdk.tar.xz
	fi

	# IOUserServer.iig of xnu-12377.121.6 has Panic(), which the 26.1 SDK's generated header has not yet; Xcode's own
	# macOS SDK has (host/bin/iig takes what is in iig-hdr/ first). On Linux: copy that one header from a Mac.
	mkdir -p iig-hdr
	if [ ! -f iig-hdr/IOUserServer.h ] && [ "$XNU_HOST_OS" = Darwin ]; then
		local xcode_dk
		xcode_dk=$(/usr/bin/xcrun --sdk macosx --show-sdk-path)/System/Library/Frameworks/Kernel.framework/Headers/DriverKit
		grep -q 'Panic_Impl' "$xcode_dk/IOUserServer.h" 2>/dev/null && cp "$xcode_dk/IOUserServer.h" iig-hdr/IOUserServer.h ||
			log "Xcode's macOS SDK has no IOUserServer.h with Panic(): iokit/Kernel/IOUserServer.cpp will not compile"
	fi

	# --- host tools ----------------------------------------------------------------------------
	mkdir -p host/bin
	if [ "$XNU_HOST_OS" = Darwin ]; then
		# macOS has it all: mig (Xcode's; it takes $MIGCC and $MIGCOM from the kernel build), unifdef, csh
		link "$(/usr/bin/xcrun -f mig)" host/bin/mig
		# with the command-line tools alone, migcom is in usr/libexec, where xcrun does not look (and xcrun must not find
		# host/bin's own link on the PATH)
		migcom=$(xcode-select -p)/usr/libexec/migcom
		[ -x "$migcom" ] || migcom=$(PATH=/usr/bin:/bin /usr/bin/xcrun -f migcom)
		link "$migcom" host/bin/migcom
		link /usr/bin/unifdef host/bin/unifdef
		link /bin/tcsh host/bin/tcsh
		# what XNU's makefiles call by Linux's names: GNU's nproc (MakeInc.cmd's job count) and python
		printf '#!/bin/sh\n# nproc as GNU coreutils has it, on macOS\nexec sysctl -n hw.ncpu\n' | update host/bin/nproc
		chmod +x host/bin/nproc
		# (a script, not a link: /usr/bin/python3 is a shim that looks for the tool its own name says, and "python" there is none)
		rm -f host/bin/python
		printf '#!/bin/sh\nexec python3 "$@"\n' | update host/bin/python
		chmod +x host/bin/python
	fi
	# mig: Apple's own does not build on glibc; this is a port that does
	if [ ! -x host/bin/migcom ] || [ ! -x host/bin/mig ]; then
		log "building mig"
		(cd mm_bootstrap_cmds && autoreconf --install && sh configure --prefix="$XNUDEPS/host" &&
			make -j$JOBS && make install) > mig-build.log 2>&1 || die "mig did not build, see xnu-deps/mig-build.log"
	fi
	if [ ! -x host/bin/unifdef ]; then
		log "building unifdef"
		# its makefile takes the version from the git history; any text will do
		[ -f unifdef/version.h ] || printf '"@(#) $Version: unifdef-2.12 $\\n"\n"@(#) $Date: 2020-11-06 11:22:51 +0000 $\\n"\n' > unifdef/version.h
		# gnu11: "constexpr", a variable name in unifdef.c, is a keyword in C23
		cc -std=gnu11 -O2 -w -o host/bin/unifdef unifdef/unifdef.c
	fi
	# csh: XNU's SETUP/config/doconf is a csh script
	if [ ! -x host/bin/tcsh ]; then
		log "building tcsh"
		(cd tcsh && ./configure --prefix="$XNUDEPS/host" && make -j$JOBS) > tcsh-build.log 2>&1 ||
			die "tcsh did not build, see xnu-deps/tcsh-build.log"
		cp tcsh/tcsh host/bin/tcsh
	fi
	link tcsh host/bin/csh
	# what 7z leaves undone when it extracts files of the Darwin images (see build/lzbitmap/unzbm.c)
	if [ ! -x host/bin/unzbm ]; then
		cc -O2 -w -o host/bin/unzbm build/lzbitmap/unzbm.c build/lzbitmap/libzbitmap.c
	fi
	# the Mach-O linker and its companions come with theos
	for tool in nm otool strip lipo libtool nmedit ctf_insert codesign_allocate install_name_tool; do
		link "$CCTOOLS/$tool" host/bin/$tool
	done
	if [ "$XNU_HOST_OS" = Darwin ]; then
		# (not a link: Xcode's ld looks itself up by the name it was called by, and knows no "ld64")
		printf '#!/bin/sh\nexec "%s" "$@"\n' "$(/usr/bin/xcrun -f ld)" > host/bin/ld64.new && chmod +x host/bin/ld64.new && mv host/bin/ld64.new host/bin/ld64
	else
		link "$CCTOOLS/ld" host/bin/ld64
	fi
	link xnu-clang host/bin/xnu-clang++

	# --- the SDK -------------------------------------------------------------------------------
	# The makefiles ask xcrun (ours: host/bin/xcrun) for an SDK and a platform directory.
	mkdir -p sdk/Platforms/iPhoneOS.platform
	if [ ! -d "$XNU_SDK_PATH/usr/include" ]; then
		log "making the SDK: links to $DARWIN_SDK"
		rm -rf "$XNU_SDK_PATH"
		linktree "$(cd "$DARWIN_SDK" && pwd -P)" "$XNU_SDK_PATH"
	fi
	local sdk_local=$XNU_SDK_PATH/usr/local
	mkdir -p $sdk_local/include/kernel/os $sdk_local/include/os $sdk_local/lib/kernel $sdk_local/libexec
	update $sdk_local/libexec/availability.pl < AvailabilityVersions/availability.pl
	[ -x $sdk_local/libexec/availability.pl ] || chmod +x $sdk_local/libexec/availability.pl
	# (since AvailabilityVersions-140 or so availability.pl only runs the program beside it)
	if [ -f AvailabilityVersions/availability ]; then
		update $sdk_local/libexec/availability < AvailabilityVersions/availability
		[ -x $sdk_local/libexec/availability ] || chmod +x $sdk_local/libexec/availability
		[ -e $sdk_local/libexec/availability.dsl ] || cp AvailabilityVersions/availability.dsl $sdk_local/libexec/availability.dsl
		[ -e $sdk_local/libexec/templates ] || cp -R AvailabilityVersions/templates $sdk_local/libexec/templates
	fi
	# the kernel's half of a private libdispatch header (unifdef exits with 1 when it removed something)
	{ host/bin/unifdef -DOS_FIREHOSE_SPI=1 -DKERNEL=1 libdispatch/os/firehose_buffer_private.h || [ $? -eq 1 ]; } |
		update $sdk_local/include/kernel/os/firehose_buffer_private.h
	update $sdk_local/include/kernel/os/firehose_types.defs < libdispatch/src/firehose/firehose_types.defs
	log "stage 1 done"
}

# The firehose (os_log) buffer code of libdispatch and the pthread kernel extension are open
# source but not part of XNU: Apple links the first from its internal SDK and loads the second
# as a kext. Here both become static libraries in the SDK (makedefs/MakeInc.def links them into
# the kernel). They are compiled against the headers the kernel build exports, so that step
# of the kernel build comes first.
stage2() {
	[ -f "$XNU_SRC/makedefs/MakeInc.def" ] || die "no kernel tree in $XNU_SRC (xnu/apply.sh makes it)"
	[ -x host/bin/migcom ] && [ -d "$XNU_SDK_PATH/usr/local/lib/kernel" ] || die "stage 1 has not been done"
	local lib=$XNU_SDK_PATH/usr/local/lib/kernel hdrs=$XNU_SRC/BUILD/obj/EXPORT_HDRS file
	# three Mach-O headers for the build tools the kernel build compiles for this machine (SETUP/)
	mkdir -p hostinc/mach-o
	for file in loader.h nlist.h fat.h; do
		[ -f hostinc/mach-o/$file ] || cp "$XNU_SRC/EXTERNAL_HEADERS/mach-o/$file" hostinc/mach-o/
	done
	if [ -n "$REBUILD_LIBS" ]; then rm -f $lib/libfirehose_kernel.a $lib/libpthread_kext.a; fi
	if [ -f $lib/libfirehose_kernel.a ] && [ -f $lib/libpthread_kext.a ]; then
		log "stage 2: nothing to do"
		return 0
	fi
	log "exporting the kernel's headers (xnu-build.sh exporthdrs)"
	"$DARWIN_REDWOOD/xnu-build.sh" -j$JOBS exporthdrs > exporthdrs.log 2>&1 && [ -d "$hdrs/osfmk" ] ||
		die "the kernel headers were not exported, see xnu-deps/exporthdrs.log"
	if [ ! -f $lib/libfirehose_kernel.a ]; then
		log "building libfirehose_kernel.a"
		mkdir -p build/firehose
		xnu-clang -arch arm64 -c -Os -mkernel -nostdinc -fno-builtin \
			-DKERNEL=1 -DKERNEL_PRIVATE=1 -DPRIVATE=1 -D__DARWIN_64_BIT_INO_T=1 \
			-DDISPATCH_USE_DTRACE=0 -DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1 \
			-DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0 -DOS_FIREHOSE_SPI=1 -DARM64_BOARD_CONFIG_BCM2837 \
			-Ibuild/pthread/inc -Ilibdispatch -Ilibdispatch/src -Ilibdispatch/src/firehose \
			-I$hdrs/osfmk -I$hdrs/bsd -I$hdrs/libkern -I$hdrs/pexpert -I$hdrs/iokit -I$hdrs/security \
			-I$XNU_SRC/libkern/firehose -I$hdrs/libkern/os -I$XNU_SRC/EXTERNAL_HEADERS \
			-I$XNU_SDK_PATH/usr/local/include/kernel \
			-o build/firehose/firehose_buffer.o libdispatch/src/firehose/firehose_buffer.c
		libtool -static -o $lib/libfirehose_kernel.a build/firehose/firehose_buffer.o
	fi
	if [ ! -f $lib/libpthread_kext.a ]; then
		log "building libpthread_kext.a"
		build/pthread/build.sh
	fi
	log "stage 2 done; now: ./xnu-build.sh -j$JOBS -k"
}

case "$1" in
--stage1) stage1 ;;
--stage2) stage2 ;;
--rebuild-libs) REBUILD_LIBS=1 stage2 ;;
"")
	stage1
	if [ -f "$XNU_SRC/makedefs/MakeInc.def" ]; then
		stage2
	else
		log "no kernel tree yet: run xnu/apply.sh, then this script again"
	fi ;;
*) sed -n '2,12p' "$HERE/setup.sh"; exit 1 ;;
esac
