#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Extract one font from a .ttc collection into a standalone .ttf.

Usage:
    python tools/ttc_extract.py <in.ttc> <index> <out.ttf>
    python tools/ttc_extract.py <in.ttc> --list        (same as tools/ttc_list.py)

Why: lv_font_conv (opentype.js) cannot read TrueType Collections -- it fails
with "Unsupported OpenType signature ttcf". Collections also have no way to say
"use font N", so the wanted face has to be lifted out into its own file first.

fontTools' TTFont.save() rewrites a correct standalone sfnt (new offset table,
tables copied and re-aligned), so no hand-rolled byte surgery is needed.
"""

import os
import sys

from fontTools.ttLib import TTCollection, TTFont


def name_of(font, name_id):
    nt = font['name'] if 'name' in font else None
    if nt is None:
        return ''
    for rec in nt.names:
        if rec.nameID == name_id:
            try:
                return rec.toUnicode()
            except Exception:
                continue
    return ''


def main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 2

    src = sys.argv[1]
    if sys.argv[2] == '--list':
        for i, f in enumerate(TTCollection(src, lazy=True).fonts):
            sys.stdout.write('[%d] %s %s\n' % (i, name_of(f, 1), name_of(f, 2)))
        return 0

    index = int(sys.argv[2])
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.splitext(src)[0] + '.ttf'

    tc = TTCollection(src, lazy=False)
    if index < 0 or index >= len(tc.fonts):
        sys.stderr.write('index %d out of range (0..%d)\n' % (index, len(tc.fonts) - 1))
        return 1

    font = tc.fonts[index]
    family = name_of(font, 1)
    sub = name_of(font, 2)

    font.save(out)
    tc.close()

    # verify what we just wrote can be re-opened as a plain font
    chk = TTFont(out, lazy=True)
    sig = open(out, 'rb').read(4)
    nchars = len(chk.getBestCmap())
    chk.close()

    sys.stdout.write('extracted index %d (%s %s)\n' % (index, family, sub))
    sys.stdout.write('  -> %s\n' % out)
    sys.stdout.write('  signature : %r (needs b"\\x00\\x01\\x00\\x00" or OTTO)\n' % sig)
    sys.stdout.write('  cmap chars: %d\n' % nchars)
    sys.stdout.write('  size      : %d bytes\n' % os.path.getsize(out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
