#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate the GBK/GB2312 -> Unicode table used by lib/Music/GbkTable.h.

Usage:
    python tools/gen_gbk_table.py

The ESP32 Arduino framework ships no iconv (verified: no iconv_open symbol in
any prebuilt library), so the player has to carry its own mapping table to turn
GBK/GB2312 lyric files into UTF-8.

Layout produced:
    index = (lead - 0x81) * 190 + trail_index(trail)
    lead  : 0x81..0xFE                                   (126 values)
    trail : 0x40..0x7E and 0x80..0xFE, 0x7F excluded      (190 values)
    0 means "undefined sequence"

The output file is pure ASCII (hex literals only), so it is immune to the
ANSI/UTF-8 source-encoding traps that plague CJK-heavy generated files.
"""

import os
import sys

LEADS = list(range(0x81, 0xFF))                       # 126
TRAILS = list(range(0x40, 0x7F)) + list(range(0x80, 0xFF))   # 190
COLS = len(TRAILS)

# Sequences the table generator is checked against before writing anything.
KNOWN = {
    0xC4E3: 0x4F60,   # 你
    0xBAC3: 0x597D,   # 好
    0xD6D0: 0x4E2D,   # 中
    0xB9FA: 0x56FD,   # 国
    0xCCEC: 0x5929,   # 天
    0xC1C1: 0x4EAE,   # 亮
    0xA1A1: 0x3000,   # ideographic space
    0xA3AC: 0xFF0C,   # fullwidth comma
    0xD5E2: 0x8FD9,   # 这
    0xC0EF: 0x91CC,   # 里
}


def build_table():
    table = []
    defined = 0
    for lead in LEADS:
        for trail in TRAILS:
            try:
                text = bytes((lead, trail)).decode('gbk')
                cp = ord(text) if len(text) == 1 else 0
            except UnicodeDecodeError:
                cp = 0
            if cp:
                defined += 1
            table.append(cp)
    return table, defined


def main():
    table, defined = build_table()

    # sanity: every known sequence must be present and correct
    bad = []
    for seq, want in KNOWN.items():
        lead, trail = seq >> 8, seq & 0xFF
        idx = (lead - 0x81) * COLS + TRAILS.index(trail)
        got = table[idx]
        if got != want:
            bad.append((seq, want, got))
    if bad:
        for seq, want, got in bad:
            sys.stderr.write("MISMATCH U+%04X -> got U+%04X want U+%04X\n" % (seq, got, want))
        sys.stderr.write("table self-check failed, not writing\n")
        return 1

    # GB2312 shares its byte ranges with GBK; check how far they diverge.
    # A handful of punctuation positions map to different (but visually almost
    # identical) code points. We deliberately follow GBK / CP936 because that
    # is what Windows "ANSI" actually writes on a Chinese system, and because
    # GBK's choice happens to be the one the embedded font has glyphs for.
    diffs = []
    for lead in range(0xA1, 0xFA):
        for trail in range(0xA1, 0xFF):
            try:
                a = bytes((lead, trail)).decode('gb2312')
                b = bytes((lead, trail)).decode('gbk')
            except UnicodeDecodeError:
                continue
            if a != b:
                diffs.append((lead, trail, ord(a[0]), ord(b[0])))
    for lead, trail, ga, gb in diffs:
        sys.stderr.write("note: %02X%02X gb2312=U+%04X gbk=U+%04X (following GBK)\n"
                         % (lead, trail, ga, gb))
    if len(diffs) > 32:
        sys.stderr.write("%d GB2312/GBK mismatches is too many -> aborting\n" % len(diffs))
        return 1

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, 'lib', 'Music', 'GbkTable.h')

    lines = []
    lines.append('/*')
    lines.append('  GbkTable.h -- GBK/GB2312 -> Unicode mapping table')
    lines.append('')
    lines.append('  GENERATED FILE, DO NOT EDIT BY HAND.')
    lines.append('  Regenerate with:  python tools/gen_gbk_table.py')
    lines.append('')
    lines.append('  Why this exists: the ESP32 Arduino framework ships no iconv,')
    lines.append('  so GBK/GB2312 lyric files have to be mapped by hand.')
    lines.append('')
    lines.append('  Index = (lead - 0x81) * GBK_TRAIL_COLS + gbkTrailIndex(trail)')
    lines.append('  A value of 0 means the byte sequence is undefined.')
    lines.append('')
    lines.append('  GB2312 shares its byte ranges with GBK and differs at only a')
    lines.append('  couple of punctuation positions (0xA1A4, 0xA1AA). This table')
    lines.append('  follows GBK / CP936, because that is what Windows "ANSI" really')
    lines.append('  writes on a Chinese system, and because GBK\'s choice there is')
    lines.append('  the one the embedded font actually has glyphs for.')
    lines.append('*/')
    lines.append('')
    lines.append('#ifndef GBK_TABLE_H')
    lines.append('#define GBK_TABLE_H')
    lines.append('')
    lines.append('#include <stdint.h>')
    lines.append('')
    lines.append('#define GBK_LEAD_FIRST 0x81')
    lines.append('#define GBK_LEAD_LAST  0xFE')
    lines.append('#define GBK_TRAIL_COLS %d' % COLS)
    lines.append('#define GBK_TABLE_SIZE %d' % len(table))
    lines.append('')
    lines.append('/* GBK single byte 0x80 is the euro sign in CP936. */')
    lines.append('#define GBK_EURO 0x20ACu')
    lines.append('')

    # Table first: the inline helpers below dereference it.
    lines.append('static const uint16_t kGbkToUnicode[GBK_TABLE_SIZE] = {')
    per_line = 12
    for i in range(0, len(table), per_line):
        chunk = table[i:i + per_line]
        lead = LEADS[(i // COLS)]
        trail0 = TRAILS[i % COLS]
        lines.append('    /* 0x%02X, 0x%02X */ %s,' % (
            lead, trail0, ', '.join('0x%04X' % v for v in chunk)))
    lines.append('};')
    lines.append('')

    lines.append('/* Convert a trail byte to a column index; -1 when out of range. */')
    lines.append('static inline int gbkTrailIndex(uint8_t trail)')
    lines.append('{')
    lines.append('    if (trail >= 0x40 && trail <= 0x7E) return trail - 0x40;')
    lines.append('    if (trail >= 0x80 && trail <= 0xFE) return trail - 0x80 + (0x7F - 0x40);')
    lines.append('    return -1;')
    lines.append('}')
    lines.append('')
    lines.append('/* Returns the Unicode code point, or 0 when the pair is undefined. */')
    lines.append('static inline uint32_t gbkLookup(uint8_t lead, uint8_t trail)')
    lines.append('{')
    lines.append('    if (lead < GBK_LEAD_FIRST || lead > GBK_LEAD_LAST) return 0;')
    lines.append('    int ti = gbkTrailIndex(trail);')
    lines.append('    if (ti < 0) return 0;')
    lines.append('    return kGbkToUnicode[(lead - GBK_LEAD_FIRST) * GBK_TRAIL_COLS + ti];')
    lines.append('}')
    lines.append('')
    lines.append('#endif /* GBK_TABLE_H */')
    lines.append('')

    with open(out, 'w', encoding='ascii', newline='\n') as fp:
        fp.write('\n'.join(lines))

    sys.stdout.write('wrote %s\n' % out)
    sys.stdout.write('  entries  : %d (%d x %d)\n' % (len(table), len(LEADS), COLS))
    sys.stdout.write('  defined  : %d\n' % defined)
    sys.stdout.write('  table    : %d bytes in flash (uint16)\n' % (len(table) * 2))
    sys.stdout.write('  self-check: %d known sequences OK, GB2312 subset OK\n' % len(KNOWN))
    return 0


if __name__ == '__main__':
    sys.exit(main())
