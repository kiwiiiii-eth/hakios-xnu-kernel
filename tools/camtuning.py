#!/usr/bin/env python3
"""Make a camera's colour tuning, as libhakios.media's processor reads it (userland/src/camera_isp.c,
isp_load_tuning), out of the camera's tuning file of Android's camera stack
(/vendor/lib64/camera/com.qti.tuned.<name>.bin of the phone: the vendor's, kept in fw-camera/ and never in
the repository; so is what this makes of it).

usage: camtuning.py TUNED.bin OUT.isp     (camtuning.py TUNED.bin -   lists what is there only)

The tuning file is a "QTI Chromatix Header" / "Parameter Parser V3.0.1" file (the OnePlus 6T's: the older layout of
its module files, "ParameterParser V1.1.0" or "Chromatix"), laid out as the sensors' module files are (see camsensor.py): a table of symbols { id, name[32], kind, ?, parent, offset, size }
and the data they point into. Two of its modules are taken, each as Chromatix keeps it -- a tree whose
first level is the flash (led_idx_data: 0 without one, the only one taken), the next the scene's light
(aec_data: from a lux index to another, Qualcomm's measure of how dark the scene is: higher is darker),
the last the light's colour temperature (cct_data: from a CCT to another, in kelvin):

  cc13 (the colour matrix, IPE's): per light and temperature a 3x3 matrix from the sensor's colours,
       white balanced, to linear sRGB, by rows R, G, B (each row sums to 1)
  lsc34 (lens shading): per light and temperature a gain for each of R, Gr, Gb and B at each point of
       a 17 x 13 grid laid over the sensor (corners on its corners, rows from the top)

and, of its white balance (Qualcomm's AWB), the reference lights (refPtV1): ten of them for every camera
of this phone, each as the sensor sees a grey under it, red over green and blue over green -- from the
bluest daylight to a candle, in the order 10000 K, D75, D65, D50 (whose gains are the AWB's first ones,
initDataV1), cool white and TL84 fluorescent tubes, U30, A (a filament lamp), horizon, candle. Each is
given the colour temperature that the tables above are picked by for it: its own, but 3800 K for U30, a
3000 K light without the infrared a filament gives -- as a white LED is -- whose tables are TL84's (the
wide camera under a warm white LED: its colour shading measured on a white sheet is TL84's table's to
within 3 %, the 2550-2900 K one's is 12 % off in red).

OUT.isp: "RWCISP02"; the number of the matrices' light ranges and per range { lux index from, to (two
floats), the number of temperature ranges, per range { CCT from, to, the matrix (9 floats) } }; the
same for the lens shading, with per temperature range { CCT from, to, R, Gr, Gb, B grids (17 x 13 floats
each) }; the number of reference lights (0 where there are not the ten) and each { R/G, B/G, CCT }.
Everything little-endian, counts as 32-bit words.
"""
import struct
import sys

GRID = 17 * 13


def layout(data):
    """Where the symbols are: V3.0.1's count at 0xa4, entries at 0xa8 and symbols of 0x38 bytes; the OnePlus 6T's
    (ParameterParser V1.1.0, and its tuning files that name no parser, "Chromatix" only): 0x9c, 0xa0 and 0x40 bytes
    { id, name[32], kind, ?, ?, ?, parent, offset, size } -- None for neither"""
    if not data.startswith(b'QTI Chromatix Header'):
        return None
    if b'Parameter Parser V3.0.1' in data[:0x60]:
        return 0xa4, 0xa8, 0x38
    count, first = struct.unpack_from('<2I', data, 0x9c)
    if 2 <= count <= 8 and first == 0 and (b'ParameterParser V1.1.0' in data[:0x60] or data[0x28:0x32] == b'Chromatix\0'):
        return 0x9c, 0xa0, 0x40
    return None


def symbols(data, where):
    count_at, entries_at, length = where
    count = struct.unpack_from('<I', data, count_at)[0]
    entries = [struct.unpack_from('<3I', data, entries_at + 12 * i) for i in range(count)]
    table, pool = entries[0][1], entries[1][1]
    syms, at = {}, table
    while at + length <= pool:
        sid = struct.unpack_from('<I', data, at)[0]
        name = data[at + 4:at + 0x24].split(b'\0')[0].decode('latin1')
        ref, size = struct.unpack_from('<2i', data, at + length - 8)
        syms[sid] = (name, data[pool + ref:pool + ref + size])
        at += length
    return syms


