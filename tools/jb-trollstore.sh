#!/bin/bash
# TrollStore in the jailbreak prefix: put what the phone needs to install TrollStore (and, with it, any
# IPA the person has) into jb/root/var/jb, as tools/mk-jb.sh builds it. Run by mk-jb.sh (TROLLSTORE=1, the
# default), and alone on a prefix that is already built, without fetching the whole bootstrap again:
#
#   tools/jb-trollstore.sh [JB-DIR]        JB-DIR defaults to jb/ of the repository (holds root/var/jb and debs/)
#
# What it leaves in the prefix (all of it replaced anew at each run, so it can be run again):
#   usr/share/hakios/trollstore/TrollStore.tar   TrollStore 2.1.1's own release archive (TrollStore.app/...), its
#                                                three Mach-Os re-signed ad hoc (see below); the argument of
#                                                "trollstorehelper install-trollstore", as upstream's installer gives it
#   usr/share/hakios/trollstore/ldid(.version)   the static ldid TrollStore wants beside its app (it signs the
#                                                IPAs it installs with it); upstream's "install-ldid" would download it
#   usr/libexec/hakios-trollstore                the boot-time part, run from prep.sh (below)
#   prep.sh                                      gets one line that starts that part, if it has none yet
#
# Why not simply put TrollStore.app in /var/jb/Applications like Sileo: TrollStore finds its own copy through
# MobileContainerManager (MCMAppContainer containerWithIdentifier:com.opa334.TrollStore), for its update, its ldid
# and the helper it spawns; an application that is not in an app container (/var/containers/Bundle/Application/
# <UUID>/, with the container's metadata) leaves it with no path to itself. So at boot the part below does what
# upstream's persistence helper does -- trollstorehelper install-trollstore, as root -- once; the container then
# lives on /private/var, which is persistent, and every later boot only checks that LaunchServices still knows it.
#
# Signing: the upstream Mach-Os are signed the way TrollStore's trick wants (a borrowed identity, valid only
# through the CoreTrust bug), with their entitlements in the signature. This kernel checks no signature at all
# (xnu/files/libkern/amfi/miniamfi.c); what it reads is the entitlement list in the signature, so each binary is
# signed ad hoc again by ldid with the very entitlements it carried. The helper becomes root without a set-ID bit:
# TrollStore spawns it with posix_spawn and a persona override, which this kernel allows to anyone
# (docs/driver-review/policy-glue.md P1) and, besides, to a program with com.apple.private.persona-mgmt, which the
# main app has.
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
OUT=${1:-$ROOT/jb}
PREFIX=$OUT/root/var/jb
TS_VERSION=${TS_VERSION:-2.1.1}
TS_URL=https://github.com/opa334/TrollStore/releases/download/$TS_VERSION/TrollStore.tar
TS_SHA256=${TS_SHA256:-144dc741d1b74d582c861e6ed504ddced3745724e216e3de79ab7d37b27d0551}      # 2.1.1, MIT
LDID_TAG=v2.1.5-procursus7-new
LDID_URL=https://github.com/opa334/ldid/releases/download/$LDID_TAG/ldid
LDID_SHA256=b6aa4018e7ef3df9ee3fee5889d162649719109521c5a2136ca98a81fccdf52a                  # AGPL-3.0, only fetched
SIGN=${LDID:-ldid}
[ -d "$PREFIX" ] || { echo "jb-trollstore.sh: no $PREFIX (run tools/mk-jb.sh first)" >&2; exit 1; }
command -v "$SIGN" >/dev/null || { echo "jb-trollstore.sh: no ldid on this machine (brew install ldid)" >&2; exit 1; }
mkdir -p "$OUT/debs"

sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
get() {                                                 # get URL FILE SHA256: fetch once into debs/, verified every time
	[ -s "$OUT/debs/$2" ] || curl -sL --fail -o "$OUT/debs/$2" "$1"
	[ "$(sha256 "$OUT/debs/$2")" = "$3" ] || { echo "jb-trollstore.sh: $2 does not have the SHA-256 it is pinned to" >&2; rm -f "$OUT/debs/$2"; exit 1; }
}
get "$TS_URL" "TrollStore-$TS_VERSION.tar" "$TS_SHA256"
get "$LDID_URL" "ldid-$LDID_TAG" "$LDID_SHA256"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir "$WORK/x" "$WORK/ents"
tar -C "$WORK/x" -xf "$OUT/debs/TrollStore-$TS_VERSION.tar"
APP=$WORK/x/TrollStore.app
[ -x "$APP/trollstorehelper" ] && [ -f "$APP/Info.plist" ] || { echo "jb-trollstore.sh: no TrollStore.app/trollstorehelper in the archive" >&2; exit 1; }

