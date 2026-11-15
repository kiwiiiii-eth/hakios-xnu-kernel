#!/usr/bin/env python3
"""The data volume of Darwin for the phone: an empty HFS Plus volume in darwin past the Darwin volume and the cryptexes,
as an Android sparse image of the whole of darwin that writes that volume and nothing else.

usage: mkdatavol.py OUT.simg [MKFS] [--first BLOCKS --darwin BLOCKS]
  MKFS     mkfs.hfsplus (Linux's hfsprogs), default the one in $PATH; on a Mac without it, macOS's own newfs_hfs on the
           image attached with hdiutil (no Linux tools)
  --first  where the volume begins in darwin, in blocks of 4096 (default 2042368, the reference phone's; the 6T's is
           3145728, 12 GiB, past var_store's two slots: hakios-fajita configs/fajita.config, CONFIG_UFS_DATA_FIRST)
  --darwin darwin's size in blocks of 4096 (default 5242875, the reference phone's; a 6T's is its GPT's, 8389115 for
           32 GiB: tools/gpt-plan.py info)

    fastboot flash darwin OUT.simg

The kernel gives that stretch of darwin out as /dev/ufs_darwin_data (drivers/storage/hakios.ufs.c, ufs_write=1) and
init mounts it on /private/var (userland/src/init.c, var_hfs=ufs_darwin_data). Flashing this again empties it.

The image says DONT_CARE for darwin's blocks before the volume -- the Darwin volume and both cryptexes, which the
bootloader therefore leaves as they are -- and, inside the volume, for every block the new volume does not use: what
is in free space does not matter to HFS Plus, and twelve gigabytes of zeros need not go over USB. The blocks it uses
(the volume headers, the allocation bitmap, the catalog and extents trees, the journal) are written whole, zeros
included: which those are is read from the volume's own allocation bitmap.

Case-sensitive (HFSX), as Darwin's own data volume is, and journaled.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

BS = 4096
DARWIN_BLOCKS = 5242875            # darwin, in blocks of 4096 (README, "The port's partition")
DATA_FIRST = 2042368               # where the volume begins: drivers/storage/hakios.ufs.c, UFS_WRITE_FIRST
MAGIC, RAW, FILL, DONT_CARE = 0xED26FF3A, 0xCAC1, 0xCAC2, 0xCAC3


def chunk(kind, blocks, data_size):
    return struct.pack('<HHII', kind, 0, blocks, 12 + data_size)


def used_blocks(img):
    """The volume's allocation blocks marked in use, from its header and bitmap; the block size must be BS."""
    img.seek(1024)
    vh = img.read(512)
    sig, = struct.unpack('>H', vh[0:2])
    if sig not in (0x482B, 0x4858):                     # H+, HX
        sys.exit('mkdatavol.py: no HFS Plus volume header in what mkfs made')
    block_size, total = struct.unpack('>II', vh[40:48])
    if block_size != BS:
        sys.exit(f'mkdatavol.py: the volume has blocks of {block_size}, not {BS}')
    fork = vh[112:192]                                  # allocationFile: HFSPlusForkData
    extents = [struct.unpack('>II', fork[16 + 8 * i:24 + 8 * i]) for i in range(8)]
    bitmap = bytearray()
    for start, count in extents:
        if count:
            img.seek(start * BS)
            bitmap += img.read(count * BS)
    if len(bitmap) * 8 < total:
        sys.exit('mkdatavol.py: the allocation file is in more pieces than its fork says')
    return total, lambda b: bitmap[b >> 3] & (0x80 >> (b & 7))


def make_volume(mkfs, path):
    """an empty HFSX volume in the file at path: its root is /private/var, root's and 0755, as on an iPhone (a root of
    another owner makes Darwin take it for a volume to migrate, and it stays at the progress bar)"""
    flags = ['-s', '-J', '-b', str(BS), '-U', '0', '-G', '0', '-M', '755', '-v', 'Data']
    if mkfs != 'macos':
        subprocess.run([mkfs] + flags + [path], check=True)
        return
    # macOS: newfs_hfs takes a device, so the file is attached as a raw disk (no partition map: the volume is the file)
    out = subprocess.run(['hdiutil', 'attach', '-nomount', '-imagekey', 'diskimage-class=CRawDiskImage', path],
                         check=True, capture_output=True, text=True).stdout.split()
    disk = next((w for w in out if w.startswith('/dev/disk')), None)
    if disk is None:
        sys.exit(f'mkdatavol.py: hdiutil attached no disk ({out})')
    try:
        subprocess.run(['/sbin/newfs_hfs'] + flags + [disk.replace('/dev/disk', '/dev/rdisk')], check=True,
                       capture_output=True)
    finally:
        subprocess.run(['hdiutil', 'detach', disk], check=True, capture_output=True)


