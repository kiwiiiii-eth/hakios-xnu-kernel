#!/usr/bin/env python3
"""gpt-plan.py: the partition table Beta 0.4's install.sh and uninstall.sh have the loader write ("oem gptwrite"):
made here from what the phone has, checked here by the same rules the loader holds it to (xnuloader/gpt.c), and
written as the file the loader is sent (a primary header block and the blocks of its entries).

usage: gpt-plan.py info HEAD TAIL
       gpt-plan.py carve HEAD TAIL GIB OUT
       gpt-plan.py restore HEAD TAIL ORIG_HEAD ORIG_TAIL OUT
       gpt-plan.py check HEAD TAIL STAGED
       gpt-plan.py mend HEAD TAIL OUT

  HEAD, TAIL            the first and last 8 blocks of the unit that holds userdata (oem readunit), as it is now
  ORIG_HEAD, ORIG_TAIL  the same, as saved before HakiOS was installed (the backup install.sh keeps)
  carve GIB             darwin made of the last GIB GiB of the unit (from a 2 MiB boundary), userdata ending before
                        it: Android's data goes (fastboot -w after the write), as userdata-carve.py --wipe
  restore               the saved table again: darwin gone, userdata to its old end
  check                 a staged table held to the rules, as the loader will
  mend                  the two copies on the flash made one again (a write cut short): the primary as it is when it
                        checks, else the backup as a primary

Each prints what it finds and ends with one line for a script:
  TABLE userdata=FIRST..LAST darwin=FIRST..LAST|none last_usable=N block=4096   (info, and the result of carve/restore)
and exits 0, or 1 with a line REFUSE: why.
"""
import os
import struct
import sys
import uuid
import zlib

BS = 4096
APFS = uuid.UUID('7c3457ef-0000-11aa-aa11-00306543ecac').bytes_le
MIN_USERDATA = 4 << 30
MIN_DARWIN = 12 << 30
ALIGN = 2 << 20


class Refuse(Exception):
    pass


def crc(b):
    return zlib.crc32(b) & 0xffffffff


def u32(b, at):
    return struct.unpack_from('<I', b, at)[0]


def u64(b, at):
    return struct.unpack_from('<Q', b, at)[0]


def header_checks(h):
    if h[:8] != b'EFI PART':
        return False
    size, count, es = u32(h, 12), u32(h, 80), u32(h, 84)
    if not 92 <= size <= 512 or es < 128 or es % 8 or count == 0 or count * es > 64 * 1024:
        return False
    c = bytearray(h[:size])
    struct.pack_into('<I', c, 16, 0)
    return crc(bytes(c)) == u32(h, 16)


def entries_bytes(h):
    return u32(h, 80) * u32(h, 84)


def table_checks(h, entries):
    return header_checks(h) and crc(entries[:entries_bytes(h)]) == u32(h, 88)


def seal(h, entries):
    """header h (bytes) with both CRCs made right for entries"""
    h = bytearray(h)
    struct.pack_into('<I', h, 88, crc(entries[:entries_bytes(h)]))
    struct.pack_into('<I', h, 16, 0)
    struct.pack_into('<I', h, 16, crc(bytes(h[:u32(h, 12)])))
    return bytes(h)


def copies(head, tail):
    """the two copies on the flash from the 8-block dumps: (p_hdr, p_entries, p_ok, b_hdr, b_entries, b_ok, disk)"""
    if len(head) != 8 * BS or len(tail) != 8 * BS:
        raise Refuse('HEAD and TAIL are to be 8 blocks of 4096 bytes each')
    ph, bh = head[BS:2 * BS], tail[7 * BS:8 * BS]
    disk = u64(ph, 32) + 1 if header_checks(ph) else (u64(bh, 24) + 1 if header_checks(bh) else 0)
    p_ok = header_checks(ph) and u64(ph, 24) == 1 and u64(ph, 32) == disk - 1 and \
        u64(ph, 72) + (entries_bytes(ph) + BS - 1) // BS <= 8
    pe = head[u64(ph, 72) * BS:u64(ph, 72) * BS + entries_bytes(ph)] if p_ok else b''
    p_ok = p_ok and table_checks(ph, pe)
    b_ok = disk != 0 and header_checks(bh) and u64(bh, 24) == disk - 1 and u64(bh, 32) == 1 and \
        disk - 8 <= u64(bh, 72) < disk - 1
    be = tail[(u64(bh, 72) - (disk - 8)) * BS:][:entries_bytes(bh)] if b_ok else b''
    b_ok = b_ok and table_checks(bh, be)
    return ph, pe, p_ok, bh, be, b_ok, disk


