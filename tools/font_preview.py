#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Render sample text from generated LVGL font files into a PNG for inspection.

Usage:
    python tools/font_preview.py out.png fontA.c [fontB.c ...]

Why: numbers (glyph count, line_height, ink coverage) cannot tell you whether a
12 px CJK face is actually legible. Source Han Sans derived fonts are known to
look soft at small sizes, so after swapping a typeface it is worth looking at
the real rasterised result rather than trusting the metrics.

The glyphs are read straight out of the generated C arrays, so what you see is
exactly what the device will draw (same bitmaps, same advance widths, same
baseline convention as lv_draw_sw_letter.c:
    top = (line_height - base_line) - box_h - ofs_y
    left = pen_x + ofs_x ,  pen_x += adv_w / 16
"""

import os
import re
import sys

from PIL import Image

SAMPLE = [
    "洛雪下载MP3版",
    "Clean Bandit - Rather Be (Feat. Jess Glynne)",
    "CMJ - 所念皆星河",
    "40mP、初音ミク - だんだん早くなる",
    "逃跑计划 - 再飞行   鑫钰闫阙鸢",
    "正在播放  暂无歌词  正在扫描 36%",
]


class LvglFont(object):
    def __init__(self, path):
        with open(path, 'r', encoding='utf-8', errors='replace') as fp:
            self.txt = fp.read()
        self.bitmap = self._bytes_array('glyph_bitmap')
        self.dsc = self._glyph_dsc()
        self.cmaps = self._cmaps()
        m = re.search(r'\.line_height = (-?\d+)', self.txt)
        self.line_height = int(m.group(1))
        m = re.search(r'\.base_line = (-?\d+)', self.txt)
        self.base_line = int(m.group(1))

    def _bytes_array(self, name):
        m = re.search(re.escape(name) + r'\[\] = \{(.*?)\n\};', self.txt, re.S)
        if not m:
            raise SystemExit('array %s not found' % name)
        return bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(1)))

    def _num_array(self, name, hexfmt=True):
        m = re.search(re.escape(name) + r'\[\] = \{(.*?)\};', self.txt, re.S)
        if not m:
            return []
        body = m.group(1)
        if hexfmt:
            return [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{1,4})', body)]
        return [int(x) for x in re.findall(r'(?<![\w.])(\d+)(?![\w.])', body)]

    def _glyph_dsc(self):
        out = []
        pat = (r'\.bitmap_index = (\d+),\s*\.adv_w = (\d+),\s*\.box_w = (\d+),'
               r'\s*\.box_h = (\d+),\s*\.ofs_x = (-?\d+),\s*\.ofs_y = (-?\d+)')
        for m in re.finditer(pat, self.txt):
            out.append(tuple(int(g) for g in m.groups()))
        return out

    def _cmaps(self):
        out = []
        for m in re.finditer(
                r'\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),\s*'
                r'\.unicode_list = (\w+), \.glyph_id_ofs_list = (\w+), \.list_length = (\d+), '
                r'\.type = (\w+)', self.txt):
            rs, rl, gs, uni, ofs, ll, typ = m.groups()
            entry = {'rs': int(rs), 'gs': int(gs), 'type': typ}
            if uni != 'NULL':
                entry['uni'] = self._num_array(uni, True)
            if ofs != 'NULL':
                entry['ofs'] = self._num_array(ofs, False)
            out.append(entry)
        return out

    def gid(self, cp):
        for c in self.cmaps:
            rcp = cp - c['rs']
            if rcp < 0:
                continue
            if c['type'] == 'LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY':
                if rcp > 4096:      # sanity, range_length not stored here
                    continue
                return c['gs'] + rcp, c
            if c['type'] == 'LV_FONT_FMT_TXT_CMAP_SPARSE_TINY':
                lst = c['uni']
                lo, hi = 0, len(lst) - 1
                while lo <= hi:
                    mid = (lo + hi) // 2
                    if lst[mid] == rcp:
                        return c['gs'] + mid, c
                    if lst[mid] < rcp:
                        lo = mid + 1
                    else:
                        hi = mid - 1
                continue
            if c['type'] == 'LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL':
                lst = c.get('ofs', [])
                if rcp < len(lst):
                    return c['gs'] + lst[rcp], c
                continue
        return 0, None

    def draw_line(self, img, x0, y0, text, value):
        pen = 0
        for ch in text:
            gid, cmap = self.gid(ord(ch))
            if gid <= 0 or gid >= len(self.dsc):
                pen += 8 if ord(ch) > 0x2000 else 6
                continue
            bi, adv, bw, bh, ox, oy = self.dsc[gid]
            if bw > 0 and bh > 0:
                top = (self.line_height - self.base_line) - bh - oy
                left = x0 + pen + ox
                stride = (bw + 1) // 2
                for row in range(bh):
                    base = bi + row * stride
                    if base + stride > len(self.bitmap):
                        break
                    for col in range(bw):
                        b = self.bitmap[base + col // 2]
                        v = (b >> 4) if (col & 1) == 0 else (b & 0x0F)
                        if v:
                            px, py = left + col, y0 + top + row
                            if 0 <= px < img.width and 0 <= py < img.height:
                                img.putpixel((px, py), min(255, v * 17))
            pen += adv // 16
        return pen


def main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    out_path = sys.argv[1]
    fonts = sys.argv[2:]
    loaded = [(os.path.basename(p), LvglFont(p)) for p in fonts]

    scale = 3
    pad = 4
    width = 344
    rows = []
    for name, f in loaded:
        rows.append(('title', '%s   line_height=%d base_line=%d' % (name, f.line_height, f.base_line)))
        for line in SAMPLE:
            rows.append(('text', line, f))

    height = 0
    for r in rows:
        height += (f.line_height + 2) if r[0] == 'text' else 14
    height += pad * 2
    img = Image.new('L', (width, height), 0)
    y = pad
    for r in rows:
        if r[0] == 'title':
            y += 14
        else:
            r[2].draw_line(img, pad, y, r[1], 0)
            y += r[2].line_height + 2
        if y > height - 8:
            break

    img = img.resize((width * scale, height * scale), Image.NEAREST)
    img.save(out_path)
    sys.stdout.write('wrote %s  (%dx%d, %d font(s), scale %dx)\n'
                     % (out_path, img.width, img.height, len(loaded), scale))
    for name, f in loaded:
        sys.stdout.write('  %-34s glyphs=%d line_height=%d base_line=%d\n'
                         % (name, len(f.dsc), f.line_height, f.base_line))
    sys.stdout.write('  order: title line then sample lines\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
