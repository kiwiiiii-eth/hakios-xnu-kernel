#!/usr/bin/env python3
"""mkbootscreen.py: the pictures of the boot screen an installed phone shows while the loader reads the system image
(xnuloader/bootscreen.c): the name HakiOS, the lines under the progress bar, the glyphs its numbers are made of, and
the printable ASCII small for the debug screen's log (bootscreen=log), drawn here once from the Inter typeface and
written as xnuloader/bootscreen_assets.h, 8-bit coverage run-length coded.

usage: mkbootscreen.py FONT_DIR [OUT]        (FONT_DIR: Inter's release, https://github.com/rsms/inter: extras/ttf/
                                              Inter-SemiBold.ttf and Inter-Regular.ttf; OUT: xnuloader/bootscreen_assets.h)
Needs Pillow (python3 -m pip install pillow). Inter is under the SIL Open Font License 1.1: the pictures made from it
may go into the loader as they are; the font files themselves are not in this repository.

The sizes are the 6T's screen's (1080 x 2340): the name 128 px, the lines 36 px, the log 26 px.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

LOGO = ('HakiOS', 'Inter-SemiBold.ttf', 128)
LINE_FONT = ('Inter-Regular.ttf', 36)
PHRASES = {
    'LOADING': 'Loading the system',
    'CHECKING': 'Checking the system',
    'STARTING': 'Starting HakiOS',
    'FAILED': 'The system could not be loaded',
}
GLYPHS = '0123456789./ GB\u00b7'           # the middle dot last: "\\x7f" in BS_GLYPHS
LOG_FONT = ('Inter-Regular.ttf', 26)


def rle(alpha):
    """(count, value) byte pairs, count 1..255, over the rows top to bottom"""
    out, run, last = bytearray(), 0, None
    for v in alpha:
        if v == last and run < 255:
            run += 1
        else:
            if last is not None:
                out += bytes((run, last))
            last, run = v, 1
    out += bytes((run, last))
    return bytes(out)


def render(text, font):
    """the text's coverage, cropped: (width, height, left, top, alpha bytes, advance) with left/top from the pen's
    origin at the line's top (ascent included), so pieces drawn at one origin line up"""
    asc, desc = font.getmetrics()
    w = int(font.getlength(text)) + 8
    im = Image.new('L', (w + 16, asc + desc + 16), 0)
    ImageDraw.Draw(im).text((8, 8), text, font=font, fill=255)
    box = im.getbbox()
    if box is None:                                     # a space
        return 0, 0, 0, 0, b'', round(font.getlength(text))
    crop = im.crop(box)
    return crop.width, crop.height, box[0] - 8, box[1] - 8, crop.tobytes(), round(font.getlength(text))


def c_bytes(data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append('\t' + ', '.join(str(b) for b in data[i:i + 24]) + ',')
    return '\n'.join(lines)


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    fonts = sys.argv[1]
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[2] if len(sys.argv) == 3 else os.path.join(here, '..', 'xnuloader', 'bootscreen_assets.h')
    logo_font = ImageFont.truetype(os.path.join(fonts, LOGO[1]), LOGO[2])
    line_font = ImageFont.truetype(os.path.join(fonts, LINE_FONT[0]), LINE_FONT[1])
    pool, parts = bytearray(), []

    def add(name, text, font):
        w, h, left, top, alpha, advance = render(text, font)
        data = rle(alpha) if alpha else b''
        parts.append((name, w, h, left, top, advance, len(pool), len(data)))
        pool.extend(data)

    add('BS_LOGO', LOGO[0], logo_font)
    for name, text in PHRASES.items():
        add('BS_' + name, text, line_font)
    for i, ch in enumerate(GLYPHS):
        add(f'BS_GLYPH_{i}', ch, line_font)
    log_font = ImageFont.truetype(os.path.join(fonts, LOG_FONT[0]), LOG_FONT[1])
    first_ascii = len(parts)
    for c in range(32, 127):
        add(f'BS_ASCII_{c}', chr(c), log_font)
    asc, desc = line_font.getmetrics()
    lasc, ldesc = log_font.getmetrics()
    lines = [
        '/* made by tools/mkbootscreen.py from Inter (SIL Open Font License 1.1, https://github.com/rsms/inter): do not edit */',
        '',
        'struct bs_picture {',
        '\tuint16_t w, h;          /* the coverage\'s size */',
        '\tint16_t left, top;      /* where it begins from the pen at the line\'s top */',
        '\tuint16_t advance;       /* how far the pen moves after it */',
        '\tuint32_t at, bytes;     /* its (count, value) pairs in bs_pool */',
        '};',
        '',
        f'#define BS_LINE_HEIGHT {asc + desc}',
        f'#define BS_LOG_HEIGHT {lasc + ldesc}',
        f'#define BS_ASCII_FIRST {first_ascii}     /* the picture of character c (32..126): BS_ASCII_FIRST + c - 32 */',
        '#define BS_GLYPHS "' + GLYPHS.replace(chr(0xb7), '\\x7f') + '"     /* \\x7f: the middle dot */',
        '',
        'static const struct bs_picture bs_pictures[] = {',
    ]
    for name, w, h, left, top, advance, at, n in parts:
        lines.append(f'\t{{ {w}, {h}, {left}, {top}, {advance}, {at}, {n} }},  /* {name} */')
    lines += ['};', '']
    for i, (name, *_rest) in enumerate(parts):
        if not name.startswith('BS_ASCII_'):
            lines.append(f'#define {name} {i}')
    lines += ['', f'static const uint8_t bs_pool[{len(pool)}] = {{', c_bytes(pool), '};', '']
    with open(out, 'w') as f:
        f.write('\n'.join(lines))
    print(f'{out}: {len(parts)} pictures, {len(pool)} bytes of runs')


if __name__ == '__main__':
    main()