def entry(h, entries, i):
    es = u32(h, 84)
    return entries[i * es:(i + 1) * es]


def empty(e):
    return e[:16] == bytes(16)


def name_of(e):
    return e[56:128].decode('utf-16le', 'replace').split('\0', 1)[0]


def index_of(h, entries, name):
    found = [i for i in range(u32(h, 80)) if not empty(entry(h, entries, i)) and name_of(entry(h, entries, i)) == name]
    return found[0] if len(found) == 1 else (-1 if not found else -2)


def rng(e):
    return (u64(e, 32), u64(e, 40))


def rules(ref_h, ref_e, new_h, new_e, primary_entries_lba):
    """the staged table against one copy of the phone's: what changes, or Refuse (xnuloader/gpt.c, gpt_rules)"""
    if not table_checks(new_h, new_e):
        raise Refuse("the staged table's CRCs do not check")
    if u64(new_h, 24) != 1:
        raise Refuse('the staged header is not a primary one')
    size = u32(ref_h, 12)
    if size > 512 or u32(new_h, 12) != size:
        raise Refuse("the staged header is not the size of the phone's")
    want = bytearray(ref_h[:size])
    if u64(ref_h, 24) != 1:
        struct.pack_into('<QQ', want, 24, u64(ref_h, 32), u64(ref_h, 24))
        struct.pack_into('<Q', want, 72, primary_entries_lba)
    if any(want[i] != new_h[i] for i in range(size) if not (16 <= i < 20 or 88 <= i < 92)):
        raise Refuse("the staged header differs from the phone's in more than its CRCs")
    count, es = u32(new_h, 80), u32(new_h, 84)
    ud_was, ud = index_of(ref_h, ref_e, 'userdata'), index_of(new_h, new_e, 'userdata')
    if ud_was < 0 or ud != ud_was:
        raise Refuse('not one userdata in each table, at the same entry')
    dw_was, dw = index_of(ref_h, ref_e, 'darwin'), index_of(new_h, new_e, 'darwin')
    if -2 in (dw_was, dw):
        raise Refuse('more than one darwin')
    for i in range(count):
        was, now = entry(ref_h, ref_e, i), entry(new_h, new_e, i)
        if was == now:
            continue
        if i == ud:
            if was[:40] != now[:40] or was[48:] != now[48:]:
                raise Refuse('userdata changes in more than its end')
            continue
        if not ((i == dw_was or empty(was)) and (i == dw or empty(now)) and i in (dw_was, dw)):
            raise Refuse('an entry other than userdata and darwin changes')
        if dw_was >= 0 and dw >= 0:
            raise Refuse('darwin would move or change: it is only added or taken away')
    named = [entry(new_h, new_e, i)[56:128] for i in range(count)
             if not empty(entry(new_h, new_e, i)) and entry(new_h, new_e, i)[56:58] != b'\0\0']
    if len(named) != len(set(named)):
        raise Refuse('two entries have the same name')
    u = rng(entry(new_h, new_e, ud))
    change = {'userdata_was': rng(entry(ref_h, ref_e, ud)), 'userdata': u,
              'darwin_was': rng(entry(ref_h, ref_e, dw_was)) if dw_was >= 0 else None, 'darwin': None}
    if u[1] < u[0] or u[1] > u64(new_h, 48):
        raise Refuse("userdata would end outside its unit's usable blocks")
    if (u[1] - u[0] + 1) * BS < MIN_USERDATA:
        raise Refuse('userdata would be smaller than 4 GiB')
    if dw >= 0:
        d = entry(new_h, new_e, dw)
        dr = change['darwin'] = rng(d)
        if d[:16] != APFS:
            raise Refuse('darwin is not of the APFS container type')
        if dr[0] <= u[1] or dr[1] < dr[0] or dr[1] > u64(new_h, 48):
            raise Refuse("darwin is not between userdata's end and the last usable block")
        if dr[0] * BS % ALIGN:
            raise Refuse('darwin does not begin on a 2 MiB boundary')
        if (dr[1] - dr[0] + 1) * BS < MIN_DARWIN:
            raise Refuse('darwin would be smaller than 12 GiB')
    for i in range(count):
        e = entry(new_h, new_e, i)
        if empty(e):
            continue
        a, b = rng(e)
        if i != ud and a <= u[1] and u[0] <= b:
            raise Refuse('another entry overlaps userdata')
        if dw >= 0 and i != dw and a <= change['darwin'][1] and change['darwin'][0] <= b:
            raise Refuse('another entry overlaps darwin')
    return change


