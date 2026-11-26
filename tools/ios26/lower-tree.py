#!/usr/bin/env python3
"""The programs of a tree, lowered, as a tree of their own to lay over it (tools/apfs/mkapfs.py NAME=TREE,over=OUT).

usage: lower-tree.py [--skip PREFIX]... [--at PREFIX] PAC_LOWER TREE OUT

Every regular file of TREE that is a Mach-O image with an arm64 part (thin or universal) is copied to the same
path below OUT (below OUT/PREFIX with --at), with its permission bits, and tools/lower's pac_lower run on the
copies: TREE itself -- a mounted disk image of the IPSW, read only -- stays as it is. --skip leaves out what is
below a path of TREE (the dyld shared cache, which pac_lower --cache does).
"""
import argparse
import os
import shutil
import stat
import struct
import subprocess
import sys

MH_MAGIC_64, FAT_MAGIC, FAT_MAGIC_64, CPU_TYPE_ARM64 = 0xFEEDFACF, 0xCAFEBABE, 0xCAFEBABF, 0x0100000C


def has_arm64(path):
    try:
        with open(path, 'rb') as f:
            head = f.read(4096)
    except OSError:
        return False
    if len(head) < 32:
        return False
    if struct.unpack_from('<I', head)[0] == MH_MAGIC_64:
        return struct.unpack_from('<I', head, 4)[0] == CPU_TYPE_ARM64
    magic = struct.unpack_from('>I', head)[0]
    if magic in (FAT_MAGIC, FAT_MAGIC_64):
        count = struct.unpack_from('>I', head, 4)[0]
        entry = 32 if magic == FAT_MAGIC_64 else 20
        if count > 40:                  # a Java class file has the same first four bytes
            return False
        return any(struct.unpack_from('>I', head, 8 + i * entry)[0] == CPU_TYPE_ARM64
                   for i in range(count) if 8 + (i + 1) * entry <= len(head))
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--skip', action='append', default=[])
    ap.add_argument('--at', default='')
    ap.add_argument('pac_lower')
    ap.add_argument('tree')
    ap.add_argument('out')
    args = ap.parse_args()
    copies, scanned = [], 0
    for base, dirs, files in os.walk(args.tree):
        rel = os.path.relpath(base, args.tree)
        rel = '' if rel == '.' else rel
        dirs[:] = [d for d in dirs if not any((os.path.join(rel, d) + '/').startswith(s.rstrip('/') + '/') for s in args.skip)]
        for name in files:
            src = os.path.join(base, name)
            st = os.lstat(src)
            if not stat.S_ISREG(st.st_mode):
                continue
            scanned += 1
            if st.st_size < 32 or not has_arm64(src):
                continue
            dst = os.path.join(args.out, args.at, rel, name)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.lexists(dst):
                os.chmod(dst, 0o644)
                os.unlink(dst)
            shutil.copyfile(src, dst)
            os.chmod(dst, 0o644)                    # writable for pac_lower; the file's own bits afterwards
            copies.append((dst, stat.S_IMODE(st.st_mode)))
    print('lower-tree.py: %s: %d files, %d of them programs' % (args.tree, scanned, len(copies)), flush=True)
    status = 0
    for i in range(0, len(copies), 500):
        r = subprocess.run([args.pac_lower, '-q'] + [c[0] for c in copies[i:i + 500]], stdout=subprocess.PIPE, text=True)
        status |= r.returncode
        sys.stdout.write(r.stdout.replace('total: ', 'lower-tree.py: '))
    for dst, mode in copies:
        os.chmod(dst, mode)
    return 1 if status else 0


if __name__ == '__main__':
    sys.exit(main())