def module(syms, name, floats):
    """[(lux from, lux to, [(cct from, cct to, values)])] of the module's first tree that does something (the 6T's
    files begin with one of an identity matrix, or gains of 1: a mode's that leaves the module out), without the
    flash"""
    trees = [tree(syms, v, floats) for n, v in syms.values() if n == 'mod_%s_led_idx_data' % name]
    if not trees:
        raise SystemExit('camtuning.py: no %s in the file' % name)
    plain = (1, 0, 0, 0, 1, 0, 0, 0, 1) if floats == 9 else (1,) * floats
    return next((t for t in trees if any(tuple(round(x, 4) for x in v) != plain for _, _, r in t for _, _, v in r)), trees[0])


def tree(syms, led, floats):
    _, regions, sid = next(e for e in struct.iter_unpack('<3I', led) if e[0] == 0)
    out = []
    for i in range(regions):
        lux_from, lux_to, _, _, n, cct_sid = struct.unpack_from('<4f2I', syms[sid][1], 24 * i)
        cct = syms[cct_sid][1]
        size = len(cct) // n
        ranges = []
        for j in range(n):
            v = struct.unpack_from('<%df' % (2 + floats), cct, size * j)
            ranges.append((v[0], v[1], v[2:]))
        out.append((lux_from, lux_to, ranges))
    return out


# the colour temperature each of the ten reference lights picks the tables by (see above)
REFERENCE_CCTS = (10000, 7500, 6500, 5000, 4150, 4000, 3800, 2850, 2300, 1900)


def references(syms):
    """[(R/G, B/G, CCT)] of the AWB's reference lights, or [] where they are not the ten known"""
    v = next((v for n, v in syms.values() if n == 'refPtV1'), None)
    if v is None:
        return []
    f = struct.unpack('<%df' % (len(v) // 4), v)
    points = [(f[i], f[i + 1]) for i in range(6, len(f) - 1, 2) if f[i] > 0 and f[i + 1] > 0]
    if len(points) != len(REFERENCE_CCTS):
        return []
    return [(rg, bg, cct) for (rg, bg), cct in zip(points, REFERENCE_CCTS)]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    where = layout(data)
    if where is None:
        sys.exit('camtuning.py: not a tuning file of the kind known here')
    syms = symbols(data, where)
    ccm = module(syms, 'cc13', 9)
    lsc = module(syms, 'lsc34', 4 * GRID)
    refs = references(syms)

    for what, tree in (('colour matrix', ccm), ('lens shading', lsc)):
        for lux_from, lux_to, ranges in tree:
            for cct_from, cct_to, v in ranges:
                if what == 'colour matrix':
                    shown = ' '.join('%.3f' % x for x in v)
                else:
                    corners = [v[c * GRID] for c in range(4)]
                    shown = 'corner gains R %.2f Gr %.2f Gb %.2f B %.2f' % tuple(corners)
                print('%-13s lux index %4.0f-%-4.0f CCT %5.0f-%-5.0f %s' % (what, lux_from, lux_to, cct_from, cct_to, shown))
    for rg, bg, cct in refs:
        print('reference light R/G %.3f B/G %.3f, tables of %.0f K' % (rg, bg, cct))
    if sys.argv[2] == '-':
        return

    out = bytearray(b'RWCISP02')
    for tree in (ccm, lsc):
        out += struct.pack('<I', len(tree))
        for lux_from, lux_to, ranges in tree:
            out += struct.pack('<2fI', lux_from, lux_to, len(ranges))
            for cct_from, cct_to, v in ranges:
                out += struct.pack('<2f%df' % len(v), cct_from, cct_to, *v)
    out += struct.pack('<I', len(refs))
    for ref in refs:
        out += struct.pack('<3f', *ref)
    open(sys.argv[2], 'wb').write(out)


main()