def decide(head, tail, staged):
    """the staged table against both copies on the flash (xnuloader/gpt.c, gpt_decide): the change, or Refuse"""
    ph, pe, p_ok, bh, be, b_ok, _ = copies(head, tail)
    if len(staged) < 2 * BS or len(staged) % BS or not header_checks(staged[:BS]) or \
            BS + (entries_bytes(staged[:BS]) + BS - 1) // BS * BS != len(staged):
        raise Refuse('the staged table is not a header and its entries')
    nh, ne = staged[:BS], staged[BS:BS + entries_bytes(staged[:BS])]
    if not p_ok and not b_ok:
        raise Refuse("neither copy of the phone's table checks")
    pel = u64(ph, 72) if p_ok else 2
    change = rules(ph, pe, nh, ne, pel) if p_ok else None
    if b_ok:
        other = rules(bh, be, nh, ne, pel)
        change = change or other
    change['mend'] = not p_ok or not b_ok or pe != be
    change['same'] = not change['mend'] and ph[:u32(ph, 12)] == nh[:u32(ph, 12)] and pe == ne
    return change


def table_line(h, entries):
    ud, dw = index_of(h, entries, 'userdata'), index_of(h, entries, 'darwin')
    u = rng(entry(h, entries, ud)) if ud >= 0 else None
    d = rng(entry(h, entries, dw)) if dw >= 0 else None
    return (f'TABLE userdata={u[0]}..{u[1]} ' if u else 'TABLE userdata=none ') + \
        (f'darwin={d[0]}..{d[1]} ' if d else 'darwin=none ') + f'last_usable={u64(h, 48)} block={BS}'


