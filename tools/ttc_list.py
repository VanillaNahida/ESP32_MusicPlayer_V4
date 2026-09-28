#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""List the fonts inside a TrueType Collection (.ttc).

Usage: python tools/ttc_list.py <file.ttc> [report.txt]

Writes a UTF-8 report (font names are CJK, so the report goes to a file rather
than to a console whose code page we cannot control).
"""

import sys
import os
from fontTools.ttLib import TTCollection


def pick(name_table, name_id):
    for rec in name_table.names:
        if rec.nameID == name_id:
            try:
                if rec.platformID == 3:          # Windows: UTF-16BE
                    return rec.toUnicode()
                return rec.toUnicode()
            except Exception:
                continue
    return ''


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.environ.get('TEMP', '.'), 'ttc_list.txt')

    tc = TTCollection(src, lazy=True)
    lines = []
    lines.append('file   : %s' % src)
    lines.append('fonts  : %d' % len(tc.fonts))
    lines.append('')
    for i, f in enumerate(tc.fonts):
        nt = f['name']
        fam = pick(nt, 1)
        sub = pick(nt, 2)
        full = pick(nt, 4)
        ps = pick(nt, 6)
        typo = pick(nt, 16)
        up = f['head'].unitsPerEm
        asc, desc = f['hhea'].ascent, f['hhea'].descent
        os2 = f['OS/2'] if 'OS/2' in f else None
        linegap = f['hhea'].lineGap
        numglyphs = f['maxp'].numGlyphs
        cmap_chars = 0
        try:
            cmap_chars = len(f.getBestCmap())
        except Exception:
            pass
        lines.append('[%d] family=%r subfamily=%r' % (i, fam, sub))
        lines.append('    full=%r  postscript=%r  typoFamily=%r' % (full, ps, typo))
        lines.append('    unitsPerEm=%d  hhea ascent=%d descent=%d lineGap=%d  glyphs=%d  cmapChars=%d'
                     % (up, asc, desc, linegap, numglyphs, cmap_chars))
        lines.append('    -> line_height@12px ~ %.2f px, base_line ~ %.2f px'
                     % ((asc - desc + linegap) * 12.0 / up, (-desc) * 12.0 / up))
        lines.append('')
    tc.close()

    with open(out, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write('\n'.join(lines))
    sys.stdout.write('report written to %s\n' % out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
