#!/usr/bin/env python3
"""gpt-test.py: the rules for a new partition table, tried on a phone's own tables: every case is made here from the
copies saved from the phone (DIR: gpt-head.orig.bin and gpt-tail.orig.bin from before darwin was carved, gpt-head.new and
gpt-tail.new after), and decided twice -- by the loader's own code (xnuloader/gpt.c, built here as tools/gpt-test.c)
and by gpt-plan.py, which install.sh runs before it sends a table -- and both must give the answer each case expects.

usage: gpt-test.py DIR        (the 6T's: out/gpt-backup-20261115)
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import importlib.util  # noqa: E402

spec = importlib.util.spec_from_file_location('gpt_plan', os.path.join(HERE, 'gpt-plan.py'))
gp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gp)
BS = gp.BS


def primary(head):
    h = head[BS:2 * BS]
    return h, head[gp.u64(h, 72) * BS:][:gp.entries_bytes(h)]


def staged(h, e):
    return gp.staged_of(gp.seal(h, e), e)


def with_entry(h, e, i, edit):
    """the table with entry i changed by edit(bytearray of the entry)"""
    es = gp.u32(h, 84)
    t = bytearray(e)
    x = bytearray(t[i * es:(i + 1) * es])
    edit(x)
    t[i * es:(i + 1) * es] = x
    return bytes(t)


def with_header(h, at, fmt, value):
    x = bytearray(h)
    struct.pack_into(fmt, x, at, value)
    return bytes(x)


def broken(head):
    """head with its primary header's CRC wrong"""
    x = bytearray(head)
    x[BS + 16] ^= 0xff
    return bytes(x)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = sys.argv[1]
    rd = lambda n: open(os.path.join(d, n), 'rb').read()
    oh, ot, nh, nt = rd('gpt-head.orig.bin'), rd('gpt-tail.orig.bin'), rd('gpt-head.new'), rd('gpt-tail.new')
    ph_o, pe_o = primary(oh)
    ph_n, pe_n = primary(nh)
    es = gp.u32(ph_n, 84)
    ud = gp.index_of(ph_n, pe_n, 'userdata')
    dw = gp.index_of(ph_n, pe_n, 'darwin')
    last_usable = gp.u64(ph_n, 48)
    u_first = gp.u64(pe_n, ud * es + 32)
    other = next(i for i in range(gp.u32(ph_n, 80)) if i not in (ud, dw) and gp.name_of(gp.entry(ph_n, pe_n, i)))
    free = next(i for i in range(gp.u32(ph_n, 80)) if i > dw and gp.empty(gp.entry(ph_n, pe_n, i))) \
        if any(gp.empty(gp.entry(ph_n, pe_n, i)) for i in range(dw + 1, gp.u32(ph_n, 80))) else None
    new_backup_crc = '%08x' % gp.u32(nt[7 * BS:], 16)
    orig_backup_crc = '%08x' % gp.u32(ot[7 * BS:], 16)

    def set_range(i, first, last):
        def f(x):
            struct.pack_into('<QQ', x, 32, first, last)
        return f

    def rename(name):
        def f(x):
            x[56:128] = bytes(72)
            x[56:56 + 2 * len(name)] = name.encode('utf-16le')
        return f

    def as_darwin(first, last):
        def f(x):
            x[:16] = gp.APFS
            rename('darwin')(x)
            struct.pack_into('<QQ', x, 32, first, last)
        return f

    small_first = (last_usable + 1 - 11 * 2**30 // BS) // 512 * 512
    carved_11 = with_entry(ph_n, with_entry(ph_n, pe_n, ud, set_range(ud, u_first, small_first - 1)), dw,
                           set_range(dw, small_first, last_usable))

    # (name, disk head, disk tail, staged, expect allow, what the result line must hold)
    cases = [
        ('no change', nh, nt, staged(ph_n, pe_n), True, f'same=1 mend=0 backup_crc={new_backup_crc}'),
        ('carve (the table written on 11-15)', oh, ot, staged(ph_n, pe_n), True, f'same=0 mend=0 backup_crc={new_backup_crc}'),
        ('restore (darwin\'s entry empty again)', nh, nt, staged(ph_o, pe_o), True, f'same=0 mend=0 backup_crc={orig_backup_crc}'),
        ('torn: primary new, backup old', nh, ot, staged(ph_n, pe_n), True, 'same=0 mend=1'),
        ('torn: primary old, backup new', oh, nt, staged(ph_n, pe_n), True, 'same=0 mend=1'),
        ('primary broken, backup good, staged = the primary as it was', broken(oh), ot, staged(ph_o, pe_o), True, 'mend=1'),
        ('primary broken, staged = carve', broken(oh), ot, staged(ph_n, pe_n), True, 'mend=1'),
        ('REFUSE another entry\'s range', nh, nt, staged(ph_n, with_entry(ph_n, pe_n, other, set_range(other, 1, 2))), False, ''),
        ('REFUSE another entry renamed darwin', nh, nt,
         staged(ph_n, with_entry(ph_n, with_entry(ph_n, pe_n, dw, lambda x: x.__setitem__(slice(0, es), bytes(es))),
                                 other, as_darwin(22048256, last_usable))), False, ''),
        ('REFUSE two darwin', nh, nt,
         staged(ph_n, with_entry(ph_n, pe_n, free, as_darwin(22048256, last_usable))) if free is not None else None, False, ''),
        ('REFUSE first usable moved', nh, nt, staged(with_header(ph_n, 40, '<Q', gp.u64(ph_n, 40) + 1), pe_n), False, ''),
        ('REFUSE disk GUID changed', nh, nt, staged(with_header(ph_n, 56, '<B', ph_n[56] ^ 1), pe_n), False, ''),
        ('REFUSE primary broken + disk GUID changed', broken(oh), ot,
         staged(with_header(ph_o, 56, '<B', ph_o[56] ^ 1), pe_o), False, ''),
        ('REFUSE primary broken + first usable moved', broken(oh), ot,
         staged(with_header(ph_o, 40, '<Q', gp.u64(ph_o, 40) + 1), pe_o), False, ''),
        ('REFUSE darwin overlapping userdata', nh, nt,
         staged(ph_n, with_entry(ph_n, pe_n, dw, set_range(dw, 22048255, last_usable))), False, ''),
        ('REFUSE darwin of 11 GiB', nh, nt, staged(ph_n, carved_11), False, ''),
        ('REFUSE darwin not on 2 MiB', nh, nt, staged(ph_n, with_entry(ph_n, with_entry(ph_n, pe_n, ud, set_range(
            ud, u_first, 22048256)), dw, set_range(dw, 22048257, last_usable))), False, ''),
        ('REFUSE darwin of another type', nh, nt,
         staged(ph_n, with_entry(ph_n, pe_n, dw, lambda x: x.__setitem__(0, x[0] ^ 1))), False, ''),
        ('REFUSE userdata to last usable + 1', nh, nt, staged(ph_o, with_entry(ph_o, pe_o, ud, set_range(
            ud, u_first, last_usable + 1))), False, ''),
        ('REFUSE userdata ending before its start', nh, nt, staged(ph_n, with_entry(ph_n, pe_n, ud, set_range(
            ud, u_first, u_first - 1))), False, ''),
        ('REFUSE userdata renamed', nh, nt, staged(ph_n, with_entry(ph_n, pe_n, ud, rename('userdat2'))), False, ''),
        ('REFUSE both copies broken', broken(nh), bytes(8 * BS), staged(ph_n, pe_n), False, ''),
        ('REFUSE staged CRC wrong', nh, nt, gp.staged_of(ph_n, with_entry(ph_n, pe_n, dw, set_range(
            dw, 22048256 + 512, last_usable))), False, ''),
    ]
    tool = os.path.join(tempfile.mkdtemp(), 'gpt-test')
    subprocess.run(['cc', '-O1', '-Wall', '-Wextra', '-Werror', '-o', tool, os.path.join(HERE, 'gpt-test.c')], check=True)
    failed = 0
    with tempfile.TemporaryDirectory() as t:
        for n, (name, head, tail, stg, allow, want) in enumerate(cases):
            if stg is None:
                print(f'  skip  {name}: no free entry')
                continue
            paths = []
            for k, b in (('head', head), ('tail', tail), ('staged', stg)):
                p = os.path.join(t, f'{n}.{k}')
                open(p, 'wb').write(b)
                paths.append(p)
            r = subprocess.run([tool] + paths, capture_output=True, text=True)
            c_allow, c_line = r.returncode == 0, r.stdout.strip()
            try:
                gp.decide(head, tail, stg)
                py_allow, py_why = True, ''
            except gp.Refuse as e:
                py_allow, py_why = False, str(e)
            ok = c_allow == allow and py_allow == allow and (not allow or want in c_line) and r.returncode in (0, 1)
            if not allow and ok:
                ok = py_why in c_line or py_why.split(' (')[0] in c_line
            failed += not ok
            print(f'  {"ok  " if ok else "FAIL"}  {name}\n        C: {c_line}\n        py: {"ALLOW" if py_allow else "REFUSE: " + py_why}')
    print(f'{len(cases) - failed} of {len(cases)} cases as expected')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
