#!/bin/bash
# Build ath10k -- Qualcomm's driver for the WCN3990, the SDM845's Wi-Fi (OnePlus 6T), whose firmware runs on
# the modem -- for our kernel, as tools/mk-ath11k.sh builds ath11k for the reference phone: Linux's driver
# unmodified (ath10k is ISC; every file is checked to say so), FreeBSD's LinuxKPI headers (patched with
# wifi/lx/patches), and wifi/lx underneath. W4b's preparation (6T Wi-Fi plan): the driver's files for the
# SNOC bus compiled, and what they want of the layer listed (out/undefined.txt, out/stubs.txt): nothing of it
# runs yet. Its own snoc.c is the bus here (no stand-in as lx_bus.c is for ath11k's ahb.c); wifi/lx/src's
# lx_base.c is shared, its ath11k-only files are not built.
#
# What comes out, in xnu-deps/build/ath10k:  src/ the driver's files, untouched; out/ath10k.o; out/stubs.txt
# usage: tools/mk-ath10k.sh
set -e
VERSION=7.0.14
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
B=$ROOT/xnu-deps/build/ath10k
TAR=$ROOT/xnu-deps/build/ath11k/linux-$VERSION.tar.xz
mkdir -p "$B"
cd "$B"
if [ "$(cat src/.release 2>/dev/null)" != "$VERSION" ]; then
	[ -f "$TAR" ] || "$ROOT/tools/mk-ath11k.sh" fetch
	rm -rf src && mkdir src
	tar xJf "$TAR" -C src --strip-components=6 linux-$VERSION/drivers/net/wireless/ath/ath10k
	# the headers of ath/ that ath10k reaches as "../ath.h" (ISC): beside src/
	tar xJf "$TAR" -C . --strip-components=5 linux-$VERSION/drivers/net/wireless/ath/ath.h \
		linux-$VERSION/drivers/net/wireless/ath/regd.h linux-$VERSION/drivers/net/wireless/ath/spectral_common.h \
		linux-$VERSION/drivers/net/wireless/ath/dfs_pattern_detector.h linux-$VERSION/drivers/net/wireless/ath/trace.h \
		linux-$VERSION/drivers/net/wireless/ath/key.c linux-$VERSION/drivers/net/wireless/ath/main.c \
		linux-$VERSION/drivers/net/wireless/ath/regd.c linux-$VERSION/drivers/net/wireless/ath/regd_common.h
	echo "$VERSION" > src/.release
