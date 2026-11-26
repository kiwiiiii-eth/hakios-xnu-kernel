#!/usr/bin/env python3
"""Names for addresses in a dyld shared cache: the library, and the nearest symbol of its symbol table before it.

usage: a2s.py CACHE [--slide 0xSLIDE] 0xADDRESS...      CACHE: its first file (dyld_shared_cache_arm64e)
       a2s.py CACHE --log LOG [PROCESS]                 the "usertrap:" and "userabort:" lines of a console log

Reads the cache's files directly (dyld's <mach-o/dyld_cache_format.h>, <mach-o/loader.h>, <mach-o/nlist.h>): the list
of images, each one's Mach-O header for its segments and LC_SYMTAB, and -- for the functions that are not exported --
the local symbols of the cache's .symbols file when it lies beside it. No other tool is needed.
"""
import bisect
import mmap
import os
import re
import struct
import sys


class Cache:
    def __init__(self, path):
        self.files, self.mappings = [], []
        self.open(path)
        h = self.files[0]
        header = struct.unpack_from('<I', h, 0x10)[0]
        sub_off, sub_count = struct.unpack_from('<II', h, 0x188)
        named = header > 0x1C8
        for i in range(sub_count):
            entry = sub_off + i * (56 if named else 24)
            suffix = h[entry + 24:entry + 56].split(b'\0')[0].decode() if named else '.%d' % (i + 1)
            self.open(path + suffix)
        off, count = struct.unpack_from('<II', h, 0x1C0 if header > 0x1C0 else 0x18)
        self.images = []                # (text start, text end, name, header address)
        for i in range(count):
            address, _, _, path_off = struct.unpack_from('<QQQI', h, off + i * 32)
            name = h[path_off:h.find(b'\0', path_off)].decode()
            self.images.append((address, name))
        self.images.sort()
        self.symbols = {}               # image address -> sorted [(address, name)]
        self.locals = None
        symbols = path + '.symbols'
        if os.path.exists(symbols):
            f = open(symbols, 'rb')
            self.locals = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)

    def open(self, path):
        f = open(path, 'rb')
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        off, count = struct.unpack_from('<II', m, 0x10)
        for i in range(count):
            address, size, file_off = struct.unpack_from('<QQQ', m, off + i * 32)
            self.mappings.append((address, size, file_off, len(self.files)))
        self.files.append(m)

    def at(self, address, size):
        for start, length, file_off, index in self.mappings:
            if start <= address and address + size <= start + length:
                return self.files[index], file_off + address - start
        return None, 0

    def image_of(self, address):
        """The image whose segments hold the address: (header address, name) or None."""
        for header, name in self.images:
            m, at = self.at(header, 32)
            if m is None:
                continue
            ncmds = struct.unpack_from('<I', m, at + 16)[0]
            pos = at + 32
            for _ in range(ncmds):
                cmd, size = struct.unpack_from('<II', m, pos)
                if cmd == 0x19:
                    vmaddr, vmsize = struct.unpack_from('<QQ', m, pos + 24)
                    if vmaddr <= address < vmaddr + vmsize and not m[pos + 8:pos + 24].startswith(b'__LINKEDIT'):
                        return header, name
                pos += size
        return None

    def symbols_of(self, header):
        if header in self.symbols:
            return self.symbols[header]
        out = []
        m, at = self.at(header, 32)
        ncmds = struct.unpack_from('<I', m, at + 16)[0]
        pos, linkedit, symtab = at + 32, None, None
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', m, pos)
            if cmd == 0x19 and m[pos + 8:pos + 24].startswith(b'__LINKEDIT'):
                linkedit = struct.unpack_from('<QQQ', m, pos + 24)      # vmaddr, vmsize, fileoff
            if cmd == 0x2:
                symtab = struct.unpack_from('<IIII', m, pos + 8)        # symoff, nsyms, stroff, strsize
            pos += size
        if linkedit and symtab:
            base = linkedit[0] - linkedit[2]
            lm, sym_at = self.at(base + symtab[0], symtab[1] * 16)
            sm, str_at = self.at(base + symtab[2], symtab[3])
            if lm is not None and sm is not None:
                for i in range(symtab[1]):
                    strx, n_type, _, _, value = struct.unpack_from('<IBBHQ', lm, sym_at + i * 16)
                    if (n_type & 0x0E) == 0x0E and not (n_type & 0xE0) and value:
                        end = sm.find(b'\0', str_at + strx)
                        out.append((value, sm[str_at + strx:end].decode(errors='replace')))
        # the functions that are not exported: the .symbols file, by the image's offset in the cache
        if self.locals is not None:
            lm = self.locals
            info = struct.unpack_from('<Q', lm, 0x48)[0]                # localSymbolsOffset
            nlist_off, nlist_count, strings_off, strings_size, entries_off, entries_count = \
                struct.unpack_from('<IIIIII', lm, info)
            base = self.images[0][0] if False else struct.unpack_from('<Q', self.files[0], 0xE0)[0]  # sharedRegionStart
            for i in range(entries_count):
                dylib_off, start, count = struct.unpack_from('<QII', lm, info + entries_off + i * 16)
                if base + dylib_off != header:
                    continue
                for k in range(start, start + count):
                    strx, n_type, _, _, value = struct.unpack_from('<IBBHQ', lm, info + nlist_off + k * 16)
                    if (n_type & 0x0E) == 0x0E and not (n_type & 0xE0) and value:
                        end = lm.find(b'\0', info + strings_off + strx)
                        out.append((value, lm[info + strings_off + strx:end].decode(errors='replace')))
                break
        out.sort()
        self.symbols[header] = out
        return out

    def name(self, address):
        found = self.image_of(address)
        if found is None:
            return '%#x (not in the shared cache)' % address
        header, image = found
        symbols = self.symbols_of(header)
        i = bisect.bisect_right(symbols, (address, '￿')) - 1
        short = os.path.basename(image)
        if i < 0:
            return '%#x %s + %#x' % (address, short, address - header)
        return '%#x %s: %s + %d' % (address, short, symbols[i][1], address - symbols[i][0])


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    cache = Cache(args[0])
    if args[1] == '--log':
        process = args[3] if len(args) > 3 else None
        seen = set()
        for line in open(args[2], errors='replace'):
            m = re.search(r'(usertrap|userabort): (\S+)\[\d+\].*shared cache slide (0x[0-9a-f]+)', line)
            if not m or (process and m.group(2) != process):
                continue
            slide = int(m.group(3), 16)
            addresses = [int(a, 16) for a in re.findall(r'0x[0-9a-f]{8,}', line.split('shared cache slide')[1])[1:]]
            pc = re.search(r'pc (0x[0-9a-f]+) lr (0x[0-9a-f]+)', line)
            if pc:
                addresses = [int(pc.group(1), 16), int(pc.group(2), 16)] + addresses
            key = (m.group(2), tuple(a - slide for a in addresses))
            if key in seen:
                continue
            seen.add(key)
            print('%s %s:' % (m.group(1), m.group(2)))
            for a in addresses:
                print('    ' + cache.name(a - slide))
        return
    slide = 0
    if args[1] == '--slide':
        slide = int(args[2], 16)
        args = args[:1] + args[3:]
    for a in args[1:]:
        print(cache.name(int(a, 16) - slide))


if __name__ == '__main__':
    main()
