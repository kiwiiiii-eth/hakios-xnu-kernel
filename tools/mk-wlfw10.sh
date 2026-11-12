#!/bin/bash
# Give the kernel's build ath10k's descriptions of the messages a WCN3990's firmware speaks (the
# SDM845's Wi-Fi, whose firmware runs on the modem), for step 11 of drivers/modem/hakios.mpss845.c
# (a stand-in for ath10k's own qmi.c). They are taken out of the driver's sources as they are --
# qmi_wlfw_v01.h and the tables of qmi_wlfw_v01.c -- and written as one header into the kernel's
# build directory, where the compiler finds it and git does not, as tools/mk-wlfw.sh does for
# ath11k. ath10k's files are ISC; they are checked to say so.
# Without that header step 11 compiles to a line that says so.
#
# usage: tools/mk-wlfw10.sh          then build the kernel (make kernel); again after a clean build
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
[ -f "$ROOT/xnu-env.sh" ] && source "$ROOT/xnu-env.sh"
VERSION=7.0.14
B=$ROOT/xnu-deps/build/ath10k
TAR=$ROOT/xnu-deps/build/ath11k/linux-$VERSION.tar.xz
if [ ! -f "$B/src/qmi_wlfw_v01.c" ]; then
	[ -f "$TAR" ] || "$ROOT/tools/mk-ath11k.sh" fetch
	rm -rf "$B/src" && mkdir -p "$B/src"
	tar xJf "$TAR" -C "$B/src" --strip-components=6 linux-$VERSION/drivers/net/wireless/ath/ath10k
	echo "$VERSION" > "$B/src/.release"
fi
for f in "$B/src/qmi_wlfw_v01.h" "$B/src/qmi_wlfw_v01.c" "$B/src/snoc.c"; do
	head -3 "$f" | grep -q "SPDX-License-Identifier: ISC" || { echo "mk-wlfw10.sh: $f does not carry the licence expected" >&2; exit 1; }
done
SRC=$ROOT/xnu-src/xnu-8796.121.2
OUT=$SRC/BUILD/obj/DEVELOPMENT_ARM64_${MACHINE:-BCM2837}/bsd/DEVELOPMENT
[ -d "$OUT" ] || { echo "mk-wlfw10.sh: no build directory $OUT: build the kernel once first" >&2; exit 1; }
python3 - "$B/src" "$VERSION" > "$OUT/hakios.wlfw_ath10k.h.new" <<'PY'
import re
import sys

src, release = sys.argv[1], sys.argv[2]
h = open(src + '/qmi_wlfw_v01.h').read()
c = open(src + '/qmi_wlfw_v01.c').read()
snoc = open(src + '/snoc.c').read()
print('/* ath10k\'s qmi_wlfw_v01.h and the tables of its qmi_wlfw_v01.c (Linux %s, ISC), put together by' % release)
print(' * tools/mk-wlfw10.sh: not to be edited, not to be kept. The tables are made static here. */')
print('''#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
''')
h = re.sub(r'^#include [<"][^>"]*[>"]\n', '', h, flags=re.M)
h = re.sub(r'^extern const struct qmi_elem_info \w+\[\];\n', '', h, flags=re.M)
print(h)
tables = re.findall(r'^(?:static )?const struct qmi_elem_info \w+\[\] = \{.*?\n\};\n', c, re.S | re.M)
print('\n'.join('static ' + re.sub(r'^static ', '', t) for t in tables))

# What the configuration message (W4b) is made of for this chip: snoc.c's two tables, and the
# definitions they are written in come from ce.h/htc.h then; for now only the counts are said.
for name in ('target_ce_config_wlan', 'target_service_to_ce_map_wlan'):
    m = re.search(r'static const struct \w+ %s\[\] = \{' % name, snoc)
    print('/* snoc.c has %s: %s */' % (name, 'yes' if m else 'NO'))
PY
mv "$OUT/hakios.wlfw_ath10k.h.new" "$OUT/hakios.wlfw_ath10k.h"
echo "mk-wlfw10.sh: $OUT/hakios.wlfw_ath10k.h ($(grep -c 'qmi_elem_info .*\[\] = {' "$OUT/hakios.wlfw_ath10k.h") tables)"