fi
for f in src/*.[ch]; do
	case "$(head -6 "$f")" in
	*"SPDX-License-Identifier: ISC"*) ;;
	*"Permission to use, copy, modify, and/or distribute this software"*) ;;
	*) echo "mk-ath10k.sh: $f does not carry the licence expected" >&2; exit 1 ;;
	esac
done
# FreeBSD's sources: the archive mk-ath11k.sh keeps, if it is there
FBT=$ROOT/xnu-deps/build/ath11k/freebsd-15.1-src.txz
[ -f "$FBT" ] && [ ! -e freebsd-15.1-src.txz ] && ln -s "$FBT" freebsd-15.1-src.txz

W=$ROOT/wifi/lx

# --- FreeBSD's LinuxKPI and the kernel headers under it, with our patches on top. When the patches
# change, the tree is taken from the archive again (as tools/mk-mesa.sh does with Mesa's).
FREEBSD=15.1
FREEBSD_SHA256=cf5762da53fd52e1eaf0f9ceee9bf58cbe314c821031d0d9ffa76823185a89e1
FSYS="compat/linuxkpi sys arm64/include net net80211 netinet netinet6 vm kern tools libkern ddb
	security fs/pseudofs crypto opencrypto geom bsm isa cam rpc xdr x86/include ofed/include
	dev/pci dev/ofw dev/fdt dev/acpica dev/usb dev/iicbus dev/backlight
	contrib/ck/include contrib/dev/acpica/include contrib/libnv"
PATCHES=$(cat "$W"/patches/*.patch 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ "$(cat freebsd/.release 2>/dev/null)" != "$FREEBSD $FREEBSD_SHA256 $PATCHES" ]; then
	[ -f freebsd-$FREEBSD-src.txz ] || curl -sL --fail -o freebsd-$FREEBSD-src.txz \
		https://download.freebsd.org/releases/amd64/$FREEBSD-RELEASE/src.txz
	echo "$FREEBSD_SHA256  freebsd-$FREEBSD-src.txz" | sha256sum -c --quiet
	rm -rf freebsd gen obj
	mkdir freebsd
	tar xJf freebsd-$FREEBSD-src.txz -C freebsd --strip-components=2 usr/src/COPYRIGHT \
		$(for d in $FSYS; do echo usr/src/sys/$d; done)
	for p in "$W"/patches/*.patch; do
		[ -f "$p" ] && patch -s --no-backup-if-mismatch -d freebsd -p1 < "$p"
	done
	echo "$FREEBSD $FREEBSD_SHA256 $PATCHES" > freebsd/.release
fi
S=$B/freebsd/sys

# --- what FreeBSD's build would have generated: the interfaces of its device framework, and the
# option headers, which here say only that there is an Internet
if [ ! -d gen ]; then
	mkdir gen
	(
		cd gen
		ln -s $S/arm64/include machine
		for m in kern/device_if.m kern/bus_if.m dev/pci/pci_if.m dev/pci/pci_iov_if.m dev/pci/pcib_if.m \
			dev/iicbus/iicbus_if.m dev/iicbus/iicbb_if.m dev/backlight/backlight_if.m dev/usb/usb_if.m \
			dev/ofw/ofw_bus_if.m; do
			awk -f $S/tools/makeobjops.awk $S/$m -h
		done
		for o in h p q; do awk -f $S/tools/vnode_if.awk $S/kern/vnode_if.src -$o; done
		for o in $(grep -rhoE "opt_[a-z0-9_]+\.h" $S | sort -u) lkpi_iic_if.h; do : > $o; done
		printf '#define INET 1\n#define INET6 1\n' > opt_inet.h
		printf '#define INET6 1\n' > opt_inet6.h
		printf '#define SMP 1\n' > opt_global.h
	)
fi

# --- compile: the driver's files that are built so far, and ours
DRIVER="core mac debug ce htc htt htt_rx htt_tx txrx wmi wmi-tlv bmi hw p2p swap snoc qmi qmi_wlfw_v01"
CC="clang -target arm64-apple-ios16.5"
LD=$ROOT/xnu-deps/host/bin/ld64
CFLAGS="-I$W/include10 -mkernel -mgeneral-regs-only -fno-builtin -fno-common -ffreestanding -fno-strict-aliasing
	-funsigned-char -nostdinc -std=gnu17 -O2 -g -w
	-D__FreeBSD__=15 -D_KERNEL -DKLD_MODULE -DHAVE_KERNEL_OPTION_HEADERS -include opt_global.h
	-I$W/include -Igen -I$S -I$S/contrib/ck/include
	-I$S/compat/linuxkpi/common/include -I$S/compat/linuxkpi/dummy/include
	-include $S/compat/linuxkpi/common/include/linux/kconfig.h -DLINUXKPI_VERSION=70000"
DFLAGS="-Isrc -Isrc/ath -I$W/include10 -include $W/include/lx_prelude.h -include $W/include10/lx10_prelude.h -DCONFIG_ATH10K_SNOC -DCONFIG_ATH10K_CE -DCONFIG_ATH10K_DEBUG
	-DKBUILD_MODNAME=\"ath10k\""
# two headers are the kernel's: what it offers this object (wifi/lx/include/lx_os.h), and its QMI
# helpers (wifi/lx/include/linux/soc/qcom/qmi.h)
source "$ROOT/xnu-env.sh"
for h in lx qmi; do
	[ -f "$XNU_SRC/bsd/sys/hakios.$h.h" ] || { echo "mk-ath10k.sh: the kernel tree has no bsd/sys/hakios.$h.h" >&2; exit 1; }
done
DFLAGS="$DFLAGS -DLX_HAKIOS_LX_H=\"$XNU_SRC/bsd/sys/hakios.lx.h\" -DLX_HAKIOS_QMI_H=\"$XNU_SRC/bsd/sys/hakios.qmi.h\""
mkdir -p obj out
rm -f obj/*.err
compile() {		# compile SOURCE OBJECT [FLAGS]: only when the object is older than anything it was made of
	local src=$1 obj=$2
	shift 2
	if [ -f "$obj" ] && [ -f "$obj.d" ] && [ -z "$(find $(sed -e 's/^[^:]*://' -e 's/\\$//' "$obj.d") "$0" -newer "$obj" 2>/dev/null | head -1)" ]; then
		return
	fi
	$CC $CFLAGS "$@" -MMD -MF "$obj.d" -c "$src" -o "$obj" 2> "$obj.err" || rm -f "$obj"
}
for f in $DRIVER; do compile src/$f.c obj/ath10k_$f.o $DFLAGS & done
for f in "$W"/src/lx_base.c "$W"/src10/*.c; do [ -f "$f" ] && compile "$f" obj/lx_$(basename "$f" .c).o $DFLAGS -I$W/src & done
wait
bad=
for f in obj/*.err; do
	[ -s "$f" ] && [ ! -f "${f%.err}" ] && { echo "--- $(basename "$f" .o.err)"; grep -m6 -A3 "error:" "$f"; bad=1; }
done
[ -z "$bad" ] || exit 1
OBJECTS=$(for f in $DRIVER; do echo obj/ath10k_$f.o; done; echo obj/lx_lx_base.o; for f in "$W"/src10/*.c; do echo obj/lx_$(basename "$f" .c).o; done)

# --- tie it all into one object. What it then still wants is one of three things: a call of the
# kernel's (hakios.lx.h; the QMI helpers; the handful of C library functions XNU has under the same
# names with the same meaning), a function of a part of the driver that is not built yet, or a
# mistake. The second kind gets a stub each that says its name and answers 0; anything else
# missing ends the build.
KERNEL="memcmp memcpy memmove memset strcmp strncmp strlen strchr strrchr strlcpy strlcat bzero
	snprintf vsnprintf __stack_chk_fail __stack_chk_guard"
$LD -r -arch arm64 $OBJECTS -o out/all.o 2> out/ld.err || { cat out/ld.err; exit 1; }
llvm-nm -u --format=just-symbols out/all.o | sed 's/^_//' | sort > out/undefined.txt
: > out/stubs.txt
: > out/missing.txt
: > out/stubs.s
missing=
for s in $(cat out/undefined.txt); do
	case " $(echo $KERNEL) " in *" $s "*) continue ;; esac
	case $s in
	lx_os_*|qmi_*|hakios_qmi_*|section\$*) continue ;;
	ath10k_*|__ath10k_*) ;;
	ath_*|__ath_*) ;;
	# W4c's: the 802.11 side (mac80211 as LinuxKPI names it, NAPI, the network device), stubs until it is written
	linuxkpi_ieee80211_*|linuxkpi_cfg80211_*|linuxkpi_wiphy_*|linuxkpi_80211_*|linuxkpi_set_ieee80211_dev|\
	lkpi_get_linuxkpi_ieee80211_regdomain|linuxkpi_napi_*|linuxkpi_netif_napi_*|linuxkpi_alloc_netdev|linuxkpi_free_netdev|\
	netif_threaded_enable|linuxkpi_skb_copy|linuxkpi_device_get_mac_address) ;;
	*) echo $s >> out/missing.txt; continue ;;
	esac
	echo $s >> out/stubs.txt
	printf '\t.globl _%s\n_%s:\n\tadrp x0, 1f@PAGE\n\tadd x0, x0, 1f@PAGEOFF\n\tb _lx_stub\n\t.cstring\n1:\t.asciz "%s"\n\t.text\n\t.p2align 2\n' \
		$s $s $s >> out/stubs.s
done
echo "mk-ath10k.sh: $(wc -l < out/missing.txt) names the layer must give (out/missing.txt): W4b's work"
[ -s out/missing.txt ] && exit 2
$CC -c out/stubs.s -o out/stubs.o
# four names are shown: where the kernel enters (lx_prepare(), lx_start(), lx_stop() and lx_transmit() of hakios.lx.h)
printf '_lx_prepare\n_lx_start\n_lx_stop\n_lx_transmit\n' > out/exported.txt
$LD -r -arch arm64 -exported_symbols_list out/exported.txt out/all.o out/stubs.o -o out/ath10k.o 2> out/ld.err ||
	{ cat out/ld.err; exit 1; }
rm -f out/all.o
echo "mk-ath10k.sh: out/ath10k.o: $(llvm-size out/ath10k.o | awk 'NR == 2 { print $1 + $2 }') bytes of code and data from" \
	"$(echo $DRIVER | wc -w) of the driver's files; $(wc -l < out/stubs.txt) functions of the others are stubs (out/stubs.txt);" \
	"asks the kernel for $(llvm-nm -u --format=just-symbols out/ath10k.o | grep -vc 'section\$') names"
