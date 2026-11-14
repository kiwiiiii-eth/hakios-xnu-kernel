#!/usr/bin/env python3
"""ufsgpt2bin.py DIR LOG [LOG ...]: logical unit 0's first and last eight blocks, out of a run's log (boot-arg ufs_gpt_dump=1:
"UFSGPT head|tail N BASE64" lines, drivers/storage/hakios.ufs.c), into DIR/gpt-head.bin and DIR/gpt-tail.bin -- what
tools/userdata-carve.py takes, and a copy of the partition table to put back. Several logs (the live ones of the same run)
are read together; every line must be there, and both GPT headers must check (their CRC-32s) or nothing is written.
"""
import base64
import os
import re
import struct
import sys
import zlib

BLOCK = 4096
LINES = (8 * BLOCK + 95) // 96               # 342: the last line holds the last 32 bytes


def header_ok(block, what):
    if block[:8] != b'EFI PART':
        sys.exit('ufsgpt2bin.py: %s: no GPT header' % what)
    size = struct.unpack_from('<I', block, 12)[0]
    h = bytearray(block[:size])
    want = struct.unpack_from('<I', h, 16)[0]
    h[16:20] = b'\0\0\0\0'
    if zlib.crc32(bytes(h)) & 0xffffffff != want:
        sys.exit('ufsgpt2bin.py: %s: the GPT header\'s CRC does not check' % what)
    return struct.unpack_from('<QQQQ', block, 24)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, logs = sys.argv[1], sys.argv[2:]
    got = {'head': {}, 'tail': {}}
    for log in logs:
        for raw in open(log, 'rb').read().decode('latin-1').splitlines():
            m = re.search(r'UFSGPT (head|tail) (\d+) ([A-Za-z0-9+/=]{4,128})\s*$', raw)
            if m:
                got[m.group(1)][int(m.group(2))] = base64.b64decode(m.group(3))
    blobs = {}
    for which in ('head', 'tail'):
        missing = [n for n in range(LINES) if n not in got[which]]
        if missing:
            sys.exit('ufsgpt2bin.py: %s: %d of %d lines missing (the first: %d)' % (which, len(missing), LINES, missing[0]))
        blobs[which] = b''.join(got[which][n] for n in range(LINES))[:8 * BLOCK]
    current, backup, first_usable, last_usable = header_ok(blobs['head'][BLOCK:2 * BLOCK], 'head')
    alt = header_ok(blobs['tail'][7 * BLOCK:], 'tail')
    if alt[0] != backup or alt[1] != current:
        sys.exit('ufsgpt2bin.py: the backup header does not point back at the primary one')
    os.makedirs(out, exist_ok=True)
    for which in ('head', 'tail'):
        with open(os.path.join(out, 'gpt-%s.bin' % which), 'wb') as f:
            f.write(blobs[which])
    print('ufsgpt2bin.py: %s/gpt-head.bin and gpt-tail.bin: LUN 0 of %d blocks (the backup header at %d), usable %d..%d'
          % (out, backup + 1, backup, first_usable, last_usable))


if __name__ == '__main__':
    main()
