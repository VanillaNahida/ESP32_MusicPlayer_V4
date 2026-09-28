#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Compare the "ink coverage" (apparent weight) of two generated LVGL fonts.

Usage: python tools/font_ink.py <a.c> <b.c> ...

Ink coverage = sum of all glyph bitmap pixel values / (total glyph box pixels),
i.e. how black the text actually looks at this size. Useful when swapping in a
heavier/lighter weight: two fonts can share line_height and size yet look very
different on a small TFT.
"""

import re
import sys


def analyse(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as fp:
        txt = fp.read()

    # glyph_dsc entries: { .bitmap_index = N, .adv_w = N, .box_w = N, .box_h = N, .ofs_x = N, .ofs_y = N }
    boxes = 0
    glyphs = 0
    for m in re.finditer(
            r'\.bitmap_index = \d+,\s*\.adv_w = \d+,\s*\.box_w = (\d+),\s*\.box_h = (\d+)', txt):
        w, h = int(m.group(1)), int(m.group(2))
        if w > 0 and h > 0:
            boxes += w * h
            glyphs += 1

    # glyph_bitmap: 4bpp packed, two pixels per byte
    mb = re.search(r'glyph_bitmap\[\] = \{(.*?)\n\};', txt, re.S)
    ink = 0
    nib = 0
    if mb:
        for m in re.finditer(r'0x([0-9A-Fa-f]{2})', mb.group(1)):
            v = int(m.group(1), 16)
            ink += (v >> 4) + (v & 0x0F)
            nib += 2

    lh = re.search(r'\.line_height = (-?\d+)', txt)
    bl = re.search(r'\.base_line = (-?\d+)', txt)
    return {
        'glyphs': glyphs,
        'boxes': boxes,
        'ink': ink,
        'coverage': (ink / (boxes * 15.0)) if boxes else 0.0,
        'line_height': lh.group(1) if lh else '?',
        'base_line': bl.group(1) if bl else '?',
        'size': len(txt),
    }


def main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    for p in sys.argv[1:]:
        a = analyse(p)
        sys.stdout.write('%-46s glyphs=%-5d line_h=%-3s base=%-3s coverage=%.1f%%  src=%dKB\n'
                         % (p.split('/')[-1].split('\\')[-1], a['glyphs'], a['line_height'],
                            a['base_line'], a['coverage'] * 100.0, a['size'] // 1024))
    return 0


if __name__ == '__main__':
    sys.exit(main())