def root_owner(img):
    """(owner, group, mode) of the volume's root folder (catalog node ID 2), from its catalog B-tree's leaf records"""
    img.seek(1024)
    vh = img.read(512)
    fork = vh[272:352]                                  # catalogFile: HFSPlusForkData
    start, count = struct.unpack('>II', fork[16:24])
    img.seek(start * BS)
    header = img.read(512)
    first_leaf, = struct.unpack('>I', header[14 + 10:14 + 14])
    node_size, = struct.unpack('>H', header[14 + 18:14 + 20])
    node = first_leaf
    while node:
        img.seek(start * BS + node * node_size)
        n = img.read(node_size)
        flink, = struct.unpack('>I', n[0:4])
        records, = struct.unpack('>H', n[10:12])
        for r in range(records):
            at, = struct.unpack('>H', n[node_size - 2 * (r + 1):node_size - 2 * r])
            key_len, = struct.unpack('>H', n[at:at + 2])
            data = at + 2 + key_len
            data += data & 1
            kind, = struct.unpack('>H', n[data:data + 2])
            if kind == 1 and struct.unpack('>I', n[data + 8:data + 12])[0] == 2:    # a folder record, folderID 2
                owner, group = struct.unpack('>II', n[data + 32:data + 40])
                mode, = struct.unpack('>H', n[data + 42:data + 44])
                return owner, group, mode
        node = flink
    sys.exit("mkdatavol.py: no root folder in the volume's catalog")


def main():
    args = sys.argv[1:]
    vol_first, darwin = DATA_FIRST, DARWIN_BLOCKS
    for name in ('--first', '--darwin'):
        if name in args:
            i = args.index(name)
            value = int(args[i + 1])
            del args[i:i + 2]
            vol_first, darwin = (value, darwin) if name == '--first' else (vol_first, value)
    if len(args) not in (1, 2) or not 0 < vol_first < darwin:
        sys.exit(__doc__)
    out = args[0]
    mkfs = args[1] if len(args) > 1 else ('mkfs.hfsplus' if shutil.which('mkfs.hfsplus') or sys.platform != 'darwin'
                                          else 'macos')
    blocks = darwin - vol_first
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'data.hfs')
        with open(path, 'wb') as f:
            f.truncate(blocks * BS)
        make_volume(mkfs, path)
        with open(path, 'rb') as img:
            total, used = used_blocks(img)
            if total != blocks:
                sys.exit(f'mkdatavol.py: the volume has {total} blocks, darwin has {blocks} for it')
            owner = root_owner(img)
            if owner[0] != 0 or owner[1] != 0 or owner[2] & 0o7777 != 0o755:
                sys.exit(f"mkdatavol.py: the volume's root is {owner[0]}:{owner[1]} {oct(owner[2])}, not root's 0755")
            runs = []                                   # [kind, first, count], in order
            for b in range(total):
                kind = RAW if used(b) else DONT_CARE
                if runs and runs[-1][0] == kind and runs[-1][1] + runs[-1][2] == b:
                    runs[-1][2] += 1
                else:
                    runs.append([kind, b, 1])
            chunks, written = [chunk(DONT_CARE, vol_first, 0)], 0
            for kind, first, count in runs:
                if kind == DONT_CARE:
                    chunks.append(chunk(DONT_CARE, count, 0))
                    continue
                img.seek(first * BS)
                data = img.read(count * BS)
                # zeros in a used stretch go as FILL, which costs nothing to send; everything else as RAW
                at = 0
                while at < count:
                    zero = data[at * BS:(at + 1) * BS] == bytes(BS)
                    end = at
                    while end < count and (data[end * BS:(end + 1) * BS] == bytes(BS)) == zero:
                        end += 1
                    if zero:
                        chunks.append(chunk(FILL, end - at, 4) + bytes(4))
                    else:
                        chunks.append(chunk(RAW, end - at, (end - at) * BS) + data[at * BS:end * BS])
                    written += end - at
                    at = end
    with open(out, 'wb') as f:
        f.write(struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, BS, darwin, len(chunks), 0))
        for c in chunks:
            f.write(c)
    print(f'{out}: {os.path.getsize(out)} bytes; the volume: {blocks} blocks ({blocks * BS >> 20} MB) from darwin '
          f'block {vol_first}, {written} of them written, the rest of darwin left as it is; its root {owner[0]}:{owner[1]} '
          f'{oct(owner[2] & 0o7777)}')


if __name__ == '__main__':
    main()