def staged_of(h, entries):
    """the file the loader is sent: the header block (zeros past the header) and the entries' blocks"""
    blk = bytearray(BS)
    blk[:u32(h, 12)] = h[:u32(h, 12)]
    body = bytearray((entries_bytes(h) + BS - 1) // BS * BS)
    body[:entries_bytes(h)] = entries[:entries_bytes(h)]
    return bytes(blk) + bytes(body)


def say_change(c):
    def r(x):
        return f'{x[0]}..{x[1]}' if x else 'none'
    print(f'userdata: {r(c["userdata_was"])} -> {r(c["userdata"])}'
          f' ({(c["userdata"][1] - c["userdata"][0] + 1) * BS / 2**30:.2f} GiB)')
    print(f'darwin:   {r(c["darwin_was"])} -> {r(c["darwin"])}' +
          (f' ({(c["darwin"][1] - c["darwin"][0] + 1) * BS / 2**30:.2f} GiB)' if c['darwin'] else ''))
    print(f'nothing else changes{"; the two copies on the flash differ now, and are made one" if c["mend"] else ""}'
          f'{"; it is the table the phone has: nothing to write" if c["same"] else ""}')


def both_agree(head, tail):
    ph, pe, p_ok, bh, be, b_ok, disk = copies(head, tail)
    if not (p_ok and b_ok and pe == be and u64(ph, 32) == disk - 1):
        raise Refuse("the phone's two copies of its table do not both check and agree")
    return ph, pe


def carve(head, tail, gib):
    ph, pe = both_agree(head, tail)
    count, es, last_usable = u32(ph, 80), u32(ph, 84), u64(ph, 48)
    ud = index_of(ph, pe, 'userdata')
    if ud < 0:
        raise Refuse('not one partition named userdata')
    if index_of(ph, pe, 'darwin') != -1:
        raise Refuse('there is a darwin already')
    used = [i for i in range(count) if not empty(entry(ph, pe, i))]
    if any(rng(entry(ph, pe, i))[1] > rng(entry(ph, pe, ud))[1] for i in used) or \
            rng(entry(ph, pe, ud))[1] != last_usable:
        raise Refuse('userdata is not the last partition, reaching to the end of the unit')
    free = [i for i in range(count) if empty(entry(ph, pe, i)) and i > max(used)]
    if not free:
        raise Refuse('no free entry after the used ones')
    first = (last_usable + 1 - gib * 2**30 // BS) // (ALIGN // BS) * (ALIGN // BS)
    table = bytearray(pe)
    struct.pack_into('<Q', table, ud * es + 40, first - 1)
    e = bytearray(es)
    e[:16] = APFS
    e[16:32] = uuid.uuid4().bytes_le
    struct.pack_into('<QQQ', e, 32, first, last_usable, 0)
    e[56:56 + 12] = 'darwin'.encode('utf-16le')
    table[free[0] * es:(free[0] + 1) * es] = e
    table = bytes(table)
    return staged_of(seal(ph, table), table)


def mend(head, tail):
    """the table to write so that both copies are one again: the primary when it checks, else the backup as a primary"""
    ph, pe, p_ok, bh, be, b_ok, disk = copies(head, tail)
    if p_ok:
        return staged_of(ph, pe)
    if b_ok:
        h = bytearray(bh)
        struct.pack_into('<QQ', h, 24, 1, u64(bh, 24))
        struct.pack_into('<Q', h, 72, 2)
        return staged_of(seal(bytes(h), be), be)
    raise Refuse("neither copy of the phone's table checks")


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    cmd, args = argv[1], argv[2:]
    rd = lambda p: open(p, 'rb').read()
    try:
        if cmd == 'info' and len(args) == 2:
            ph, pe, p_ok, bh, be, b_ok, disk = copies(rd(args[0]), rd(args[1]))
            print(f'primary: {"checks" if p_ok else "DOES NOT CHECK"}; backup: {"checks" if b_ok else "DOES NOT CHECK"}; '
                  f'{"the same" if p_ok and b_ok and pe == be else "not the same"}; the unit has {disk} blocks')
            if not (p_ok or b_ok):
                raise Refuse("neither copy of the phone's table checks")
            print(table_line(ph, pe) if p_ok else table_line(bh, be))
            return 0
        if cmd == 'carve' and len(args) == 4:
            head, tail, gib, out = rd(args[0]), rd(args[1]), int(args[2]), args[3]
            staged = carve(head, tail, gib)
        elif cmd == 'restore' and len(args) == 5:
            head, tail, out = rd(args[0]), rd(args[1]), args[4]
            oh, oe = both_agree(rd(args[2]), rd(args[3]))
            if index_of(oh, oe, 'darwin') != -1:
                raise Refuse('the saved table has a darwin: it is not the phone\'s own from before')
            staged = staged_of(oh, oe)
        elif cmd == 'mend' and len(args) == 3:
            head, tail, out = rd(args[0]), rd(args[1]), args[2]
            staged = mend(head, tail)
        elif cmd == 'check' and len(args) == 3:
            head, tail, staged, out = rd(args[0]), rd(args[1]), rd(args[2]), None
        else:
            sys.exit(__doc__)
        c = decide(head, tail, staged)
        say_change(c)
        if out is not None:
            with open(out + '.part', 'wb') as f:
                f.write(staged)
            os.replace(out + '.part', out)
            print(f'staged table: {out} ({len(staged)} bytes)')
        print(table_line(staged[:BS], staged[BS:]))
        return 0
    except Refuse as r:
        print(f'REFUSE: {r}')
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