# resign FILE IDENTIFIER: ad hoc, the entitlements it had (none is allowed: ldid has nothing to keep then)
resign() {
	local f=$1 id=$2 ents=$WORK/ents/$(basename "$1").plist
	"$SIGN" -e "$f" > "$ents" 2>/dev/null || true
	if [ -s "$ents" ]; then "$SIGN" -S"$ents" -I"$id" "$f"; else "$SIGN" -S -I"$id" "$f"; fi
	chmod 755 "$f"
	echo "jb-trollstore.sh: ${f#$WORK/x/} signed ad hoc as $id, $(grep -c '<key>' "$ents" || true) entitlements kept"
}
resign "$APP/TrollStore"        com.opa334.TrollStore
resign "$APP/PersistenceHelper" com.opa334.trollstorepersistencehelper
resign "$APP/trollstorehelper"  trollstorehelper
cp "$OUT/debs/ldid-$LDID_TAG" "$WORK/ldid"
resign "$WORK/ldid" ldid

SHARE=$PREFIX/usr/share/hakios/trollstore
rm -rf "$SHARE"
mkdir -p "$SHARE" "$PREFIX/usr/libexec"
# the archive as the helper's install-trollstore reads it: TrollStore.app at its top, no AppleDouble leftovers
(cd "$WORK/x" && COPYFILE_DISABLE=1 tar --no-xattrs -cf "$SHARE/TrollStore.tar" TrollStore.app)
cp "$WORK/ldid" "$SHARE/ldid"; chmod 755 "$SHARE/ldid"
printf '%s' "${LDID_TAG}" > "$SHARE/ldid.version"
echo "$TS_VERSION" > "$SHARE/version"

# The boot-time part. Aside and patient, like Sileo's registration: right after SpringBoard, LaunchServices and
# containermanagerd may not answer yet. Everything it prints goes into the log of prep.sh's caller.
cat > "$PREFIX/usr/libexec/hakios-trollstore" <<'BOOT'
#!/var/jb/bin/sh
# Installs TrollStore (once) into an app container and keeps it registered; called by /var/jb/prep.sh, as root.
D=/var/jb/usr/share/hakios/trollstore
STAGE=/var/jb/tmp/trollstore
JB=/var/jb/usr/bin
[ -f "$D/TrollStore.tar" ] || exit 0
installed() {                                           # the app's own copy in an app container, if there is one
	for p in /var/containers/Bundle/Application/*/TrollStore.app; do
		[ -x "$p/trollstorehelper" ] && { echo "$p"; return 0; }
	done
	return 1
}
for try in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24; do
	if app=$(installed); then
		# the ldid TrollStore wants beside itself (upstream's "install-ldid" fetches it from the network)
		[ -x "$app/ldid" ] || { cp "$D/ldid" "$app/ldid" && cp "$D/ldid.version" "$app/ldid.version" &&
			chmod 755 "$app/ldid" && echo "hakios-trollstore: ldid put in ${app}"; }
		# registered with LaunchServices, if it does not know it (the database may not have outlived the boot)
		if $JB/uicache -l 2>/dev/null | grep -q com.opa334.TrollStore; then
			echo "hakios-trollstore: TrollStore is installed ($app) and registered (try $try)"
			exit 0
		fi
		case "$($JB/uicache -p "$app" 2>&1)" in
		*Error*) echo "hakios-trollstore: uicache -p $app failed (try $try)" ;;
		*) echo "hakios-trollstore: TrollStore registered from $app (try $try)"; exit 0 ;;
		esac
	else
		rm -rf "$STAGE"; mkdir -p "$STAGE"
		if $JB/tar -C "$STAGE" -xf "$D/TrollStore.tar" 2>/dev/null; then
			out=$("$STAGE/TrollStore.app/trollstorehelper" install-trollstore "$D/TrollStore.tar" 2>&1)
			echo "hakios-trollstore: install-trollstore exit $? (try $try): $out"
		else
			echo "hakios-trollstore: cannot unpack $D/TrollStore.tar (try $try)"
		fi
		rm -rf "$STAGE"
	fi
	sleep 5
done
echo "hakios-trollstore: TrollStore could not be installed or registered"
exit 0
BOOT
chmod 755 "$PREFIX/usr/libexec/hakios-trollstore"

# prep.sh starts it, aside, once per boot (mk-jb.sh writes the line itself; a prefix built before has it added here)
HOOK='[ -x /var/jb/usr/libexec/hakios-trollstore ] && /var/jb/bin/sh /var/jb/usr/libexec/hakios-trollstore &'
if ! grep -qF hakios-trollstore "$PREFIX/prep.sh"; then
	awk -v hook="$HOOK" '/^\/var\/jb\/usr\/sbin\/pwd_mkdb/ && !done { print "# TrollStore, installed once into an app container and kept registered (tools/jb-trollstore.sh)"; print hook; done = 1 } { print }' \
		"$PREFIX/prep.sh" > "$PREFIX/prep.sh.new"
	cat "$PREFIX/prep.sh.new" > "$PREFIX/prep.sh" && rm -f "$PREFIX/prep.sh.new"
	chmod 755 "$PREFIX/prep.sh"
	grep -qF hakios-trollstore "$PREFIX/prep.sh" || { echo "jb-trollstore.sh: prep.sh has no place for the hook" >&2; exit 1; }
fi
echo "jb-trollstore.sh: TrollStore $TS_VERSION in $SHARE ($(du -sh "$SHARE" | cut -f1)), started from prep.sh"
